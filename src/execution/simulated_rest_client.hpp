// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "execution/i_rest_client.hpp"
#include "infra/config.hpp"
#include "risk/position_tracker.hpp"

namespace spreadara::execution {

// WHY: in-process fill simulator for backtests. Holds the latest market
// snapshot via update_market(); on each update walks pending orders and emits
// fills via the on_fill callback when the opposite side crosses the order.
// Single-threaded: callers must serialize update_market / place / cancel.
class SimulatedRestClient final : public IRestClient {
public:
    SimulatedRestClient(const infra::Config& cfg, const std::string& symbol);

    SimulatedRestClient(const SimulatedRestClient&) = delete;
    SimulatedRestClient& operator=(const SimulatedRestClient&) = delete;

    OrderAck place_order(const std::string& side, double qty, double price,
                         bool post_only, const std::string& client_order_id) override;
    OrderAck place_market_order(const std::string& side, double qty,
                                const std::string& client_order_id) override;
    CancelAck cancel_order(const std::string& client_order_id,
                           int64_t exchange_order_id) override;
    AmendAck amend_order(int64_t exchange_order_id, double new_price, double new_qty,
                         const std::string& side, bool post_only,
                         const std::string& replacement_client_order_id) override;
    PositionsSnapshot query_positions() override;
    OpenOrdersSnapshot query_open_orders() override;

    // Feed the next market state. Triggers fill scanning over pending_ before
    // returning. Depths are summed over levels 1..5 if provided; otherwise the
    // best-level qty is used as the available counter-side liquidity.
    void update_market(double best_bid, double best_ask,
                       double best_bid_qty, double best_ask_qty,
                       uint64_t timestamp_ns);

    using OnFill = std::function<void(const risk::FillInput&)>;
    void set_on_fill(OnFill cb) { on_fill_ = std::move(cb); }

    std::size_t pending_count() const { return pending_.size(); }
    std::size_t fill_count() const { return fill_count_; }

private:
    struct Pending {
        int64_t exchange_oid;
        std::string symbol;
        int8_t side;  // +1 BUY, -1 SELL
        double qty;
        double price;
        double executed;
    };

    void scan_fills();
    // WHY: shared by scan_fills and place_market_order so flip semantics
    // (close + open residual at fill price) are handled identically.
    void apply_inv_fill(double signed_qty, double fill_price);
    int64_t next_oid() { return ++oid_seq_; }

    const infra::Config& cfg_;
    std::string symbol_;
    std::unordered_map<std::string, Pending> pending_;
    OnFill on_fill_;
    int64_t oid_seq_{1000};
    std::size_t fill_count_{0};

    // Last market snapshot (set via update_market).
    double last_bid_{0.0};
    double last_ask_{0.0};
    double last_bid_qty_{0.0};
    double last_ask_qty_{0.0};
    uint64_t last_ts_ns_{0};

    // Tracks net inventory + accumulated entry for synthetic position queries.
    double inv_{0.0};
    double entry_{0.0};
};

}
