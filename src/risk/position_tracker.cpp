#include "risk/position_tracker.hpp"

#include <cmath>

#include <spdlog/spdlog.h>

namespace spreadara::risk {

// WHY: on x86_64 std::atomic<double> uses lock cmpxchg; this static_assert
// guards us against builds where the price/inventory atomics would silently
// degrade to mutex-backed locks, breaking the wait-free read contract.
static_assert(std::atomic<double>::is_always_lock_free,
              "std::atomic<double> must be lock-free on this platform");

PositionTracker::PositionTracker() = default;

bool PositionTracker::apply_fill(const FillInput& f) {
    if (f.qty <= 0.0 || (f.side != 1 && f.side != -1) || f.price <= 0.0) {
        spdlog::warn("position_tracker_invalid_fill side={} price={} qty={}",
                     f.side, f.price, f.qty);
        return false;
    }

    const double signed_qty = static_cast<double>(f.side) * f.qty;
    const double inv = current_inventory_.load(std::memory_order_acquire);
    const double ae = avg_entry_price_.load(std::memory_order_acquire);
    double new_inv = inv + signed_qty;
    double new_ae = ae;
    double realized_delta = 0.0;

    // WHY: tolerance compare — floating residuals from prior partial reduces
    // can leave inv ~1e-18 instead of exactly 0, mis-routing into the
    // weighted-average branch with a near-zero denominator.
    constexpr double kInvEpsilon = 1e-12;
    if (std::abs(inv) < kInvEpsilon) {
        new_ae = f.price;
    } else if ((inv > 0.0 && signed_qty > 0.0) || (inv < 0.0 && signed_qty < 0.0)) {
        // Adding to existing position — weighted average.
        const double total_qty = std::abs(inv) + std::abs(signed_qty);
        new_ae = (std::abs(inv) * ae + std::abs(signed_qty) * f.price) / total_qty;
    } else {
        // Reducing or flipping.
        const double close_qty = std::min(std::abs(inv), std::abs(signed_qty));
        // sign of inv defines direction being closed: long closes by sell -> profit when price > ae.
        const double dir = (inv > 0.0) ? 1.0 : -1.0;
        realized_delta = dir * close_qty * (f.price - ae);
        if (std::abs(signed_qty) > std::abs(inv)) {
            // Flip: residual opens new position at fill price.
            new_ae = f.price;
        } else if (new_inv == 0.0) {
            new_ae = 0.0;
        }
        // else: partial reduce, avg_entry stays the same.
    }

    // WHY: store realized + fees BEFORE inv/ae so concurrent readers computing
    // equity = realized + inv·(mid - ae) skew upward during the write window
    // (over-counts realized briefly) rather than downward. Upward skew is
    // benign — peak-equity only ratchets up, no false drawdown trigger.
    if (realized_delta != 0.0) {
        const double prev = realized_pnl_.load(std::memory_order_acquire);
        realized_pnl_.store(prev + realized_delta, std::memory_order_release);
    }
    const double prev_fees = total_fees_.load(std::memory_order_acquire);
    total_fees_.store(prev_fees + f.fee, std::memory_order_release);
    current_inventory_.store(new_inv, std::memory_order_release);
    avg_entry_price_.store(new_ae, std::memory_order_release);
    return true;
}

void PositionTracker::update_mid(double mid) {
    if (mid > 0.0) last_mid_.store(mid, std::memory_order_release);
}

}
