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

class RiskManager {
public:
    RiskManager(const infra::Config& cfg, PositionTracker& pt);

    // side_signed_qty: +qty for buy, -qty for sell.
    RiskResult pre_trade_check(double side_signed_qty, double price, double current_mid);

    void set_open_order_count(int n) { open_order_count_.store(n, std::memory_order_release); }
    int open_order_count() const { return open_order_count_.load(std::memory_order_acquire); }

    // Number of rejections recorded in the last 30 seconds.
    int consecutive_rejections_in_window(std::chrono::steady_clock::time_point now);

    // WHY: optional Phase-5 hook. nullptr by default keeps existing tests intact.
    void set_reporter(db::PgReporter* r) { reporter_ = r; }

private:
    void record_attempt(std::chrono::steady_clock::time_point tp);
    void record_rejection(std::chrono::steady_clock::time_point tp);

    const infra::Config& cfg_;
    PositionTracker& pt_;

    std::mutex mu_;
    std::deque<std::chrono::steady_clock::time_point> attempt_times_;
    std::deque<std::chrono::steady_clock::time_point> rejection_times_;

    std::atomic<int> open_order_count_{0};

    db::PgReporter* reporter_{nullptr};
};

}
