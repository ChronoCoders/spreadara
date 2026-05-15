#include "market_data/orderbook.hpp"

#include <algorithm>

namespace spreadara::market_data {

void OrderBook::apply_snapshot(const std::array<PriceLevel, kMaxLevels>& bids, std::size_t bid_count,
                               const std::array<PriceLevel, kMaxLevels>& asks, std::size_t ask_count,
                               uint64_t last_update_id) {
    bids_ = bids;
    asks_ = asks;
    bid_count_ = std::min(bid_count, kMaxLevels);
    ask_count_ = std::min(ask_count, kMaxLevels);

    std::sort(bids_.begin(), bids_.begin() + static_cast<long>(bid_count_),
              [](const PriceLevel& a, const PriceLevel& b) { return a.price > b.price; });
    std::sort(asks_.begin(), asks_.begin() + static_cast<long>(ask_count_),
              [](const PriceLevel& a, const PriceLevel& b) { return a.price < b.price; });

    last_update_id_ = last_update_id;
    initialized_ = true;
    needs_resync_ = false;
}

bool OrderBook::apply_partial_update(const DepthEvent& ev) {
    if (initialized_ && ev.prev_final_update_id != last_update_id_) {
        needs_resync_ = true;
        return false;
    }

    const std::size_t b = std::min<std::size_t>(ev.bid_count, kMaxLevels);
    const std::size_t a = std::min<std::size_t>(ev.ask_count, kMaxLevels);
    // TODO(phase3): stale tail — entries past bid_count_/ask_count_ retain old values.
    // Invisible via accessors but a debugger trap when strategy code reads raw arrays.
    for (std::size_t i = 0; i < b; ++i) bids_[i] = ev.bids[i];
    for (std::size_t i = 0; i < a; ++i) asks_[i] = ev.asks[i];
    bid_count_ = b;
    ask_count_ = a;

    std::sort(bids_.begin(), bids_.begin() + static_cast<long>(bid_count_),
              [](const PriceLevel& x, const PriceLevel& y) { return x.price > y.price; });
    std::sort(asks_.begin(), asks_.begin() + static_cast<long>(ask_count_),
              [](const PriceLevel& x, const PriceLevel& y) { return x.price < y.price; });

    last_update_id_ = ev.final_update_id;
    initialized_ = true;
    return true;
}

// TODO(phase3): no crossed-book detection (best_ask < best_bid). Add when strategy logic arrives.

double OrderBook::best_bid() const {
    return bid_count_ > 0 ? bids_[0].price : 0.0;
}

double OrderBook::best_ask() const {
    return ask_count_ > 0 ? asks_[0].price : 0.0;
}

double OrderBook::mid() const {
    if (bid_count_ == 0 || ask_count_ == 0) return 0.0;
    return (bids_[0].price + asks_[0].price) * 0.5;
}

double OrderBook::spread_bps() const {
    const double m = mid();
    if (m <= 0.0) return 0.0;
    return (asks_[0].price - bids_[0].price) / m * 10000.0;
}

double OrderBook::depth_sum(bool bid_side, std::size_t levels) const {
    const auto& arr = bid_side ? bids_ : asks_;
    const std::size_t n = std::min(levels, bid_side ? bid_count_ : ask_count_);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) sum += arr[i].qty;
    return sum;
}

}
