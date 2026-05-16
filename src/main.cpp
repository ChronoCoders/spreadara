#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include "backtest/backtest_runner.hpp"
#include "backtest/historical_data_loader.hpp"
#include "backtest/recorder.hpp"
#include "infra/config.hpp"
#include "infra/cpu_affinity.hpp"
#include "infra/logger.hpp"
#include "infra/rdtsc.hpp"
#include "market_data/binance_ws_client.hpp"
#include "market_data/tick_processor.hpp"
#include "risk/circuit_breaker.hpp"
#include "risk/position_tracker.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/inventory_manager.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/signal_aggregator.hpp"
#include "strategy/spread_model.hpp"
#include "execution/rest_client.hpp"
#include "execution/order_manager.hpp"
#include "db/pg_reporter.hpp"

// Phase-6 testnet validation note:
//   When [testnet] enabled=true in config.toml, the ws/rest URLs are swapped
//   and credentials are read from SPREADARA_TESTNET_API_KEY/_SECRET instead of
//   the production env vars. To validate end-to-end on Binance Futures testnet:
//     1. Register at https://testnet.binancefuture.com and create API keys.
//     2. export SPREADARA_TESTNET_API_KEY=<key>
//        export SPREADARA_TESTNET_API_SECRET=<secret>
//     3. Set [testnet] enabled = true in config/config.toml.
//     4. Run ./spreadara — log line "using_testnet=true ..." confirms wiring.
//     5. Manually post a small order via the web UI; verify the position
//        snapshot in Postgres reflects it after reconcile_interval_seconds.

namespace {
std::atomic<bool> g_shutdown{false};
boost::asio::io_context* g_ioc_ptr{nullptr};
}

int main(int argc, char** argv) {
    // WHY: positional first arg = config path (back-compat). Remaining argv
    // is parsed for Phase-6 flags so existing invocations stay unchanged.
    std::string cfg_path = "config/config.toml";
    bool flag_backtest = false;
    bool flag_calibration_smoke = false;
    bool flag_record = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--backtest") flag_backtest = true;
        else if (a == "--calibration-smoke" || a == "--calibration_smoke")
            flag_calibration_smoke = true;
        else if (a == "--record") flag_record = true;
        else if (!a.empty() && a[0] != '-') cfg_path = a;
    }

    spreadara::infra::Config cfg;
    try {
        cfg = spreadara::infra::load_config(cfg_path);
    } catch (const std::exception& e) {
        std::cerr << "config_load_error: " << e.what() << '\n';
        return 1;
    }

    spreadara::infra::init_logger(cfg.logging.level, cfg.logging.file);
    spreadara::infra::calibrate_tsc();
    spdlog::info("startup symbol={} tsc_ghz={:.3f}", cfg.market_data.symbol, spreadara::infra::tsc_ghz());

    // WHY: testnet URL+cred override happens BEFORE any client / credential
    // read. cfg is mutated in-place so all downstream wiring sees the swap.
    // testnet creds NEVER fall back to production creds — fail loudly if
    // SPREADARA_TESTNET_API_KEY/SECRET aren't set, so production keys can't be
    // accidentally used against testnet endpoints.
    const bool using_testnet = cfg.testnet.enabled;
    std::string api_key_env = "SPREADARA_API_KEY";
    std::string api_secret_env = "SPREADARA_API_SECRET";
    if (using_testnet) {
        if (!cfg.testnet.ws_base_url.empty())
            cfg.market_data.ws_base_url = cfg.testnet.ws_base_url;
        if (!cfg.testnet.rest_base_url.empty())
            cfg.execution.rest_base_url = cfg.testnet.rest_base_url;
        api_key_env = "SPREADARA_TESTNET_API_KEY";
        api_secret_env = "SPREADARA_TESTNET_API_SECRET";
        spdlog::info("using_testnet=true ws={} rest={}",
                     cfg.market_data.ws_base_url, cfg.execution.rest_base_url);
    }

    // Phase-6 backtest path — short-circuits the live wiring entirely.
    if (flag_backtest || flag_calibration_smoke) {
        namespace fs = std::filesystem;
        std::vector<std::string> archives;
        for (const auto& e : fs::directory_iterator(cfg.backtest.data_dir)) {
            const auto p = e.path();
            if (p.extension() == ".bin" &&
                p.filename().string().find(cfg.market_data.symbol) != std::string::npos) {
                archives.push_back(p.string());
            }
        }
        // WHY: if no .bin archives present, auto-convert any .csv in the dir.
        if (archives.empty()) {
            for (const auto& e : fs::directory_iterator(cfg.backtest.data_dir)) {
                const auto p = e.path();
                if (p.extension() == ".csv" &&
                    p.filename().string().find(cfg.market_data.symbol) != std::string::npos) {
                    auto bin = p; bin.replace_extension(".bin");
                    spreadara::backtest::HistoricalDataLoader loader(cfg);
                    if (loader.load_csv(p.string(), bin.string())) {
                        archives.push_back(bin.string());
                    }
                }
            }
        }
        std::sort(archives.begin(), archives.end());
        if (archives.empty()) {
            spdlog::error("backtest_no_archives dir={} symbol={}",
                          cfg.backtest.data_dir, cfg.market_data.symbol);
            return 3;
        }
        if (flag_calibration_smoke) {
            auto s = spreadara::backtest::run_calibration_smoke(cfg, archives);
            std::cout << "calibration_smoke_best pnl=" << s.total_pnl
                      << " sharpe=" << s.sharpe_ratio
                      << " max_dd_pct=" << s.max_drawdown_pct
                      << " fills=" << s.fill_count << '\n';
        } else {
            auto s = spreadara::backtest::run_backtest(cfg, archives);
            std::cout << "backtest_done pnl=" << s.total_pnl
                      << " sharpe=" << s.sharpe_ratio
                      << " max_dd_pct=" << s.max_drawdown_pct
                      << " fills=" << s.fill_count
                      << " maker_ratio=" << s.maker_ratio
                      << " initial_capital=" << s.initial_capital
                      << " final_equity=" << s.final_equity << '\n';
        }
        spdlog::shutdown();
        return 0;
    }

    // WHY: credentials MUST come from env. Read once here so they never enter
    // any spdlog format call, error message, or config-derived code path.
    const char* key_env = std::getenv(api_key_env.c_str());
    const char* sec_env = std::getenv(api_secret_env.c_str());
    if (!key_env || !*key_env || !sec_env || !*sec_env) {
        spdlog::critical("missing_credentials need_env={},{}", api_key_env, api_secret_env);
        return 2;
    }
    spreadara::execution::Credentials creds{key_env, sec_env};

    curl_global_init(CURL_GLOBAL_DEFAULT);

    auto ring = std::make_unique<spreadara::market_data::EventRing>();
    auto snap_ring = std::make_unique<spreadara::strategy::SnapshotRing>();
    auto quote_ring = std::make_unique<spreadara::strategy::QuoteRing>();

    // WHY: --record opens a second SnapshotRing that the TickProcessor also
    // pushes to (the strategy SPSC consumer is untouched). The Recorder pops
    // from that second ring on its own thread and writes length-prefixed FB
    // records to data/recorded/<UTC>.bin.
    std::unique_ptr<spreadara::strategy::SnapshotRing> record_ring;
    std::unique_ptr<spreadara::backtest::Recorder> recorder;
    if (flag_record) {
        record_ring = std::make_unique<spreadara::strategy::SnapshotRing>();
        std::filesystem::create_directories("data/recorded");
        const std::time_t tt = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now());
        std::tm tm_buf{};
        gmtime_r(&tt, &tm_buf);
        std::ostringstream oss;
        oss << "data/recorded/"
            << std::put_time(&tm_buf, "%Y%m%d_%H%M%S") << ".bin";
        recorder = std::make_unique<spreadara::backtest::Recorder>(oss.str(), record_ring.get());
        recorder->start();
        spdlog::info("recorder_started path={}", oss.str());
    }

    spreadara::market_data::TickProcessor processor(cfg, *ring, snap_ring.get(),
                                                    record_ring.get());
    processor.start();

    spreadara::strategy::InventoryManager inv_mgr(cfg);
    spreadara::strategy::SpreadModel spread_model(cfg);
    spreadara::strategy::SignalAggregator aggregator;
    spreadara::strategy::MarketMaker mm(cfg, spread_model, inv_mgr, quote_ring.get());

    spreadara::risk::PositionTracker pos_tracker;
    spreadara::risk::RiskManager risk_mgr(cfg, pos_tracker);
    auto risk_event_ring = std::make_unique<spreadara::risk::RiskEventRing>();
    spreadara::risk::CircuitBreaker circuit_breaker(cfg, pos_tracker, risk_mgr, risk_event_ring.get());
    mm.set_risk(&risk_mgr, &circuit_breaker);
    circuit_breaker.start();

    // WHY: Phase-5 Postgres reporter. DSN comes ONLY from env. If unset, the
    // reporter runs in dry mode (logs warn at startup; never blocks trading).
    const char* pg_dsn_env = std::getenv("SPREADARA_PG_DSN");
    const std::string pg_dsn = (pg_dsn_env && *pg_dsn_env) ? pg_dsn_env : "";
    if (pg_dsn.empty()) {
        spdlog::warn("SPREADARA_PG_DSN_unset reporter_dry_mode");
    }
    auto db_ring = std::make_unique<spreadara::db::DbEventRing>();
    spreadara::db::PgReporter reporter(cfg, *db_ring, pg_dsn);
    reporter.start();
    circuit_breaker.set_reporter(&reporter);
    // WHY: rejection-path observability — wired AFTER reporter exists,
    // BEFORE the order_manager starts producing pre_trade_check traffic.
    risk_mgr.set_reporter(&reporter);

    spreadara::execution::RestClient rest_client(cfg, creds, &circuit_breaker);
    rest_client.set_reporter(&reporter);
    spreadara::execution::OrderManager order_manager(cfg, rest_client, pos_tracker,
                                                     risk_mgr, circuit_breaker,
                                                     quote_ring.get());
    order_manager.set_reporter(&reporter);
    order_manager.start();

    // Periodic position-snapshot + daily-pnl pusher. ~1 Hz.
    std::atomic<bool> snap_running{true};
    std::thread snap_thread([&] {
        while (snap_running.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint64_t ts_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            const double realized = pos_tracker.realized_pnl();
            const double unreal = pos_tracker.unrealized_pnl();
            const double fees = pos_tracker.total_fees();
            spreadara::db::DbEvent ev{};
            ev.kind = spreadara::db::DbEventKind::PositionSnapshot;
            ev.snap.ts_ns = ts_ns;
            ev.snap.inventory = pos_tracker.current_inventory();
            ev.snap.avg_entry = pos_tracker.avg_entry();
            ev.snap.realized = realized;
            ev.snap.unrealized = unreal;
            ev.snap.fees = fees;
            ev.snap.mid = pos_tracker.last_mid();
            (void)reporter.push(ev);

            // WHY: push every second; the reporter coalesces same-date
            // DailyPnl events in memory and only writes a row on UTC-day
            // rollover (or final drain at shutdown).
            const std::time_t tt = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            std::tm tm{};
            gmtime_r(&tt, &tm);
            const int32_t date_i =
                (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
            spreadara::db::DbEvent dev{};
            dev.kind = spreadara::db::DbEventKind::DailyPnl;
            dev.daily.date = date_i;
            dev.daily.realized = realized;
            dev.daily.unrealized = unreal;
            dev.daily.fees = fees;
            dev.daily.total = realized + unreal - fees;
            (void)reporter.push(dev);
        }
    });

    std::atomic<bool> strat_running{true};
    std::thread strat_thread([&] {
        if (cfg.runtime.strategy_cpu_core >= 0) {
            spreadara::infra::pin_current_thread_to_core(cfg.runtime.strategy_cpu_core);
        }
        spreadara::strategy::SnapshotMsg msg;
        while (strat_running.load(std::memory_order_acquire)) {
            if (snap_ring->pop(msg)) {
                if (aggregator.ingest(msg)) {
                    mm.on_signals(aggregator.signals());
                }
            } else {
                std::this_thread::yield();
            }
        }
    });

    boost::asio::io_context ioc;
    g_ioc_ptr = &ioc;

    auto fatal_cb = [&circuit_breaker] {
        spdlog::critical("fatal_callback_triggered exiting");
        circuit_breaker.notify_ws_disconnect_exhausted();
        if (g_ioc_ptr) g_ioc_ptr->stop();
        g_shutdown.store(true);
    };

    spreadara::market_data::BinanceWsClient client(ioc, cfg, *ring, fatal_cb);

    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code, int) {
        spdlog::info("signal_received shutting_down");
        g_shutdown.store(true);
        // WHY: cancel live orders and (optionally) flatten BEFORE tearing
        // down the WS / io_context so the REST client can still issue calls.
        order_manager.shutdown_cancel_all();
        client.stop();
        ioc.stop();
    });

    client.start();

    std::thread ws_thread([&] {
        if (cfg.runtime.ws_cpu_core >= 0) {
            spreadara::infra::pin_current_thread_to_core(cfg.runtime.ws_cpu_core);
        }
        ioc.run();
    });

    ws_thread.join();
    processor.stop();
    strat_running.store(false, std::memory_order_release);
    if (strat_thread.joinable()) strat_thread.join();
    snap_running.store(false, std::memory_order_release);
    if (snap_thread.joinable()) snap_thread.join();
    order_manager.stop();
    circuit_breaker.stop();
    reporter.stop();
    if (recorder) recorder->stop();
    curl_global_cleanup();
    spdlog::shutdown();
    return 0;
}
