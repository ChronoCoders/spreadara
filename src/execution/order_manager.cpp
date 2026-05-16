#include "execution/order_manager.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include "fill_event_generated.h"
#include "infra/cpu_affinity.hpp"
#include "infra/rdtsc.hpp"
#include "quote_update_generated.h"
#include "db/pg_reporter.hpp"
#include "risk/circuit_breaker.hpp"
#include "risk/risk_manager.hpp"

namespace spreadara::execution {

const char* to_str(OrderState s) {
    switch (s) {
        case OrderState::NEW: return "NEW";
        case OrderState::PENDING: return "PENDING";
        case OrderState::SUBMITTED: return "SUBMITTED";
        case OrderState::ACKNOWLEDGED: return "ACKNOWLEDGED";
        case OrderState::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderState::FILLED: return "FILLED";
        case OrderState::CANCELED: return "CANCELED";
        case OrderState::REJECTED: return "REJECTED";
    }
    return "?";
}

bool is_valid_transition(OrderState from, OrderState to) {
    using S = OrderState;
    if (from == to) return false;
    switch (from) {
        case S::NEW:
            // WHY: a freshly defaulted slot can only enter PENDING; everything
            // else (CANCELED, REJECTED, etc.) requires going through a place_new.
            return to == S::PENDING;
        case S::PENDING:
            return to == S::SUBMITTED || to == S::REJECTED;
        case S::SUBMITTED:
            return to == S::ACKNOWLEDGED || to == S::REJECTED || to == S::CANCELED;
        case S::ACKNOWLEDGED:
            return to == S::PARTIALLY_FILLED || to == S::FILLED ||
                   to == S::CANCELED || to == S::REJECTED;
        case S::PARTIALLY_FILLED:
            return to == S::FILLED || to == S::CANCELED;
        case S::FILLED:
        case S::CANCELED:
        case S::REJECTED:
            return false;
    }
    return false;
}

OrderManager::OrderManager(const infra::Config& cfg, IRestClient& rest,
                           risk::PositionTracker& pt, risk::RiskManager& rm,
                           risk::CircuitBreaker& cb, strategy::QuoteRing* quote_ring)
    : cfg_(cfg), rest_(rest), pt_(pt), rm_(rm), cb_(cb), quote_ring_(quote_ring) {
    start_ms_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

OrderManager::~OrderManager() {
    stop();
}

void OrderManager::start() {
    running_.store(true, std::memory_order_release);
    quote_thread_ = std::thread(&OrderManager::quote_loop, this);
    fill_thread_ = std::thread(&OrderManager::fill_apply_loop, this);
    halt_thread_ = std::thread(&OrderManager::halt_watcher_loop, this);
    reconcile_thread_ = std::thread(&OrderManager::reconcile_loop, this);
}

void OrderManager::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (quote_thread_.joinable()) quote_thread_.join();
    if (fill_thread_.joinable()) fill_thread_.join();
    if (halt_thread_.joinable()) halt_thread_.join();
    if (reconcile_thread_.joinable()) reconcile_thread_.join();
}

std::string OrderManager::make_cid() {
    const uint64_t n = cid_counter_.fetch_add(1, std::memory_order_relaxed);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "spr-%llu-%llu",
                  static_cast<unsigned long long>(start_ms_),
                  static_cast<unsigned long long>(n));
    return std::string(buf);
}

bool OrderManager::decode_quote(const strategy::QuoteMsg& msg, double& bid,
                                double& ask, double& qty) const {
    if (msg.size == 0) return false;
    flatbuffers::Verifier v(msg.bytes.data(), msg.size);
    if (!schemas::VerifyQuoteUpdateBuffer(v)) return false;
    auto q = schemas::GetQuoteUpdate(msg.bytes.data());
    bid = q->bid_price();
    ask = q->ask_price();
    qty = q->qty();
    return true;
}

void OrderManager::quote_loop() {
    if (cfg_.runtime.execution_cpu_core >= 0) {
        infra::pin_current_thread_to_core(cfg_.runtime.execution_cpu_core);
    }
    strategy::QuoteMsg msg;
    while (running_.load(std::memory_order_acquire)) {
        if (quote_ring_ && quote_ring_->pop(msg)) {
            double bid = 0.0, ask = 0.0, qty = 0.0;
            if (decode_quote(msg, bid, ask, qty)) {
                on_quote(bid, ask, qty);
            }
        } else {
            // Watchdog on every idle tick: ack-timeout per slot.
            const uint64_t now_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            const uint64_t ack_to_ns =
                static_cast<uint64_t>(cfg_.execution.ack_timeout_ms) * 1'000'000ULL;
            std::lock_guard<std::mutex> lk(mu_);
            for (int i = 0; i < 2; ++i) {
                auto& s = slots_[i];
                if (s.active && s.state == OrderState::SUBMITTED &&
                    s.submit_ts_ns != 0 && (now_ns - s.submit_ts_ns) > ack_to_ns) {
                    spdlog::warn("order_ack_timeout cid={} side={} elapsed_ms={}",
                                 s.client_order_id, static_cast<int>(s.side),
                                 (now_ns - s.submit_ts_ns) / 1'000'000ULL);
                    cancel_slot(i);
                }
            }
            // WHY: don't busy-spin on an empty quote ring; watchdog only
            // needs ~ack_timeout granularity. 10ms keeps wake latency low
            // while freeing the core for other work.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

void OrderManager::on_quote(double bid, double ask, double qty) {
    if (cb_.halted()) {
        spdlog::info("on_quote_dropped reason=cb_halted");
        return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    maybe_requote_side(BID, bid, qty);
    maybe_requote_side(ASK, ask, qty);
}

void OrderManager::for_testing_on_quote(double bid, double ask, double qty) {
    on_quote(bid, ask, qty);
}

void OrderManager::maybe_requote_side(int slot_idx, double new_price, double qty) {
    auto& s = slots_[slot_idx];
    const int8_t side = (slot_idx == BID) ? +1 : -1;
    if (s.active) {
        const bool terminal = (s.state == OrderState::FILLED ||
                               s.state == OrderState::CANCELED ||
                               s.state == OrderState::REJECTED);
        if (terminal) {
            s.active = false;
        } else {
            const double tick = cfg_.strategy.min_tick;
            const double thresh =
                tick * static_cast<double>(cfg_.strategy.price_move_ticks_threshold);
            if (std::fabs(new_price - s.price) <= thresh) return;
            if (!cancel_slot(slot_idx)) return;
        }
    }
    s.side = side;
    place_new(slot_idx, new_price, qty);
}

bool OrderManager::place_new(int slot_idx, double price, double qty) {
    auto& s = slots_[slot_idx];
    s.client_order_id = make_cid();
    s.price = price;
    s.qty = qty;
    s.executed_qty = 0.0;
    s.active = true;
    s.submit_ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // WHY: route NEW -> PENDING through transition() so the audit log captures
    // the initial state too. Slots that have already cycled through a terminal
    // state are reset back to NEW here so the next placement starts clean.
    if (s.state != OrderState::NEW) s.state = OrderState::NEW;
    transition(slot_idx, OrderState::PENDING);
    transition(slot_idx, OrderState::SUBMITTED);
    const char* side_str = (s.side > 0) ? "BUY" : "SELL";
    OrderAck a = rest_.place_order(side_str, qty, price, true, s.client_order_id);
    if (a.ok) {
        s.exchange_order_id = a.exchange_order_id;
        transition(slot_idx, OrderState::ACKNOWLEDGED);
        sync_open_orders();
        return true;
    }
    transition(slot_idx, OrderState::REJECTED);
    sync_open_orders();
    return false;
}

bool OrderManager::cancel_slot(int slot_idx) {
    auto& s = slots_[slot_idx];
    if (!s.active) return true;
    CancelAck c = rest_.cancel_order(s.client_order_id, s.exchange_order_id);
    if (c.ok) {
        transition(slot_idx, OrderState::CANCELED);
        s.active = false;
        sync_open_orders();
        return true;
    }
    spdlog::warn("cancel_failed cid={} http={} binance_code={}",
                 s.client_order_id, c.http_code, c.binance_code);
    return false;
}

void OrderManager::transition(int slot_idx, OrderState to) {
    auto& s = slots_[slot_idx];
    if (!is_valid_transition(s.state, to)) {
        spdlog::warn("invalid_order_transition cid={} from={} to={}",
                     s.client_order_id, to_str(s.state), to_str(to));
        return;
    }
    spdlog::info("order_state cid={} side={} from={} to={} price={:.8f} qty={:.8f} tsc_cycles={}",
                 s.client_order_id, static_cast<int>(s.side),
                 to_str(s.state), to_str(to), s.price, s.qty,
                 infra::rdtsc_cycles());
    s.state = to;
}

bool OrderManager::for_testing_state_transition(int slot_idx, OrderState to) {
    auto& s = slots_[slot_idx];
    if (!is_valid_transition(s.state, to)) return false;
    s.state = to;
    return true;
}

void OrderManager::sync_open_orders() {
    int n = 0;
    for (auto& s : slots_) {
        if (s.active && s.state != OrderState::CANCELED &&
            s.state != OrderState::FILLED && s.state != OrderState::REJECTED) {
            ++n;
        }
    }
    rm_.set_open_order_count(n);
}

bool OrderManager::inject_fill(const risk::FillInput& f) {
    // WHY: report fill to Postgres before applying — drop on full ring, never block.
    if (reporter_) {
        db::DbEvent ev{};
        ev.kind = db::DbEventKind::Trade;
        std::snprintf(ev.trade.order_id, sizeof(ev.trade.order_id), "%s", f.order_id.c_str());
        ev.trade.side = static_cast<int8_t>(f.side);
        ev.trade.price = f.price;
        ev.trade.qty = f.qty;
        ev.trade.fee = f.fee;
        std::snprintf(ev.trade.fee_asset, sizeof(ev.trade.fee_asset), "%s", f.fee_asset.c_str());
        ev.trade.ts_ns = f.timestamp_ns;
        if (!reporter_->push(ev)) {
            spdlog::warn("db_ring_full kind=trade order_id={}", f.order_id);
        }
    }
    std::lock_guard<std::mutex> lk(mu_);
    flatbuffers::FlatBufferBuilder fbb(128);
    auto oid = fbb.CreateString(f.order_id);
    auto sym = fbb.CreateString(f.symbol);
    auto fa = fbb.CreateString(f.fee_asset);
    auto ev = schemas::CreateFillEvent(fbb, oid, sym, f.side, f.price, f.qty,
                                       f.fee, fa, f.timestamp_ns);
    fbb.Finish(ev);

    // Update the matching slot's executed qty + state.
    for (auto& s : slots_) {
        if (s.active && s.client_order_id == f.order_id) {
            s.executed_qty += f.qty;
            const bool full = std::fabs(s.executed_qty - s.qty) < 1e-12 ||
                              s.executed_qty >= s.qty;
            if (full) {
                if (s.state == OrderState::ACKNOWLEDGED ||
                    s.state == OrderState::PARTIALLY_FILLED) {
                    if (s.state == OrderState::ACKNOWLEDGED) {
                        // PARTIALLY_FILLED -> FILLED requires PF first; jump via PF.
                        // ACKNOWLEDGED -> FILLED is allowed by is_valid_transition.
                    }
                    int idx = static_cast<int>(&s - slots_);
                    transition(idx, OrderState::FILLED);
                    s.active = false;
                    sync_open_orders();
                }
            } else {
                int idx = static_cast<int>(&s - slots_);
                if (s.state == OrderState::ACKNOWLEDGED) {
                    transition(idx, OrderState::PARTIALLY_FILLED);
                }
            }
            break;
        }
    }

    FillMsg m;
    if (fbb.GetSize() > m.bytes.size()) return false;
    m.size = static_cast<uint16_t>(fbb.GetSize());
    std::memcpy(m.bytes.data(), fbb.GetBufferPointer(), fbb.GetSize());
    return fill_ring_.push(m);
}

void OrderManager::fill_apply_loop() {
    FillMsg m;
    while (running_.load(std::memory_order_acquire)) {
        if (fill_ring_.pop(m)) {
            flatbuffers::Verifier v(m.bytes.data(), m.size);
            if (!schemas::VerifyFillEventBuffer(v)) continue;
            auto e = schemas::GetFillEvent(m.bytes.data());
            risk::FillInput f;
            if (e->order_id()) f.order_id = e->order_id()->str();
            if (e->symbol()) f.symbol = e->symbol()->str();
            f.side = e->side();
            f.price = e->price();
            f.qty = e->qty();
            f.fee = e->fee();
            if (e->fee_asset()) f.fee_asset = e->fee_asset()->str();
            f.timestamp_ns = e->timestamp_ns();
            pt_.apply_fill(f);
        } else {
            std::this_thread::yield();
        }
    }
}

void OrderManager::cancel_and_flatten_locked(int& cancelled_out, bool& flattened_out) {
    cancelled_out = 0;
    flattened_out = false;
    for (int i = 0; i < 2; ++i) {
        auto& s = slots_[i];
        if (s.active && s.state != OrderState::CANCELED &&
            s.state != OrderState::FILLED && s.state != OrderState::REJECTED) {
            if (cancel_slot(i)) ++cancelled_out;
        }
    }
    const double inv = pt_.current_inventory();
    if (std::fabs(inv) > cfg_.execution.flatten_threshold) {
        const char* side = inv > 0 ? "SELL" : "BUY";
        const double qty = std::fabs(inv);
        spdlog::critical("flatten_market_order side={} qty={:.8f}", side, qty);
        const auto a = rest_.place_market_order(side, qty, make_cid());
        flattened_out = a.ok;
    }
}

void OrderManager::on_halt() {
    std::lock_guard<std::mutex> lk(mu_);
    spdlog::critical("on_halt invoked cancelling_outstanding");
    int cancelled = 0;
    bool flattened = false;
    cancel_and_flatten_locked(cancelled, flattened);
}

void OrderManager::shutdown_cancel_all() {
    // WHY: safe to call before start() — slots default to inactive/NEW and
    // the helper is a pure cancel/flatten over slot state. Shutdown is NOT a
    // fault; we don't call cb_.notify_exception here.
    std::lock_guard<std::mutex> lk(mu_);
    int cancelled = 0;
    bool flattened = false;
    cancel_and_flatten_locked(cancelled, flattened);
    spdlog::info("shutdown_cancel_all_complete cancelled={} flattened={}",
                 cancelled, flattened);
}

void OrderManager::halt_watcher_loop() {
    while (running_.load(std::memory_order_acquire)) {
        if (cb_.halted() && !halt_handled_.exchange(true, std::memory_order_acq_rel)) {
            on_halt();
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg_.risk.circuit_breaker_poll_ms));
    }
}

void OrderManager::for_testing_reconcile(const PositionsSnapshot& pos,
                                          const OpenOrdersSnapshot& oo) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pos.ok) {
        double exch = 0.0;
        for (auto& e : pos.positions) {
            if (e.symbol == cfg_.market_data.symbol) { exch = e.position_amt; break; }
        }
        const double local = pt_.current_inventory();
        const double diff = std::fabs(local - exch);
        if (diff > cfg_.execution.position_divergence_tolerance) {
            spdlog::critical("position_divergence local={:.8f} exchange={:.8f} tolerance={:.8f}",
                             local, exch, cfg_.execution.position_divergence_tolerance);
            cb_.notify_exception("position_divergence");
        }
    }
    (void)oo;
}

void OrderManager::reconcile_now() {
    std::lock_guard<std::mutex> lk(mu_);
    auto pos = rest_.query_positions();
    auto oo = rest_.query_open_orders();
    if (pos.ok) {
        double exch = 0.0;
        for (auto& e : pos.positions) {
            if (e.symbol == cfg_.market_data.symbol) {
                exch = e.position_amt;
                break;
            }
        }
        const double local = pt_.current_inventory();
        const double diff = std::fabs(local - exch);
        if (diff > cfg_.execution.position_divergence_tolerance) {
            spdlog::critical("position_divergence local={:.8f} exchange={:.8f} tolerance={:.8f}",
                             local, exch, cfg_.execution.position_divergence_tolerance);
            cb_.notify_exception("position_divergence");
        }
    }
    if (oo.ok) {
        for (auto& e : oo.orders) {
            bool found = false;
            for (auto& s : slots_) {
                if (s.active && s.exchange_order_id == e.exchange_order_id) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                spdlog::warn("reconcile_unknown_order_at_exchange order_id={} cid={}",
                             e.exchange_order_id, e.client_order_id);
            }
        }
        for (auto& s : slots_) {
            if (!s.active) continue;
            if (s.state != OrderState::ACKNOWLEDGED &&
                s.state != OrderState::PARTIALLY_FILLED) continue;
            bool found = false;
            for (auto& e : oo.orders) {
                if (e.exchange_order_id == s.exchange_order_id) { found = true; break; }
            }
            if (!found) {
                spdlog::warn("reconcile_local_only cid={} state={}",
                             s.client_order_id, to_str(s.state));
            }
        }
    }
}

void OrderManager::reconcile_loop() {
    reconcile_now();
    auto next = std::chrono::steady_clock::now() +
                std::chrono::seconds(cfg_.execution.reconcile_interval_seconds);
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (std::chrono::steady_clock::now() >= next) {
            reconcile_now();
            next = std::chrono::steady_clock::now() +
                   std::chrono::seconds(cfg_.execution.reconcile_interval_seconds);
        }
    }
}

}
