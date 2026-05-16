#include "backtest/backtest_reporter.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace spreadara::backtest {

BacktestReporter::BacktestReporter(double initial_capital, double risk_free_rate,
                                   int flush_interval_ms)
    : initial_capital_(initial_capital),
      risk_free_rate_(risk_free_rate),
      flush_interval_ms_(flush_interval_ms > 0 ? flush_interval_ms : 1000),
      peak_equity_(initial_capital) {}

void BacktestReporter::on_equity_sample(double equity) {
    equity_curve_.push_back(equity);
    if (equity > peak_equity_) peak_equity_ = equity;
    if (peak_equity_ > 0.0) {
        const double dd = (peak_equity_ - equity) / peak_equity_ * 100.0;
        if (dd > max_dd_pct_) max_dd_pct_ = dd;
    }
}

void BacktestReporter::on_fill(double /*price*/, double spread_bps_at_fill, bool maker) {
    ++fill_count_;
    if (maker) ++maker_fill_count_;
    sum_spread_bps_at_fill_ += spread_bps_at_fill;
}

BacktestStats BacktestReporter::finalize() const {
    BacktestStats s{};
    s.initial_capital = initial_capital_;
    s.fill_count = fill_count_;
    s.maker_ratio = (fill_count_ > 0)
        ? static_cast<double>(maker_fill_count_) / static_cast<double>(fill_count_)
        : 0.0;
    s.avg_spread_captured_bps = (fill_count_ > 0)
        ? sum_spread_bps_at_fill_ / static_cast<double>(fill_count_)
        : 0.0;
    s.max_drawdown_pct = max_dd_pct_;

    if (!equity_curve_.empty()) {
        s.final_equity = equity_curve_.back();
        s.total_pnl = s.final_equity - initial_capital_;
    }

    if (equity_curve_.size() >= 2) {
        std::vector<double> rets;
        rets.reserve(equity_curve_.size() - 1);
        for (std::size_t i = 1; i < equity_curve_.size(); ++i) {
            const double prev = equity_curve_[i - 1];
            if (prev > 0.0) {
                rets.push_back((equity_curve_[i] - prev) / prev);
            }
        }
        if (!rets.empty()) {
            double mean = 0.0;
            for (double r : rets) mean += r;
            mean /= static_cast<double>(rets.size());
            double var = 0.0;
            for (double r : rets) var += (r - mean) * (r - mean);
            var /= static_cast<double>(rets.size());
            const double stdev = std::sqrt(var);
            // Annualization: number of samples per year at flush_interval_ms cadence.
            const double samples_per_year =
                252.0 * 24.0 * 60.0 * 60.0 * 1000.0 / static_cast<double>(flush_interval_ms_);
            const double rf_per_sample = risk_free_rate_ / samples_per_year;
            if (stdev > 1e-12) {
                s.sharpe_ratio = (mean - rf_per_sample) / stdev * std::sqrt(samples_per_year);
            }
        }
    }
    return s;
}

bool BacktestReporter::write_csv(const std::string& path, const BacktestStats& s) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return false;
    out << "total_pnl,sharpe_ratio,max_drawdown_pct,fill_count,maker_ratio,"
           "avg_spread_captured_bps,initial_capital,final_equity\n";
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%.8f,%.6f,%.6f,%zu,%.6f,%.6f,%.6f,%.8f\n",
                  s.total_pnl, s.sharpe_ratio, s.max_drawdown_pct, s.fill_count,
                  s.maker_ratio, s.avg_spread_captured_bps,
                  s.initial_capital, s.final_equity);
    out << buf;
    return true;
}

}
