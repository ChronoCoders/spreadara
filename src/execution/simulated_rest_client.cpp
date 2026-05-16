#include "execution/simulated_rest_client.hpp"

#include <algorithm>
#include <cmath>

#include <spdlog/spdlog.h>

namespace spreadara::execution {

SimulatedRestClient::SimulatedRestClient(const infra::Config& cfg, const std::string& symbol)
    : cfg_(cfg), symbol_(symbol) {
    (void)cfg_;
}

OrderAck SimulatedRestClient::place_order(const std::string& side, double qty, double price,
                                          bool /*post_only*/, const std::string& cid) {
    OrderAck a{};
    a.ok = true;
    a.exchange_order_id = next_oid();
    a.client_order_id = cid;
    a.status = "NEW";
    a.price = price;
    a.qty = qty;
    a.http_code = 200;

    Pending p;
    p.exchange_oid = a.exchange_order_id;
    p.symbol = symbol_;
    p.side = (side == "BUY") ? +1 : -1;
    p.qty = qty;
    p.price = price;
    p.executed = 0.0;
    pending_[cid] = p;
    // WHY: a new resting order might already be marketable against the last
    // recorded book; scan immediately so the integration test sees fills even
    // when no further market updates happen before query.
    scan_fills();
    return a;
}

OrderAck SimulatedRestClient::place_market_order(const std::string& side, double qty,
                                                 const std::string& cid) {
    // WHY: simulate an immediate fill at the opposite best.
    OrderAck a{};
    a.exchange_order_id = next_oid();
    a.client_order_id = cid;
    const double px = (side == "BUY") ? last_ask_ : last_bid_;
    if (px <= 0.0 || qty <= 0.0) {
        a.ok = false;
        a.http_code = 400;
        return a;
    }
    a.ok = true;
    a.status = "FILLED";
    a.price = px;
    a.qty = qty;
    a.http_code = 200;

    if (on_fill_) {
        risk::FillInput f;
        f.order_id = cid;
        f.symbol = symbol_;
        f.side = (side == "BUY") ? +1 : -1;
        f.price = px;
        f.qty = qty;
        f.fee = 0.0;
        f.fee_asset = "USDT";
        f.timestamp_ns = last_ts_ns_;
        on_fill_(f);
    }
    apply_inv_fill((side == "BUY") ? qty : -qty, px);
    ++fill_count_;
    return a;
}

CancelAck SimulatedRestClient::cancel_order(const std::string& cid, int64_t /*oid*/) {
    CancelAck c{};
    auto it = pending_.find(cid);
    if (it != pending_.end()) {
        c.ok = true;
        c.exchange_order_id = it->second.exchange_oid;
        c.client_order_id = cid;
        c.status = "CANCELED";
        c.http_code = 200;
        pending_.erase(it);
    } else {
        // WHY: idempotent — already filled/cancelled is success for the caller.
        c.ok = true;
        c.client_order_id = cid;
        c.status = "CANCELED";
        c.http_code = 200;
    }
    return c;
}

AmendAck SimulatedRestClient::amend_order(int64_t /*oid*/, double new_price, double new_qty,
                                          const std::string& side, bool /*post_only*/,
                                          const std::string& replacement_cid) {
    AmendAck a{};
    a.ok = true;
    a.exchange_order_id = next_oid();
    a.client_order_id = replacement_cid;
    a.price = new_price;
    a.qty = new_qty;
    a.http_code = 200;
    Pending p;
    p.exchange_oid = a.exchange_order_id;
    p.symbol = symbol_;
    p.side = (side == "BUY") ? +1 : -1;
    p.qty = new_qty;
    p.price = new_price;
    p.executed = 0.0;
    pending_[replacement_cid] = p;
    scan_fills();
    return a;
}

PositionsSnapshot SimulatedRestClient::query_positions() {
    PositionsSnapshot s{};
    s.ok = true;
    s.http_code = 200;
    PositionEntry e;
    e.symbol = symbol_;
    e.position_amt = inv_;
    e.entry_price = (inv_ != 0.0) ? entry_ : 0.0;
    s.positions.push_back(e);
    return s;
}

OpenOrdersSnapshot SimulatedRestClient::query_open_orders() {
    OpenOrdersSnapshot s{};
    s.ok = true;
    s.http_code = 200;
    for (auto& kv : pending_) {
        OpenOrderEntry e;
        e.exchange_order_id = kv.second.exchange_oid;
        e.client_order_id = kv.first;
        e.symbol = kv.second.symbol;
        e.side = kv.second.side > 0 ? "BUY" : "SELL";
        e.price = kv.second.price;
        e.orig_qty = kv.second.qty;
        e.executed_qty = kv.second.executed;
        e.status = "NEW";
        s.orders.push_back(e);
    }
    return s;
}

void SimulatedRestClient::update_market(double best_bid, double best_ask,
                                        double best_bid_qty, double best_ask_qty,
                                        uint64_t ts_ns) {
    last_bid_ = best_bid;
    last_ask_ = best_ask;
    last_bid_qty_ = best_bid_qty;
    last_ask_qty_ = best_ask_qty;
    last_ts_ns_ = ts_ns;
    scan_fills();
}

void SimulatedRestClient::apply_inv_fill(double signed_qty, double fill_price) {
    // WHY: handles add, partial reduce, full close, AND flip. On flip, entry
    // becomes fill_price for the residual side; add-same-side uses weighted
    // average; reduce keeps entry; full-close leaves entry stale (unused
    // because query_positions returns 0 for entry when inv == 0).
    const double new_inv = inv_ + signed_qty;
    constexpr double kEps = 1e-12;
    if (std::fabs(inv_) < kEps) {
        entry_ = fill_price;
    } else if ((inv_ > 0.0 && signed_qty > 0.0) ||
               (inv_ < 0.0 && signed_qty < 0.0)) {
        const double abs_inv = std::fabs(inv_);
        const double abs_add = std::fabs(signed_qty);
        const double total = abs_inv + abs_add;
        entry_ = (total > 0.0) ? (abs_inv * entry_ + abs_add * fill_price) / total
                               : entry_;
    } else if (std::fabs(new_inv) >= kEps &&
               ((inv_ > 0.0 && new_inv < 0.0) ||
                (inv_ < 0.0 && new_inv > 0.0))) {
        entry_ = fill_price;
    }
    inv_ = new_inv;
}

void SimulatedRestClient::scan_fills() {
    if (last_bid_ <= 0.0 || last_ask_ <= 0.0) return;
    // WHY: snapshot cids before iterating. on_fill_ may re-enter place/cancel
    // via OrderManager side-effects, which can insert into pending_ and
    // invalidate iterators on rehash. Snapshot + per-step lookup is robust to
    // both insertion and erase mid-iteration.
    std::vector<std::string> cids;
    cids.reserve(pending_.size());
    for (auto& kv : pending_) cids.push_back(kv.first);

    std::vector<std::string> done;
    done.reserve(cids.size());
    for (const auto& cid : cids) {
        auto it = pending_.find(cid);
        if (it == pending_.end()) continue;
        Pending& p = it->second;
        const double remaining = p.qty - p.executed;
        if (remaining <= 0.0) { done.push_back(cid); continue; }
        bool crossed = false;
        double avail = 0.0;
        if (p.side > 0) {
            // BUY: taker sells at our bid when last_ask_ drops onto/below us.
            if (last_ask_ <= p.price) { crossed = true; avail = last_ask_qty_; }
        } else {
            if (last_bid_ >= p.price) { crossed = true; avail = last_bid_qty_; }
        }
        if (!crossed) continue;
        const double fill_qty = std::min(remaining, std::max(avail, 0.0));
        if (fill_qty <= 0.0) continue;
        p.executed += fill_qty;
        ++fill_count_;
        apply_inv_fill((p.side > 0) ? fill_qty : -fill_qty, p.price);
        if (on_fill_) {
            risk::FillInput f;
            f.order_id = cid;
            f.symbol = p.symbol;
            f.side = p.side;
            f.price = p.price;
            f.qty = fill_qty;
            f.fee = 0.0;
            f.fee_asset = "USDT";
            f.timestamp_ns = last_ts_ns_;
            on_fill_(f);
        }
        // Re-resolve iterator after on_fill_ (may have rehashed pending_).
        auto it2 = pending_.find(cid);
        if (it2 != pending_.end() && it2->second.executed >= it2->second.qty - 1e-12) {
            done.push_back(cid);
        }
    }
    for (auto& cid : done) pending_.erase(cid);
}

}
