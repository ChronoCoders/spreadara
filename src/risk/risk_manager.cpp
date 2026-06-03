// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "risk/risk_manager.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

#include <spdlog/spdlog.h>

#include "db/pg_reporter.hpp"

namespace spreadara::risk {

namespace {
constexpr std::chrono::seconds kRateWindow{60};
constexpr std::chrono::seconds kRejectionWindow{30};
}

const char* to_str(RiskResult r) {
    switch (r) {
        case RiskResult::REJECTED_POSITION: return "position";
        case RiskResult::REJECTED_SIZE: return "size";
        case RiskResult::REJECTED_PRICE: return "price";
        case RiskResult::REJECTED_RATE: return "rate";
        case RiskResult::REJECTED_LOSS: return "loss";
        case RiskResult::REJECTED_OPEN_ORDERS: return "open_orders";
        case RiskResult::APPROVED: return "approved";
    }
    return "unknown";
}

RiskManager::RiskManager(const infra::Config& cfg, PositionTracker& pt)
    : cfg_(cfg), pt_(pt) {}

void RiskManager::record_in(std::deque<std::chrono::steady_clock::time_point>& bucket,
                            std::chrono::steady_clock::time_point tp) {
    const auto cutoff = tp - kRateWindow;
    while (!bucket.empty() && bucket.front() < cutoff) {
        bucket.pop_front();
    }
    bucket.push_back(tp);
}

void RiskManager::record_submission() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu_);
    record_in(order_times_, now);
}

void RiskManager::record_cancel() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu_);
    record_in(cancel_times_, now);
}

void RiskManager::record_rejection(std::chrono::steady_clock::time_point tp) {
    const auto cutoff = tp - kRejectionWindow;
    while (!rejection_times_.empty() && rejection_times_.front() < cutoff) {
        rejection_times_.pop_front();
    }
    rejection_times_.push_back(tp);
}

int RiskManager::consecutive_rejections_in_window(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto cutoff = now - kRejectionWindow;
    while (!rejection_times_.empty() && rejection_times_.front() < cutoff) {
        rejection_times_.pop_front();
    }
    return static_cast<int>(rejection_times_.size());
}

RiskResult RiskManager::pre_trade_check(double side_signed_qty, double price, double current_mid) {
    const auto now = std::chrono::steady_clock::now();
    const double abs_qty = std::abs(side_signed_qty);
    const double inv = pt_.current_inventory();
    const double equity = pt_.equity();
    const int open_orders = open_order_count_.load(std::memory_order_acquire);

    RiskResult result = RiskResult::APPROVED;

    // 1. Position limit.
    const double projected_inv = inv + side_signed_qty;
    if (std::abs(projected_inv) > cfg_.risk.max_position) {
        result = RiskResult::REJECTED_POSITION;
    }
    // 2. Order size.
    else if (abs_qty > cfg_.risk.max_order_size) {
        result = RiskResult::REJECTED_SIZE;
    }
    // 3. Price sanity (% deviation from mid).
    else if (current_mid > 0.0 &&
             std::abs(price - current_mid) / current_mid * 100.0 > cfg_.risk.price_sanity_pct) {
        result = RiskResult::REJECTED_PRICE;
    }
    else {
        // 4. Rate limit — READ-ONLY check against the window. The window is
        // populated exclusively by record_submission() called from
        // OrderManager immediately before each REST write call. We do NOT
        // increment here because pre_trade_check fires far more often than
        // actual REST traffic (the strategy evaluates every books5 tick).
        std::lock_guard<std::mutex> lk(mu_);
        // Trim stale entries before reading the count. Gates the ORDER window.
        const auto cutoff = now - kRateWindow;
        while (!order_times_.empty() && order_times_.front() < cutoff) {
            order_times_.pop_front();
        }
        if (static_cast<int>(order_times_.size()) > cfg_.risk.rate_limit_threshold) {
            result = RiskResult::REJECTED_RATE;
        }
        // 5. Daily loss.
        else if (equity < -cfg_.risk.max_daily_loss) {
            result = RiskResult::REJECTED_LOSS;
        }
        // 6. Open orders.
        else if (open_orders >= cfg_.risk.max_open_orders) {
            result = RiskResult::REJECTED_OPEN_ORDERS;
        }
    }

    if (result != RiskResult::APPROVED) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            record_rejection(now);
        }
        spdlog::warn("risk_reject reason={} qty={:.6f} price={:.4f} mid={:.4f} inv={:.6f} equity={:.4f} open_orders={}",
                     to_str(result), side_signed_qty, price, current_mid, inv, equity,
                     open_orders);
        if (reporter_) {
            db::DbEvent ev{};
            ev.kind = db::DbEventKind::SystemEvent;
            ev.evt.ts_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            std::snprintf(ev.evt.severity, sizeof(ev.evt.severity), "warn");
            std::snprintf(ev.evt.source, sizeof(ev.evt.source), "risk");
            std::snprintf(ev.evt.code, sizeof(ev.evt.code), "%s", to_str(result));
            std::snprintf(ev.evt.msg, sizeof(ev.evt.msg),
                          "qty=%.6f price=%.4f mid=%.4f inv=%.6f equity=%.4f open_orders=%d",
                          side_signed_qty, price, current_mid, inv, equity, open_orders);
            (void)reporter_->push(ev);
        }
    }
    return result;
}

}
