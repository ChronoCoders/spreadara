// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "strategy/market_maker.hpp"

#include <cmath>
#include <cstring>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include "quote_update_generated.h"
#include "risk/circuit_breaker.hpp"
#include "risk/risk_manager.hpp"

namespace spreadara::strategy {

MarketMaker::MarketMaker(const infra::Config& cfg,
                         SpreadModel& spread,
                         InventoryManager& inv,
                         QuoteRing* out_ring)
    : cfg_(cfg), spread_(spread), inv_(inv), out_ring_(out_ring) {}

MarketMaker::InvTier MarketMaker::classify_inventory(double inv) const {
    if (cfg_.strategy.max_inventory <= 0.0) return InvTier::T0;
    const double r = std::abs(inv) / cfg_.strategy.max_inventory;
    if (r < 0.25) return InvTier::T0;
    if (r < 0.50) return InvTier::T1;
    if (r < 0.75) return InvTier::T2;
    return InvTier::T3;
}

MarketMaker::VolRegime MarketMaker::classify_vol(double rv) const {
    const double base = cfg_.strategy.baseline_volatility;
    const double mult = cfg_.strategy.vol_widen_multiplier;
    if (rv < base) return VolRegime::Low;
    if (rv < base * mult) return VolRegime::Mid;
    return VolRegime::High;
}

bool MarketMaker::should_requote(double new_bid, double new_ask, InvTier tier, VolRegime regime,
                                 std::chrono::steady_clock::time_point tp) const {
    if (!last_quote_.valid) return true;

    const double tick = cfg_.strategy.min_tick;
    const double thresh = cfg_.strategy.price_move_ticks_threshold * tick;
    const double bid_move = std::abs(new_bid - last_quote_.bid_price);
    const double ask_move = std::abs(new_ask - last_quote_.ask_price);
    const double max_move = std::max(bid_move, ask_move);

    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp - last_emit_).count();

    // WHY: force-path bypasses min_requote_ms when the move is large (>2x threshold).
    const bool force = max_move > thresh * 2.0;
    if (!force && age_ms < cfg_.strategy.min_requote_ms) return false;

    if (max_move >= thresh) return true;
    if (tier != last_tier_) return true;
    if (regime != last_regime_) return true;
    if (age_ms > cfg_.strategy.quote_lifetime_ms) return true;
    return false;
}

void MarketMaker::emit_quote(double bid, double ask, double inv, double skew_bps,
                             std::chrono::steady_clock::time_point tp) {
    const uint64_t ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count());

    fbb_.Clear();
    auto& fbb = fbb_;
    auto q = schemas::CreateQuoteUpdate(fbb, bid, ask, cfg_.strategy.quote_qty, ts_ns, inv, skew_bps);
    fbb.Finish(q);

    if (out_ring_) {
        QuoteMsg msg;
        const auto sz = fbb.GetSize();
        if (sz <= msg.bytes.size()) {
            msg.size = static_cast<uint16_t>(sz);
            std::memcpy(msg.bytes.data(), fbb.GetBufferPointer(), sz);
            if (!out_ring_->push(msg)) {
                spdlog::warn("quote_ring_full bid={:.4f} ask={:.4f}", bid, ask);
            }
        } else {
            spdlog::warn("quote_oversize bytes={}", sz);
        }
    }

    last_quote_.bid_price = bid;
    last_quote_.ask_price = ask;
    last_quote_.qty = cfg_.strategy.quote_qty;
    last_quote_.inventory = inv;
    last_quote_.skew_bps = skew_bps;
    last_quote_.timestamp_ns = ts_ns;
    last_quote_.valid = true;
    last_emit_ = tp;
}

bool MarketMaker::on_signals(const Signals& sig) {
    if (!sig.valid || sig.mid <= 0.0) return false;

    const double s = sig.mid;
    const double q = inv_.current_inventory();
    const double gamma = cfg_.strategy.gamma;
    const double k = cfg_.strategy.k;
    const double T = cfg_.strategy.horizon;
    const double sigma_sq = sig.sigma_sq;

    // Avellaneda-Stoikov reservation price + base half-spread.
    const double r = s - q * gamma * sigma_sq * T;
    const double base_delta = gamma * sigma_sq * T + (2.0 / gamma) * std::log1p(gamma / k);

    // WHY: compose widen factor on base_delta first, then floor against minimum_spread();
    // this preserves the formula proportions while guaranteeing we never quote inside the
    // exchange tick / volatility floor.
    const double delta = spread_.final_spread(base_delta, sig, q);

    double bid = r - delta / 2.0;
    double ask = r + delta / 2.0;

    // Inventory skew (bps of mid) shifts both sides to lean away from inventory.
    const double skew_bps = inv_.skew_bps();
    const double shift = s * (skew_bps / 10000.0);
    bid -= shift;
    ask -= shift;

    const auto tp = now();
    const InvTier tier = classify_inventory(q);
    const VolRegime regime = classify_vol(sig.realized_vol);

    // WHY: halt is a hard gate — no quotes emitted while circuit breaker is tripped.
    if (cb_ && cb_->halted()) return false;

    if (risk_) {
        const double qty = cfg_.strategy.quote_qty;
        const auto br = risk_->pre_trade_check(+qty, bid, sig.mid);
        const auto ar = risk_->pre_trade_check(-qty, ask, sig.mid);
        if (br != risk::RiskResult::APPROVED || ar != risk::RiskResult::APPROVED) {
            return false;
        }
    }

    if (!should_requote(bid, ask, tier, regime, tp)) return false;

    emit_quote(bid, ask, q, skew_bps, tp);
    last_tier_ = tier;
    last_regime_ = regime;
    return true;
}

}
