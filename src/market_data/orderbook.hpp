#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "market_data/market_event.hpp"

namespace spreadara::market_data {

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
