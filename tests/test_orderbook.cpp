#include <gtest/gtest.h>

#include "market_data/orderbook.hpp"

using namespace spreadara::market_data;

namespace {

std::array<PriceLevel, OrderBook::kMaxLevels> make_levels(std::initializer_list<PriceLevel> items) {
    std::array<PriceLevel, OrderBook::kMaxLevels> arr{};
    std::size_t i = 0;
    for (auto& it : items) {
        if (i >= arr.size()) break;
        arr[i++] = it;
    }
    return arr;
}

}

TEST(OrderBook, SnapshotApplyAndTopOfBook) {
    OrderBook ob;
    auto bids = make_levels({{100.0, 1.0}, {99.5, 2.0}, {99.0, 3.0}});
    auto asks = make_levels({{101.0, 1.5}, {101.5, 2.5}, {102.0, 3.5}});
    ob.apply_snapshot(bids, 3, asks, 3, 1000);

    EXPECT_DOUBLE_EQ(ob.best_bid(), 100.0);
    EXPECT_DOUBLE_EQ(ob.best_ask(), 101.0);
    EXPECT_DOUBLE_EQ(ob.mid(), 100.5);
    EXPECT_FALSE(ob.needs_resync());
    EXPECT_EQ(ob.last_update_id(), 1000u);
}

TEST(OrderBook, SnapshotSortsCorrectly) {
    OrderBook ob;
    auto bids = make_levels({{99.0, 1.0}, {100.0, 1.0}, {99.5, 1.0}});
    auto asks = make_levels({{102.0, 1.0}, {101.0, 1.0}, {101.5, 1.0}});
    ob.apply_snapshot(bids, 3, asks, 3, 1);
    EXPECT_DOUBLE_EQ(ob.best_bid(), 100.0);
    EXPECT_DOUBLE_EQ(ob.best_ask(), 101.0);
}

TEST(OrderBook, PartialUpdateContinuity) {
    OrderBook ob;
    auto bids = make_levels({{100.0, 1.0}});
    auto asks = make_levels({{101.0, 1.0}});
    ob.apply_snapshot(bids, 1, asks, 1, 1000);

    DepthEvent ev{};
    ev.first_update_id = 1001;
    ev.final_update_id = 1005;
    ev.prev_final_update_id = 1000;
    ev.bid_count = 1;
    ev.ask_count = 1;
    ev.bids[0] = {100.5, 2.0};
    ev.asks[0] = {101.5, 2.0};

    EXPECT_TRUE(ob.apply_partial_update(ev));
    EXPECT_DOUBLE_EQ(ob.best_bid(), 100.5);
    EXPECT_EQ(ob.last_update_id(), 1005u);
    EXPECT_FALSE(ob.needs_resync());
}

TEST(OrderBook, SequenceGapTriggersResync) {
    OrderBook ob;
    auto bids = make_levels({{100.0, 1.0}});
    auto asks = make_levels({{101.0, 1.0}});
    ob.apply_snapshot(bids, 1, asks, 1, 1000);

    DepthEvent ev{};
    ev.first_update_id = 1010;
    ev.final_update_id = 1015;
    ev.prev_final_update_id = 1005;  // gap: should be 1000
    ev.bid_count = 1;
    ev.ask_count = 1;
    ev.bids[0] = {100.5, 2.0};
    ev.asks[0] = {101.5, 2.0};

    EXPECT_FALSE(ob.apply_partial_update(ev));
    EXPECT_TRUE(ob.needs_resync());
}

TEST(OrderBook, DepthSum) {
    OrderBook ob;
    auto bids = make_levels({{100.0, 1.0}, {99.5, 2.0}, {99.0, 3.0}, {98.5, 4.0}});
    auto asks = make_levels({{101.0, 1.5}, {101.5, 2.5}, {102.0, 3.5}, {102.5, 4.5}});
    ob.apply_snapshot(bids, 4, asks, 4, 1);

    EXPECT_DOUBLE_EQ(ob.depth_sum(true, 3), 6.0);
    EXPECT_DOUBLE_EQ(ob.depth_sum(false, 3), 7.5);
    EXPECT_DOUBLE_EQ(ob.depth_sum(true, 100), 10.0);
}

TEST(OrderBook, SpreadBps) {
    OrderBook ob;
    auto bids = make_levels({{100.0, 1.0}});
    auto asks = make_levels({{100.1, 1.0}});
    ob.apply_snapshot(bids, 1, asks, 1, 1);
    const double mid = 100.05;
    const double expected = (100.1 - 100.0) / mid * 10000.0;
    EXPECT_NEAR(ob.spread_bps(), expected, 1e-9);
}
