#include "risk/circuit_breaker.hpp"

#include "db/pg_reporter.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include "infra/cpu_affinity.hpp"
#include "risk_event_generated.h"

namespace spreadara::risk {

CircuitBreaker::CircuitBreaker(const infra::Config& cfg,
                               PositionTracker& pt,
                               RiskManager& rm,
                               RiskEventRing* out_ring)
    : cfg_(cfg), pt_(pt), rm_(rm), out_ring_(out_ring) {}

CircuitBreaker::~CircuitBreaker() {
    stop();
}

void CircuitBreaker::start() {
    if (running_.exchange(true)) return;
    monitor_thread_ = std::thread([this] { monitor_loop(); });
}

void CircuitBreaker::stop() {
    if (!running_.exchange(false)) return;
    if (monitor_thread_.joinable()) monitor_thread_.join();
}

void CircuitBreaker::monitor_loop() {
    if (cfg_.runtime.risk_cpu_core >= 0) {
        spreadara::infra::pin_current_thread_to_core(cfg_.runtime.risk_cpu_core);
    }
    const auto period = std::chrono::milliseconds(cfg_.risk.circuit_breaker_poll_ms);
    while (running_.load(std::memory_order_acquire)) {
        do_tick(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(period);
    }
}

static uint64_t now_ns_wall() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

void CircuitBreaker::do_tick(std::chrono::steady_clock::time_point now) {
    // Drawdown — latch on cross, clear when condition recedes.
    const double eq = pt_.equity();
    if (eq > peak_equity_) peak_equity_ = eq;
    if (peak_equity_ > 0.0) {
        const double dd_pct = (peak_equity_ - eq) / peak_equity_ * 100.0;
        if (dd_pct > cfg_.risk.max_drawdown_pct) {
            if (!dd_active_) {
                dd_active_ = true;
                set_halt_synchronously("drawdown",
                    "peak=" + std::to_string(peak_equity_) + " current=" + std::to_string(eq),
                    now_ns_wall());
                enqueue("drawdown",
                    "peak=" + std::to_string(peak_equity_) + " current=" + std::to_string(eq),
                    now_ns_wall());
            }
        } else {
            dd_active_ = false;
        }
    }

    // Unhedged duration — latch on cross, clear when inv returns to ~0.
    // WHY: float accumulation across fills can leave residuals like 1e-18 even
    // after a position fully nets out. Treat anything below 1e-6 contracts as
    // flat so the timer doesn't latch on noise that the operator can never
    // clear.
    constexpr double kInventoryEpsilon = 1e-6;
    const double inv = pt_.current_inventory();
    if (std::abs(inv) > kInventoryEpsilon) {
        if (!inv_started_) {
            inv_started_ = true;
            inv_started_at_ = now;
        } else {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - inv_started_at_).count();
            const auto limit_ms = static_cast<int64_t>(cfg_.risk.max_unhedged_seconds) * 1000;
            if (elapsed_ms > limit_ms && !unhedged_active_) {
                unhedged_active_ = true;
                const std::string detail = "inv=" + std::to_string(inv) +
                                           " ms=" + std::to_string(elapsed_ms);
                set_halt_synchronously("unhedged", detail, now_ns_wall());
                enqueue("unhedged", detail, now_ns_wall());
            }
        }
    } else {
        inv_started_ = false;
        unhedged_active_ = false;
    }

    // Consecutive rejections — latch on cross, clear when count recedes.
    const int rej = rm_.consecutive_rejections_in_window(now);
    if (rej > cfg_.risk.max_consecutive_rejections) {
        if (!rej_active_) {
            rej_active_ = true;
            const std::string detail = "count=" + std::to_string(rej);
            set_halt_synchronously("consecutive_rejections", detail, now_ns_wall());
            enqueue("consecutive_rejections", detail, now_ns_wall());
        }
    } else {
        rej_active_ = false;
    }

    publish_pending();
}

void CircuitBreaker::notify_ws_disconnect_exhausted() {
    const uint64_t ts = now_ns_wall();
    set_halt_synchronously("ws_disconnect", "reconnect_exhausted", ts);
    enqueue("ws_disconnect", "reconnect_exhausted", ts);
}

void CircuitBreaker::notify_exception(const std::string& detail) {
    const uint64_t ts = now_ns_wall();
    set_halt_synchronously("exception", detail, ts);
    enqueue("exception", detail, ts);
}

void CircuitBreaker::set_halt_synchronously(const std::string& trig,
                                            const std::string& detail,
                                            uint64_t ts_ns) {
    bool first = false;
    if (!halted_.exchange(true, std::memory_order_acq_rel)) {
        halted_at_ns_.store(ts_ns, std::memory_order_release);
        first = true;
    }
    spdlog::critical("circuit_breaker_trigger first={} trigger={} detail={}", first, trig, detail);
    if (reporter_) {
        db::DbEvent ev{};
        ev.kind = db::DbEventKind::SystemEvent;
        ev.evt.ts_ns = ts_ns;
        std::snprintf(ev.evt.severity, sizeof(ev.evt.severity), "critical");
        std::snprintf(ev.evt.source, sizeof(ev.evt.source), "circuit_breaker");
        std::snprintf(ev.evt.code, sizeof(ev.evt.code), "%s", trig.c_str());
        std::snprintf(ev.evt.msg, sizeof(ev.evt.msg), "%s", detail.c_str());
        if (!reporter_->push(ev)) {
            spdlog::warn("db_ring_full kind=system_event source=circuit_breaker");
        }
    }
}

void CircuitBreaker::enqueue(const std::string& trig, const std::string& detail, uint64_t ts_ns) {
    std::lock_guard<std::mutex> lk(pending_mu_);
    if (pending_.size() < kPendingCap) {
        pending_.push_back({trig, detail, ts_ns});
    } else {
        spdlog::warn("circuit_breaker_pending_full dropping trigger={}", trig);
    }
}

void CircuitBreaker::publish_pending() {
    std::vector<PendingTrigger> local;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        local.swap(pending_);
    }
    if (!out_ring_ || local.empty()) return;

    for (const auto& p : local) {
        flatbuffers::FlatBufferBuilder fbb(256);
        auto event_type = fbb.CreateString("halt");
        auto trig_s = fbb.CreateString(p.trig);
        auto detail_s = fbb.CreateString(p.detail);
        schemas::RiskEventBuilder b(fbb);
        b.add_event_type(event_type);
        b.add_trigger(trig_s);
        b.add_detail(detail_s);
        b.add_timestamp_ns(p.ts_ns);
        fbb.Finish(b.Finish());

        RiskEventMsg msg;
        const auto sz = fbb.GetSize();
        if (sz <= msg.bytes.size()) {
            msg.size = static_cast<uint16_t>(sz);
            std::memcpy(msg.bytes.data(), fbb.GetBufferPointer(), sz);
            if (!out_ring_->push(msg)) {
                spdlog::warn("risk_event_ring_full trigger={}", p.trig);
            }
        } else {
            spdlog::warn("risk_event_oversize bytes={}", sz);
        }
    }
}

}
