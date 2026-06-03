#pragma once

#include <cmath>

#include "infra/config.hpp"

namespace spreadara::strategy {

class InventoryManager {
public:
    explicit InventoryManager(const infra::Config& cfg) : cfg_(cfg) {}

    void apply_fill(double signed_qty) { current_inventory_ += signed_qty; }

    double current_inventory() const { return current_inventory_; }

    double skew_bps() const {
        if (cfg_.strategy.max_inventory <= 0.0) return 0.0;
        return (current_inventory_ / cfg_.strategy.max_inventory) * cfg_.strategy.max_skew_bps;
    }

    bool emergency_unwind() const {
        if (cfg_.strategy.max_inventory <= 0.0) return false;
        const double ratio = std::abs(current_inventory_) / cfg_.strategy.max_inventory;
        return ratio >= (cfg_.strategy.emergency_unwind_pct / 100.0);
    }

    // Will wire live funding from the exchange; for now return config stub.
    double funding_rate() const { return cfg_.strategy.funding_rate; }

private:
    const infra::Config& cfg_;
    double current_inventory_{0.0};
};

}
