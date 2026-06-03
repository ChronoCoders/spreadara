// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "market_data/market_event.hpp"

namespace spreadara::market_data {

// Concurrency contract: single writer (TickProcessor's consumer thread
// calling apply_snapshot / apply_partial_update), multiple readers
// (tick_processor itself, plus the 1 Hz snap_thread in main.cpp that pulls
// best_bid/best_ask/spread/depth into the dashboard telemetry row). Reads
// of distinct fields are NOT atomic as a group — a reader may observe a
// transient mix of old/new bids_[0].price and bid_count_ during an
// apply_partial_update. Acceptable here: snap_thread runs at 1 Hz for
// dashboard display, not P&L truth. Strict consistency would require a
// seqlock; intentionally not done given the µs-scale write windows.
class OrderBook {
public:
    static constexpr std::size_t kMaxLevels = 20;

    void apply_snapshot(const std::array<PriceLevel, kMaxLevels>& bids, std::size_t bid_count,
                        const std::array<PriceLevel, kMaxLevels>& asks, std::size_t ask_count,
                        uint64_t last_update_id);

    bool apply_partial_update(const DepthEvent& ev);

    bool has_data() const { return bid_count_ > 0 && ask_count_ > 0; }
    double best_bid() const;
    double best_ask() const;
    double mid() const;
    double spread_bps() const;
    double depth_sum(bool bid_side, std::size_t levels) const;

    bool needs_resync() const { return needs_resync_; }
    void clear_resync() { needs_resync_ = false; }
    void mark_resync() { needs_resync_ = true; }

    uint64_t last_update_id() const { return last_update_id_; }

private:
    std::array<PriceLevel, kMaxLevels> bids_{};
    std::array<PriceLevel, kMaxLevels> asks_{};
    std::size_t bid_count_{0};
    std::size_t ask_count_{0};
    uint64_t last_update_id_{0};
    bool initialized_{false};
    bool needs_resync_{true};
};

}
