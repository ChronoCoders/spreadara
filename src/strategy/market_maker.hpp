#pragma once

#include <chrono>
#include <cstdint>

#include "infra/config.hpp"
#include "strategy/inventory_manager.hpp"
#include "strategy/signal_aggregator.hpp"
#include "strategy/spread_model.hpp"

namespace spreadara::strategy {

struct QuoteState {
    double bid_price{0.0};
    double ask_price{0.0};
    double qty{0.0};
    double inventory{0.0};
    double skew_bps{0.0};
    uint64_t timestamp_ns{0};
    bool valid{false};
};

class MarketMaker {
public:
    MarketMaker(const infra::Config& cfg,
                SpreadModel& spread,
                InventoryManager& inv,
                QuoteRing* out_ring);

    // Returns true when a new quote was emitted (and pushed to the ring if present).
    bool on_signals(const Signals& sig);

    const QuoteState& last_quote() const { return last_quote_; }

    // Test hook: override "now" for deterministic gating.
    void set_clock_override(std::chrono::steady_clock::time_point tp) {
        clock_override_ = tp;
        clock_overridden_ = true;
    }
    void clear_clock_override() { clock_overridden_ = false; }

private:
    enum class InvTier : uint8_t { T0, T1, T2, T3 };
    enum class VolRegime : uint8_t { Low, Mid, High };

    std::chrono::steady_clock::time_point now() const {
        return clock_overridden_ ? clock_override_ : std::chrono::steady_clock::now();
    }
    InvTier classify_inventory(double inv) const;
    VolRegime classify_vol(double realized_vol) const;
    bool should_requote(double new_bid, double new_ask, InvTier tier, VolRegime regime,
                        std::chrono::steady_clock::time_point tp) const;
    void emit_quote(double bid, double ask, double inv, double skew_bps,
                    std::chrono::steady_clock::time_point tp);

    const infra::Config& cfg_;
    SpreadModel& spread_;
    InventoryManager& inv_;
    QuoteRing* out_ring_;

    QuoteState last_quote_{};
    std::chrono::steady_clock::time_point last_emit_{};
    InvTier last_tier_{InvTier::T0};
    VolRegime last_regime_{VolRegime::Low};

    bool clock_overridden_{false};
    std::chrono::steady_clock::time_point clock_override_{};
};

}
