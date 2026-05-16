#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "infra/config.hpp"
#include "risk/circuit_breaker.hpp"
#include "risk/position_tracker.hpp"
#include "risk/risk_manager.hpp"

using namespace spreadara;

namespace {

infra::Config make_cfg() {
    infra::Config c{};
    c.risk.max_position = 0.1;
    c.risk.max_order_size = 0.05;
    c.risk.price_sanity_pct = 0.5;
    c.risk.rate_limit_threshold = 1000;
    c.risk.max_daily_loss = 500.0;
    c.risk.max_open_orders = 10;
    c.risk.max_drawdown_pct = 5.0;
    c.risk.max_unhedged_seconds = 30;
    c.risk.max_consecutive_rejections = 5;
    c.risk.circuit_breaker_poll_ms = 5;
    c.runtime.risk_cpu_core = -1;
    return c;
}

risk::FillInput buy(double price, double qty, double fee = 0.0) {
    risk::FillInput f;
    f.side = 1;
    f.price = price;
    f.qty = qty;
    f.fee = fee;
    return f;
}

risk::FillInput sell(double price, double qty, double fee = 0.0) {
    risk::FillInput f;
    f.side = -1;
    f.price = price;
    f.qty = qty;
    f.fee = fee;
    return f;
}

}

TEST(PositionTracker, AvgEntryAndRealizedPnl) {
    risk::PositionTracker pt;
    ASSERT_TRUE(pt.apply_fill(buy(100.0, 1.0)));
    ASSERT_TRUE(pt.apply_fill(buy(110.0, 1.0)));
    EXPECT_DOUBLE_EQ(pt.avg_entry(), 105.0);
    EXPECT_DOUBLE_EQ(pt.current_inventory(), 2.0);

    ASSERT_TRUE(pt.apply_fill(sell(120.0, 1.0)));
    EXPECT_DOUBLE_EQ(pt.realized_pnl(), 15.0);
    EXPECT_DOUBLE_EQ(pt.current_inventory(), 1.0);
    EXPECT_DOUBLE_EQ(pt.avg_entry(), 105.0);
}

TEST(PositionTracker, FlipDirection) {
    risk::PositionTracker pt;
    ASSERT_TRUE(pt.apply_fill(buy(100.0, 1.0)));
    ASSERT_TRUE(pt.apply_fill(sell(120.0, 2.0)));
    EXPECT_DOUBLE_EQ(pt.realized_pnl(), 20.0);
    EXPECT_DOUBLE_EQ(pt.current_inventory(), -1.0);
    EXPECT_DOUBLE_EQ(pt.avg_entry(), 120.0);
}

TEST(PositionTracker, Unrealized) {
    risk::PositionTracker pt;
    ASSERT_TRUE(pt.apply_fill(buy(100.0, 1.0)));
    pt.update_mid(110.0);
    EXPECT_DOUBLE_EQ(pt.unrealized_pnl(), 10.0);
}

TEST(PositionTracker, Fees) {
    risk::PositionTracker pt;
    ASSERT_TRUE(pt.apply_fill(buy(100.0, 1.0, 0.1)));
    ASSERT_TRUE(pt.apply_fill(buy(101.0, 1.0, 0.2)));
    ASSERT_TRUE(pt.apply_fill(buy(102.0, 1.0, 0.05)));
    EXPECT_NEAR(pt.total_fees(), 0.35, 1e-12);
}

TEST(RiskManager, PositionLimit) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    ASSERT_TRUE(pt.apply_fill(buy(100.0, 0.08)));
    risk::RiskManager rm(cfg, pt);
    EXPECT_EQ(rm.pre_trade_check(+0.05, 100.0, 100.0), risk::RiskResult::REJECTED_POSITION);
}

TEST(RiskManager, OrderSize) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    EXPECT_EQ(rm.pre_trade_check(+0.06, 100.0, 100.0), risk::RiskResult::REJECTED_SIZE);
}

TEST(RiskManager, PriceSanity) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    EXPECT_EQ(rm.pre_trade_check(+0.01, 100.6, 100.0), risk::RiskResult::REJECTED_PRICE);
    EXPECT_EQ(rm.pre_trade_check(+0.01, 100.4, 100.0), risk::RiskResult::APPROVED);
}

TEST(RiskManager, RateLimit) {
    auto cfg = make_cfg();
    cfg.risk.rate_limit_threshold = 5;
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(rm.pre_trade_check(+0.01, 100.0, 100.0), risk::RiskResult::APPROVED);
    }
    EXPECT_EQ(rm.pre_trade_check(+0.01, 100.0, 100.0), risk::RiskResult::REJECTED_RATE);
}

TEST(RiskManager, DailyLoss) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    pt.set_realized_pnl_for_test(-cfg.risk.max_daily_loss - 1.0);
    risk::RiskManager rm(cfg, pt);
    EXPECT_EQ(rm.pre_trade_check(+0.01, 100.0, 100.0), risk::RiskResult::REJECTED_LOSS);
}

TEST(RiskManager, OpenOrders) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    rm.set_open_order_count(cfg.risk.max_open_orders);
    EXPECT_EQ(rm.pre_trade_check(+0.01, 100.0, 100.0), risk::RiskResult::REJECTED_OPEN_ORDERS);
}

TEST(CircuitBreaker, DrawdownTrigger) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    risk::RiskEventRing ring;
    risk::CircuitBreaker cb(cfg, pt, rm, &ring);

    pt.set_realized_pnl_for_test(1000.0);
    cb.tick_for_test();  // record peak
    pt.set_realized_pnl_for_test(940.0);
    cb.tick_for_test();  // drawdown 6% > 5%
    EXPECT_TRUE(cb.halted());

    risk::RiskEventMsg msg;
    EXPECT_TRUE(ring.pop(msg));
    EXPECT_GT(msg.size, 0);
}

TEST(CircuitBreaker, UnhedgedTrigger) {
    auto cfg = make_cfg();
    cfg.risk.max_unhedged_seconds = 0;
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    risk::RiskEventRing ring;
    risk::CircuitBreaker cb(cfg, pt, rm, &ring);

    ASSERT_TRUE(pt.apply_fill(buy(100.0, 1.0)));
    cb.tick_for_test();  // start tracking
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    cb.tick_for_test();  // elapsed > 0
    EXPECT_TRUE(cb.halted());
}

TEST(CircuitBreaker, WsDisconnect) {
    auto cfg = make_cfg();
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    risk::RiskEventRing ring;
    risk::CircuitBreaker cb(cfg, pt, rm, &ring);

    cb.notify_ws_disconnect_exhausted();
    EXPECT_TRUE(cb.halted());  // halt is synchronous

    risk::RiskEventMsg msg;
    EXPECT_FALSE(ring.pop(msg));  // ring publish is deferred to monitor tick

    cb.tick_for_test();
    EXPECT_TRUE(ring.pop(msg));
}

TEST(CircuitBreaker, ConsecutiveRejections) {
    auto cfg = make_cfg();
    cfg.risk.max_consecutive_rejections = 3;
    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    risk::RiskEventRing ring;
    risk::CircuitBreaker cb(cfg, pt, rm, &ring);

    for (int i = 0; i < 4; ++i) {
        // Order size rejection — > max_order_size.
        rm.pre_trade_check(+0.5, 100.0, 100.0);
    }
    cb.tick_for_test();
    EXPECT_TRUE(cb.halted());
}
