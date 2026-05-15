#pragma once

#include "infra/config.hpp"
#include "strategy/signal_aggregator.hpp"

namespace spreadara::strategy {

class SpreadModel {
public:
    explicit SpreadModel(const infra::Config& cfg) : cfg_(cfg) {}

    double minimum_spread() const;
    double widen_factor(const Signals& sig, double current_inventory) const;
    double final_spread(double base_delta, const Signals& sig, double current_inventory) const;

private:
    const infra::Config& cfg_;
};

}
