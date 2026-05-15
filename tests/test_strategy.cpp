#include <chrono>
#include <cmath>
#include <memory>

#include <gtest/gtest.h>

#include "infra/config.hpp"
#include "strategy/inventory_manager.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/signal_aggregator.hpp"
#include "strategy/spread_model.hpp"

using namespace spreadara;

namespace {

infra::Config make_cfg() {
    infra::Config c{};
    c.strategy.gamma = 0.1;
    c.strategy.k = 1.5;
    c.strategy.horizon = 1.0;
    c.strategy.min_tick = 0.1;
    c.strategy.volatility_floor = 0.0001;
    c.strategy.baseline_volatility = 0.0002;
    c.strategy.vol_widen_multiplier = 1.5;
    c.strategy.depth_threshold = 10.0;
    c.strategy.inventory_skew_threshold_pct = 30.0;
    c.strategy.max_inventory = 5.0;
    c.strategy.max_skew_bps = 25.0;
    c.strategy.emergency_unwind_pct = 90.0;
    c.strategy.funding_rate = 0.0;
    c.strategy.min_requote_ms = 50;
    c.strategy.price_move_ticks_threshold = 2;
    c.strategy.quote_lifetime_ms = 5000;
    c.strategy.quote_qty = 0.01;
    return c;
}

strategy::Signals make_signals(double mid, double realized_vol, double bid_d, double ask_d) {
    strategy::Signals s{};
    s.mid = mid;
    s.realized_vol = realized_vol;
    s.sigma_sq = realized_vol * realized_vol;
    s.bid_depth_5 = bid_d;
    s.ask_depth_5 = ask_d;
    s.total_depth = bid_d + ask_d;
    s.depth_imbalance = (s.total_depth > 0.0) ? (bid_d - ask_d) / s.total_depth : 0.0;
    s.last_trade_price = mid;
    s.last_trade_qty = 0.0;
    s.valid = true;
    return s;
}

}

TEST(Strategy, AvellanedaStoikovFormula) {
    const double gamma = 0.1;
    const double k = 1.5;
    const double T = 1.0;
    const double sigma_sq = 4e-8;
    const double s = 100.0;

    {
        const double q = 0.0;
        const double r = s - q * gamma * sigma_sq * T;
        const double delta = gamma * sigma_sq * T + (2.0 / gamma) * std::log1p(gamma / k);
        EXPECT_NEAR(r, 100.0, 1e-9);
        EXPECT_NEAR(delta, 1.290770, 1e-4);
    }
    {
        const double q = 1.0;
        const double r = s - q * gamma * sigma_sq * T;
        EXPECT_NEAR(r, 100.0 - 0.1 * 4e-8, 1e-12);
        EXPECT_NEAR(r, 100.0, 1e-6);
    }
}

TEST(Strategy, SpreadWidens) {
    auto cfg = make_cfg();
    strategy::SpreadModel sm(cfg);

    // Below trigger: vol below baseline * multiplier, plenty of depth.
    auto sig_calm = make_signals(100.0, cfg.strategy.baseline_volatility * 0.5, 50.0, 50.0);
    EXPECT_DOUBLE_EQ(sm.widen_factor(sig_calm, 0.0), 1.0);

    // Above trigger: realized vol = baseline * 2.
    auto sig_hot = make_signals(100.0, cfg.strategy.baseline_volatility * 2.0, 50.0, 50.0);
    EXPECT_GT(sm.widen_factor(sig_hot, 0.0), 1.0);

    const double base_delta = 1.0;
    const double widened = sm.final_spread(base_delta, sig_hot, 0.0);
    const double base = sm.final_spread(base_delta, sig_calm, 0.0);
    EXPECT_GT(widened, base);
    EXPECT_DOUBLE_EQ(base, base_delta);
}

TEST(Strategy, InventorySkew) {
    auto cfg = make_cfg();
    strategy::InventoryManager im(cfg);
    im.apply_fill(2.5);
    EXPECT_DOUBLE_EQ(im.current_inventory(), 2.5);
    EXPECT_DOUBLE_EQ(im.skew_bps(), 12.5);
    EXPECT_FALSE(im.emergency_unwind());
}

TEST(Strategy, EmergencyUnwind) {
    auto cfg = make_cfg();
    strategy::InventoryManager im(cfg);
    im.apply_fill(4.6);
    EXPECT_TRUE(im.emergency_unwind());
}

TEST(Strategy, QuoteRefreshGating) {
    auto cfg = make_cfg();
    strategy::SpreadModel sm(cfg);
    strategy::InventoryManager im(cfg);
    auto ring_up = std::make_unique<strategy::QuoteRing>();
    auto& ring = *ring_up;
    strategy::MarketMaker mm(cfg, sm, im, &ring);

    auto t0 = std::chrono::steady_clock::now();
    mm.set_clock_override(t0);

    auto sig = make_signals(100.0, cfg.strategy.baseline_volatility * 0.5, 50.0, 50.0);
    ASSERT_TRUE(mm.on_signals(sig));  // first quote emits
    strategy::QuoteMsg msg;
    ASSERT_TRUE(ring.pop(msg));

    // Tiny price move (well under 2 ticks = 0.2), same tick: shouldn't requote within window.
    mm.set_clock_override(t0 + std::chrono::milliseconds(10));
    auto sig2 = make_signals(100.001, cfg.strategy.baseline_volatility * 0.5, 50.0, 50.0);
    EXPECT_FALSE(mm.on_signals(sig2));
    EXPECT_FALSE(ring.pop(msg));

    // After min_requote_ms with the lifetime path still gated by no trigger -> no requote either.
    // To assert the "elapsed allows" leg fires, we cross both elapsed >= min_requote_ms AND
    // a price move >= threshold.
    mm.set_clock_override(t0 + std::chrono::milliseconds(60));
    auto sig3 = make_signals(100.5, cfg.strategy.baseline_volatility * 0.5, 50.0, 50.0);
    EXPECT_TRUE(mm.on_signals(sig3));
    EXPECT_TRUE(ring.pop(msg));
}
