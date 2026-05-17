#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "backtest/backtest_reporter.hpp"
#include "infra/config.hpp"

namespace spreadara::db { class PgReporter; }

namespace spreadara::backtest {

// One-shot synchronous backtest harness. Wires:
//   ReplayEngine -> SnapshotRing -> SignalAggregator -> MarketMaker
//                -> OrderManager (no threads) -> SimulatedRestClient
// Runs to archive exhaustion; returns aggregated stats.
class BacktestRunner {
public:
    explicit BacktestRunner(const infra::Config& cfg);

    // Discovers archives in cfg.backtest.data_dir matching the symbol + date range.
    // Files: "<SYMBOL>-bookTicker-YYYY-MM-DD.bin"
    std::vector<std::string> discover_archives() const;

    // Run a backtest over `archives`. Writes results to backtest_results.csv
    // in CWD (unless csv_path is overridden) and returns stats.
    BacktestStats run(const std::vector<std::string>& archives,
                      const std::string& csv_path = "backtest_results.csv");

    // Calibration smoke: 2x2x2 = 8-combo grid over (gamma, k, horizon).
    // Top result is written to calibration_top10.csv (despite the smoke name,
    // since the full 600-combo path uses the same writer).
    BacktestStats run_calibration_smoke(const std::vector<std::string>& archives,
                                        const std::string& csv_path = "calibration_top10.csv");
};

// Free-function entrypoints used by main.cpp and tests.
// WHY: optional db_reporter — when non-null, simulated fills and periodic
// position snapshots are pushed through the same PgReporter the live binary
// uses, so the dashboard can show replay results. Defaults to nullptr for
// existing callers (tests, calibration grid) that don't want PG side-effects.
BacktestStats run_backtest(const infra::Config& cfg,
                           const std::vector<std::string>& archives,
                           const std::string& csv_path = "backtest_results.csv",
                           db::PgReporter* db_reporter = nullptr);

BacktestStats run_calibration_smoke(const infra::Config& cfg,
                                    const std::vector<std::string>& archives,
                                    const std::string& csv_path = "calibration_top10.csv");

}
