// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace spreadara::schemas {
struct FillEventT;
}

namespace spreadara::risk {

struct FillInput {
    std::string order_id;
    std::string symbol;
    int8_t side{0};  // +1 buy, -1 sell
    double price{0.0};
    double qty{0.0};
    double fee{0.0};
    std::string fee_asset;
    uint64_t timestamp_ns{0};
};

// Concurrency contract: single writer (the fill-processing thread calling
// apply_fill / update_mid), multiple readers (risk manager, circuit breaker,
// reporters). Reads of distinct fields are NOT atomic as a group — equity()
// and unrealized_pnl() may observe a transient mix of old/new field values
// during an apply_fill. apply_fill orders its stores so the cross-field skew
// is biased upward (over-counts realized briefly) to avoid false drawdown
// triggers. Full strict consistency would require a seqlock; intentionally
// not done given the µs-scale read window and benign skew direction.
class PositionTracker {
public:
    PositionTracker();

    bool apply_fill(const FillInput& f);
    void update_mid(double mid);

    double current_inventory() const { return current_inventory_.load(std::memory_order_acquire); }
    double avg_entry() const { return avg_entry_price_.load(std::memory_order_acquire); }
    double realized_pnl() const { return realized_pnl_.load(std::memory_order_acquire); }
    double total_fees() const { return total_fees_.load(std::memory_order_acquire); }
    double last_mid() const { return last_mid_.load(std::memory_order_acquire); }

    double unrealized_pnl() const {
        const double inv = current_inventory();
        const double mid = last_mid();
        const double ae = avg_entry();
        if (inv == 0.0 || mid <= 0.0) return 0.0;
        return inv * (mid - ae);
    }

    double equity() const { return realized_pnl() + unrealized_pnl(); }

    // Test/synth helpers — direct setters so tests can manufacture P&L without
    // synthesizing many fills.
    void set_realized_pnl_for_test(double v) { realized_pnl_.store(v, std::memory_order_release); }

private:
    std::atomic<double> current_inventory_{0.0};
    std::atomic<double> avg_entry_price_{0.0};
    std::atomic<double> realized_pnl_{0.0};
    std::atomic<double> total_fees_{0.0};
    std::atomic<double> last_mid_{0.0};
};

}
