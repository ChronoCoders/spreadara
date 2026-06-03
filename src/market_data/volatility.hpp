// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace spreadara::market_data {

class TickVolatility {
public:
    explicit TickVolatility(std::size_t window) : window_(window) {
        prices_.reserve(window);
    }

    void push_mid(double mid) {
        if (mid <= 0.0) return;
        if (prices_.size() < window_) {
            prices_.push_back(mid);
            if (prices_.size() >= 2) {
                ++count_;
            }
            return;
        }
        prices_[idx_] = mid;
        idx_ = (idx_ + 1) % window_;
    }

    double stdev_log_returns() const {
        const std::size_t n = prices_.size();
        if (n < 2) return 0.0;

        // WHY: walk in chronological order starting after idx_ for the rolling window
        const std::size_t start = (prices_.size() < window_) ? 0 : idx_;
        std::vector<double> returns;
        returns.reserve(n - 1);

        double prev = prices_[start];
        for (std::size_t k = 1; k < n; ++k) {
            const std::size_t i = (start + k) % n;
            const double p = prices_[i];
            if (prev > 0.0 && p > 0.0) {
                returns.push_back(std::log(p / prev));
            }
            prev = p;
        }
        if (returns.empty()) return 0.0;

        double mean = 0.0;
        for (double r : returns) mean += r;
        mean /= static_cast<double>(returns.size());

        double sq = 0.0;
        for (double r : returns) sq += (r - mean) * (r - mean);
        return std::sqrt(sq / static_cast<double>(returns.size()));
    }

private:
    std::size_t window_;
    std::vector<double> prices_;
    std::size_t idx_{0};
    std::size_t count_{0};
};

}
