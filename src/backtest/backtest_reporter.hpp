// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spreadara::backtest {

struct BacktestStats {
    double total_pnl{0.0};
    double sharpe_ratio{0.0};
    double max_drawdown_pct{0.0};
    std::size_t fill_count{0};
    double maker_ratio{0.0};
    double avg_spread_captured_bps{0.0};
    double initial_capital{0.0};
    double final_equity{0.0};
};

// Streaming equity-curve aggregator. Sample equity at the reporter's flush
// cadence; compute Sharpe (annualized) from the resulting return series.
class BacktestReporter {
public:
    BacktestReporter(double initial_capital, double risk_free_rate, int flush_interval_ms);

    void on_equity_sample(double equity);
    void on_fill(double price, double spread_bps_at_fill, bool maker);

    BacktestStats finalize() const;

    // Write CSV summary (single-row + header) to `path`.
    bool write_csv(const std::string& path, const BacktestStats& s) const;

private:
    double initial_capital_;
    double risk_free_rate_;
    int flush_interval_ms_;

    std::vector<double> equity_curve_;
    double peak_equity_{0.0};
    double max_dd_pct_{0.0};

    std::size_t fill_count_{0};
    std::size_t maker_fill_count_{0};
    double sum_spread_bps_at_fill_{0.0};
};

}
