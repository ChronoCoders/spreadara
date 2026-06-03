// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

#include "infra/config.hpp"
#include "risk/position_tracker.hpp"

namespace spreadara::db { class PgReporter; }

namespace spreadara::risk {

enum class RiskResult {
    APPROVED,
    REJECTED_POSITION,
    REJECTED_SIZE,
    REJECTED_PRICE,
    REJECTED_RATE,
    REJECTED_LOSS,
    REJECTED_OPEN_ORDERS,
};

// Short stable label for a RiskResult, for structured rejection logging.
const char* to_str(RiskResult r);

class RiskManager {
public:
    RiskManager(const infra::Config& cfg, PositionTracker& pt);

    // side_signed_qty: +qty for buy, -qty for sell.
    RiskResult pre_trade_check(double side_signed_qty, double price, double current_mid);

    // WHY: rate limit tracks REAL REST calls at the OKX REST boundary, not
    // cheap local pre_trade_check evaluations. Order and cancel traffic are
    // counted in SEPARATE windows so a cancel storm can't exhaust the order
    // budget (or vice versa) — OKX enforces per-endpoint limits. OrderManager
    // calls record_submission() before each place/market-order and
    // record_cancel() before each cancel. pre_trade_check gates the ORDER
    // window; cancels are recorded but never blocked (a cancel is a safety
    // operation that must not be abandoned on a rate cap).
    void record_submission();
    void record_cancel();

    void set_open_order_count(int n) { open_order_count_.store(n, std::memory_order_release); }
    int open_order_count() const { return open_order_count_.load(std::memory_order_acquire); }

    // Number of rejections recorded in the last 30 seconds.
    int consecutive_rejections_in_window(std::chrono::steady_clock::time_point now);

    // WHY: optional hook. nullptr by default keeps existing tests intact.
    void set_reporter(db::PgReporter* r) { reporter_ = r; }

private:
    // Trim entries older than the window and append tp. Caller holds mu_.
    void record_in(std::deque<std::chrono::steady_clock::time_point>& bucket,
                   std::chrono::steady_clock::time_point tp);
    void record_rejection(std::chrono::steady_clock::time_point tp);

    const infra::Config& cfg_;
    PositionTracker& pt_;

    std::mutex mu_;
    // Separate per-endpoint rate windows (see record_submission/record_cancel).
    std::deque<std::chrono::steady_clock::time_point> order_times_;
    std::deque<std::chrono::steady_clock::time_point> cancel_times_;
    std::deque<std::chrono::steady_clock::time_point> rejection_times_;

    std::atomic<int> open_order_count_{0};

    db::PgReporter* reporter_{nullptr};
};

}
