// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "strategy/spread_model.hpp"

#include <algorithm>
#include <cmath>

namespace spreadara::strategy {

namespace {
// WHY: model coefficients (not runtime tuning). Each trigger contributes a small
// multiplicative bump; composing them keeps the widen monotone in the number of
// active stress conditions.
static constexpr double kVolBump = 1.25;
static constexpr double kDepthBump = 1.25;
static constexpr double kInvBump = 1.25;
}

double SpreadModel::minimum_spread() const {
    return std::max(cfg_.strategy.min_tick * 2.0, cfg_.strategy.volatility_floor);
}

double SpreadModel::widen_factor(const Signals& sig, double current_inventory) const {
    double f = 1.0;
    if (sig.realized_vol > cfg_.strategy.baseline_volatility * cfg_.strategy.vol_widen_multiplier) {
        f *= kVolBump;
    }
    if (sig.total_depth > 0.0 && sig.total_depth < cfg_.strategy.depth_threshold) {
        f *= kDepthBump;
    }
    if (cfg_.strategy.max_inventory > 0.0) {
        const double ratio = std::abs(current_inventory) / cfg_.strategy.max_inventory;
        if (ratio > cfg_.strategy.inventory_skew_threshold_pct / 100.0) {
            f *= kInvBump;
        }
    }
    return f;
}

double SpreadModel::final_spread(double base_delta, const Signals& sig, double current_inventory) const {
    const double widened = base_delta * widen_factor(sig, current_inventory);
    return std::max(widened, minimum_spread());
}

}
