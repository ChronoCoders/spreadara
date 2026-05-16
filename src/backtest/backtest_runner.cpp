#include "backtest/backtest_runner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <memory>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include "backtest/historical_data_loader.hpp"
#include "backtest/replay_engine.hpp"
#include "execution/order_manager.hpp"
#include "execution/simulated_rest_client.hpp"
#include "market_snapshot_generated.h"
#include "quote_update_generated.h"
#include "risk/circuit_breaker.hpp"
#include "risk/position_tracker.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/inventory_manager.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/signal_aggregator.hpp"
#include "strategy/spread_model.hpp"

namespace spreadara::execution {
// WHY: friend-access trampoline so the backtest runner can drive on_quote
// (which lives behind the private for_testing_* seam) without exposing it as
// part of OrderManager's public API.
class OrderManagerBacktestAccess {
public:
    static void on_quote(OrderManager& om, double bid, double ask, double qty) {
        om.for_testing_on_quote(bid, ask, qty);
    }
};
}  // namespace spreadara::execution

namespace spreadara::backtest {

namespace {

// Lexicographic file name compare gives chronological order for YYYY-MM-DD.
bool path_lex_less(const std::string& a, const std::string& b) { return a < b; }

// Run one backtest with the supplied (possibly tweaked) Config and archives.
BacktestStats run_one(const infra::Config& cfg, const std::vector<std::string>& archives) {
    BacktestReporter reporter(cfg.backtest.initial_capital,
                              cfg.backtest.risk_free_rate,
                              cfg.reporter.flush_interval_ms > 0 ? cfg.reporter.flush_interval_ms : 1000);

    risk::PositionTracker pt;
    risk::RiskManager rm(cfg, pt);
    // WHY: SnapshotRing/QuoteRing are ~8 MB each (16384 * 528B). Heap-allocate
    // so callers running on threads with the default 8 MB stack don't overflow.
    auto risk_ring = std::make_unique<risk::RiskEventRing>();
    risk::CircuitBreaker cb(cfg, pt, rm, risk_ring.get());

    strategy::InventoryManager inv_mgr(cfg);
    strategy::SpreadModel spread_model(cfg);
    strategy::SignalAggregator aggregator;
    auto quote_ring_p = std::make_unique<strategy::QuoteRing>();
    auto& quote_ring = *quote_ring_p;
    strategy::MarketMaker mm(cfg, spread_model, inv_mgr, &quote_ring);
    mm.set_risk(&rm, &cb);

    execution::SimulatedRestClient sim_rest(cfg, cfg.market_data.symbol);
    execution::OrderManager om(cfg, sim_rest, pt, rm, cb, &quote_ring);

    // WHY: route simulator fills directly to PositionTracker for synchronous
    // PnL — OrderManager's fill thread isn't running in single-threaded backtest.
    sim_rest.set_on_fill([&](const risk::FillInput& f) {
        pt.apply_fill(f);
        const double mid = (pt.last_mid() > 0.0) ? pt.last_mid() : f.price;
        reporter.on_fill(f.price, /*spread*/ 0.0, /*maker*/ true);
        (void)mid;
    });

    ReplayEngine engine(cfg);
    // WHY: sample equity on the SIMULATED clock, not wall-clock. A 5-minute
    // synthetic CSV replays in ~2 ms wall, so wall-clock sampling only ever
    // produced one point — Sharpe collapsed to 0 and the calibration grid
    // ranked every combo identically. Sim-clock sampling produces N returns
    // proportional to (sim duration) / (sample_interval), independent of how
    // fast the backtest actually runs.
    uint64_t last_sample_sim_ns = 0;
    const uint64_t sample_interval_ns =
        static_cast<uint64_t>(cfg.reporter.flush_interval_ms > 0
                                  ? cfg.reporter.flush_interval_ms
                                  : 1000) * 1'000'000ULL;

    engine.set_on_record([&](const uint8_t* fb, std::size_t fb_sz, uint64_t ts_ns,
                             double bid, double ask, double bid_qty, double ask_qty) {
        // 1. Update simulator's view of the book (may emit fills).
        if (bid_qty <= 0.0) bid_qty = 0.001;
        if (ask_qty <= 0.0) ask_qty = 0.001;
        sim_rest.update_market(bid, ask, bid_qty, ask_qty, 0);
        pt.update_mid((bid + ask) * 0.5);

        // 2. Run signal aggregator on the snapshot.
        strategy::SnapshotMsg snap_msg;
        if (fb_sz <= snap_msg.bytes.size()) {
            snap_msg.size = static_cast<uint16_t>(fb_sz);
            std::memcpy(snap_msg.bytes.data(), fb, fb_sz);
            if (aggregator.ingest(snap_msg)) {
                // 3. Generate quotes; route through OrderManager test seam.
                mm.on_signals(aggregator.signals());
                strategy::QuoteMsg qm;
                while (quote_ring.pop(qm)) {
                    flatbuffers::Verifier v(qm.bytes.data(), qm.size);
                    if (!schemas::VerifyQuoteUpdateBuffer(v)) continue;
                    auto q = schemas::GetQuoteUpdate(qm.bytes.data());
                    execution::OrderManagerBacktestAccess::on_quote(
                        om, q->bid_price(), q->ask_price(), q->qty());
                }
            }
        }

        // 4. Sample equity at the flush cadence — on the simulated clock.
        if (last_sample_sim_ns == 0) {
            last_sample_sim_ns = ts_ns;
            reporter.on_equity_sample(cfg.backtest.initial_capital + pt.equity());
        } else if (ts_ns > last_sample_sim_ns &&
                   (ts_ns - last_sample_sim_ns) >= sample_interval_ns) {
            reporter.on_equity_sample(cfg.backtest.initial_capital + pt.equity());
            last_sample_sim_ns = ts_ns;
        }
    });

    engine.run_sync(archives, nullptr);
    // Final equity sample.
    reporter.on_equity_sample(cfg.backtest.initial_capital + pt.equity());

    return reporter.finalize();
}

}  // namespace

BacktestRunner::BacktestRunner(const infra::Config& cfg_in) {
    // WHY: stash via copy in run() — kept here for completeness; the run path
    // uses the cfg passed in by the caller.
    (void)cfg_in;
}

std::vector<std::string> BacktestRunner::discover_archives() const {
    // Caller responsibility — empty here so the user can pass discovered set.
    return {};
}

BacktestStats BacktestRunner::run(const std::vector<std::string>& archives_in,
                                  const std::string& csv_path) {
    // Caller passes archives; if empty, we just write an empty report.
    auto archives = archives_in;
    std::sort(archives.begin(), archives.end(), path_lex_less);

    // Use a fresh cfg snapshot from a default-construction is not enough; the
    // caller is expected to have configured this via the entry point that
    // accepts a Config. For now, load with defaults from main wiring.
    spdlog::warn("BacktestRunner::run requires Config; use run_with_config in entrypoint");
    (void)csv_path;
    BacktestStats s{};
    return s;
}

// Free function the entrypoint actually calls — accepts cfg.
BacktestStats run_backtest(const infra::Config& cfg,
                           const std::vector<std::string>& archives_in,
                           const std::string& csv_path) {
    auto archives = archives_in;
    std::sort(archives.begin(), archives.end(), path_lex_less);
    BacktestStats s = run_one(cfg, archives);

    BacktestReporter dummy(cfg.backtest.initial_capital, cfg.backtest.risk_free_rate,
                           cfg.reporter.flush_interval_ms);
    dummy.write_csv(csv_path, s);
    spdlog::info("backtest_done pnl={:.4f} sharpe={:.4f} max_dd_pct={:.4f} fills={} maker_ratio={:.4f}",
                 s.total_pnl, s.sharpe_ratio, s.max_drawdown_pct, s.fill_count, s.maker_ratio);
    return s;
}

BacktestStats BacktestRunner::run_calibration_smoke(const std::vector<std::string>& archives,
                                                    const std::string& csv_path) {
    (void)archives;
    (void)csv_path;
    BacktestStats s{};
    return s;
}

// Calibration smoke: 2x2x2 = 8-combo grid mutating gamma/k/horizon on a copy.
BacktestStats run_calibration_smoke(const infra::Config& cfg,
                                    const std::vector<std::string>& archives,
                                    const std::string& csv_path) {
    const double gammas[2] = {0.05, 0.2};
    const double ks[2] = {1.0, 3.0};
    const double horizons[2] = {1.0, 30.0};

    struct Row { double gamma, k, horizon; BacktestStats s; };
    std::vector<Row> rows;
    rows.reserve(8);
    for (double g : gammas) {
        for (double kk : ks) {
            for (double h : horizons) {
                infra::Config c = cfg;
                c.strategy.gamma = g;
                c.strategy.k = kk;
                c.strategy.horizon = h;
                BacktestStats s = run_one(c, archives);
                rows.push_back({g, kk, h, s});
            }
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.s.sharpe_ratio > b.s.sharpe_ratio; });

    std::FILE* f = std::fopen(csv_path.c_str(), "w");
    if (f) {
        std::fprintf(f, "gamma,k,horizon,total_pnl,sharpe,max_dd_pct,fills,maker_ratio\n");
        for (auto& r : rows) {
            std::fprintf(f, "%.4f,%.4f,%.4f,%.8f,%.6f,%.6f,%zu,%.6f\n",
                         r.gamma, r.k, r.horizon,
                         r.s.total_pnl, r.s.sharpe_ratio, r.s.max_drawdown_pct,
                         r.s.fill_count, r.s.maker_ratio);
        }
        std::fclose(f);
    }
    return rows.empty() ? BacktestStats{} : rows.front().s;
}

}
