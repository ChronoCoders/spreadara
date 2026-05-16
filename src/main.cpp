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
#include "market_data/okx_ws_client.hpp"
#include "market_data/tick_processor.hpp"
#include "risk/circuit_breaker.hpp"
#include "risk/position_tracker.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/inventory_manager.hpp"
#include "strategy/market_maker.hpp"
#include "strategy/signal_aggregator.hpp"
#include "strategy/spread_model.hpp"
#include "execution/rest_client.hpp"
#include "execution/okx_rest_client.hpp"
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
    bool flag_validate_config = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--backtest") flag_backtest = true;
        else if (a == "--calibration-smoke" || a == "--calibration_smoke")
            flag_calibration_smoke = true;
        else if (a == "--record") flag_record = true;
        else if (a == "--validate-config") flag_validate_config = true;
        else if (!a.empty() && a[0] != '-') cfg_path = a;
    }

    spreadara::infra::Config cfg;
    try {
        cfg = spreadara::infra::load_config(cfg_path);
    } catch (const std::exception& e) {
        std::cerr << "config_load_error: " << e.what() << '\n';
        return 1;
    }

    // Phase 7: --validate-config runs offline checks and exits. Done BEFORE
    // logger init so PASS/FAIL goes to stdout, not the log file.
    if (flag_validate_config) {
        int fails = 0;
        auto report = [&fails](const char* name, bool ok, const std::string& detail = "") {
            if (ok) {
                std::cout << "PASS " << name << '\n';
            } else {
                std::cout << "FAIL " << name << ": " << detail << '\n';
                ++fails;
            }
        };

        // 1. Exchange name set and known. Empty is permitted (back-compat
        // tests construct Config directly with no exchange.name set) but
        // unknown values are rejected to prevent typo-launching the wrong
        // adapter with live keys.
        const bool name_ok = cfg.exchange.name.empty() ||
                             cfg.exchange.name == "binance" ||
                             cfg.exchange.name == "okx";
        report("exchange_name", name_ok,
               "cfg.exchange.name must be 'binance', 'okx', or empty; got '" +
                   cfg.exchange.name + "'");

        // 2. URLs parseable (basic scheme check).
        const std::string ws = cfg.market_data.ws_base_url;
        report("ws_base_url", ws.rfind("wss://", 0) == 0, "expect wss:// scheme: " + ws);
        const std::string rs = cfg.execution.rest_base_url;
        report("rest_base_url", rs.rfind("https://", 0) == 0, "expect https:// scheme: " + rs);

        // 3. Numeric ranges sane.
        report("min_tick_positive", cfg.strategy.min_tick > 0.0,
               "min_tick=" + std::to_string(cfg.strategy.min_tick));
        report("qty_step_positive", cfg.strategy.qty_step > 0.0,
               "qty_step=" + std::to_string(cfg.strategy.qty_step));
        report("symbol_set", !cfg.market_data.symbol.empty(), "market_data.symbol empty");

        // 4. Env vars (per-exchange).
        if (cfg.exchange.name == "okx") {
            for (const char* var : {"SPREADARA_OKX_API_KEY",
                                    "SPREADARA_OKX_API_SECRET",
                                    "SPREADARA_OKX_PASSPHRASE"}) {
                const char* v = std::getenv(var);
                report(var, v != nullptr && *v != '\0', "env var unset");
            }
        } else if (cfg.exchange.name == "binance" || cfg.exchange.name.empty()) {
            for (const char* var : {"SPREADARA_API_KEY", "SPREADARA_API_SECRET"}) {
                const char* v = std::getenv(var);
                report(var, v != nullptr && *v != '\0', "env var unset");
            }
        }
        const char* dsn = std::getenv("SPREADARA_PG_DSN");
        report("SPREADARA_PG_DSN", dsn != nullptr && *dsn != '\0', "env var unset");

        // 5. OKX-specific: contract_size must be > 0.
        if (cfg.exchange.name == "okx") {
            report("contract_size_positive", cfg.exchange.contract_size > 0.0,
                   "exchange.contract_size must be > 0 for OKX");
        }

        return fails == 0 ? 0 : 1;
    }

    spreadara::infra::init_logger(cfg.logging.level, cfg.logging.file);
    spreadara::infra::calibrate_tsc();
    spdlog::info("startup symbol={} exchange={} tsc_ghz={:.3f}",
                 cfg.market_data.symbol, cfg.exchange.name, spreadara::infra::tsc_ghz());

    // WHY: hard-fail on unknown exchange.name. Empty is permitted (defaults to
    // Binance adapter). Anything else (typo, unsupported exchange) exits 4 so
    // a misconfigured production deploy can't silently launch the wrong
    // adapter against live API keys.
    if (!cfg.exchange.name.empty() &&
        cfg.exchange.name != "binance" &&
        cfg.exchange.name != "okx") {
        spdlog::critical("unknown_exchange_name name=\"{}\" supported=binance,okx",
                         cfg.exchange.name);
        return 4;
    }

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
    // Phase 7: OKX uses three env vars (key/secret/passphrase) under separate
    // names. Binance path is untouched.
    spreadara::execution::Credentials creds;
    if (cfg.exchange.name == "okx") {
        const char* key_env = std::getenv("SPREADARA_OKX_API_KEY");
        const char* sec_env = std::getenv("SPREADARA_OKX_API_SECRET");
        const char* pass_env = std::getenv("SPREADARA_OKX_PASSPHRASE");
        if (!key_env || !*key_env || !sec_env || !*sec_env || !pass_env || !*pass_env) {
            spdlog::critical("missing_credentials need_env=SPREADARA_OKX_API_KEY,SPREADARA_OKX_API_SECRET,SPREADARA_OKX_PASSPHRASE");
            return 2;
        }
        creds.api_key = key_env;
        creds.api_secret = sec_env;
        creds.api_passphrase = pass_env;
    } else {
        const char* key_env = std::getenv(api_key_env.c_str());
        const char* sec_env = std::getenv(api_secret_env.c_str());
        if (!key_env || !*key_env || !sec_env || !*sec_env) {
            spdlog::critical("missing_credentials need_env={},{}", api_key_env, api_secret_env);
            return 2;
        }
        creds.api_key = key_env;
        creds.api_secret = sec_env;
    }

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

    // Phase 7: REST adapter dispatch on cfg.exchange.name. Both implement
    // IRestClient; OrderManager takes a reference and never sees the concrete
    // type. Default (empty / "binance") preserves existing behavior.
    std::unique_ptr<spreadara::execution::IRestClient> rest_iface;
    spreadara::execution::RestClient* binance_rest_ptr = nullptr;
    spreadara::execution::okx::OkxRestClient* okx_rest_ptr = nullptr;
    if (cfg.exchange.name == "okx") {
        auto p = std::make_unique<spreadara::execution::okx::OkxRestClient>(
            cfg, creds, &circuit_breaker);
        okx_rest_ptr = p.get();
        okx_rest_ptr->set_reporter(&reporter);
        rest_iface = std::move(p);
    } else {
        auto p = std::make_unique<spreadara::execution::RestClient>(
            cfg, creds, &circuit_breaker);
        binance_rest_ptr = p.get();
        binance_rest_ptr->set_reporter(&reporter);
        rest_iface = std::move(p);
    }
    (void)binance_rest_ptr; (void)okx_rest_ptr;
    spreadara::execution::OrderManager order_manager(cfg, *rest_iface, pos_tracker,
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
            // Phase 8: pull live order-book + strategy + latency telemetry so
            // the dashboard's per-second snapshot row carries actionable data.
            // OrderBook getters return 0.0 / 0.0 before first depth event, which
            // is fine — the dashboard treats 0 mid/spread as "not yet ready".
            const auto& ob = processor.book();
            ev.snap.best_bid = ob.best_bid();
            ev.snap.best_ask = ob.best_ask();
            ev.snap.spread_bps = ob.spread_bps();
            ev.snap.bid_qty = ob.depth_sum(true, 1);
            ev.snap.ask_qty = ob.depth_sum(false, 1);
            ev.snap.volatility = processor.current_volatility();
            ev.snap.gamma = cfg.strategy.gamma;
            ev.snap.k = cfg.strategy.k;
            ev.snap.T = cfg.strategy.horizon;
            double p50 = 0.0, p95 = 0.0, p99 = 0.0;
            order_manager.latency_percentiles(p50, p95, p99);
            ev.snap.lat_p50_us = p50;
            ev.snap.lat_p95_us = p95;
            ev.snap.lat_p99_us = p99;
            ev.snap.open_orders = risk_mgr.open_order_count();
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
                    const auto& sig = aggregator.signals();
                    // WHY: push the live mid into PositionTracker so the
                    // 1 Hz snapshot pusher writes a real mid_price to
                    // position_snapshots, which the dashboard reads. Without
                    // this, live runs show mid_price=0 forever even though
                    // the tick processor is computing it correctly.
                    if (sig.mid > 0.0) pos_tracker.update_mid(sig.mid);
                    mm.on_signals(sig);
                }
            } else {
                // WHY: same rate-limit pattern as OrderManager::quote_loop —
                // don't busy-spin on an empty ring.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
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

    // Phase 7: WS adapter dispatch. Both clients push into the same EventRing;
    // TickProcessor and downstream consumers are exchange-agnostic.
    std::unique_ptr<spreadara::market_data::BinanceWsClient> binance_client;
    std::unique_ptr<spreadara::market_data::okx::OkxWsClient> okx_client;
    auto stop_clients = [&] {
        if (binance_client) binance_client->stop();
        if (okx_client) okx_client->stop();
    };
    if (cfg.exchange.name == "okx") {
        okx_client = std::make_unique<spreadara::market_data::okx::OkxWsClient>(
            ioc, cfg, *ring, fatal_cb);
    } else {
        binance_client = std::make_unique<spreadara::market_data::BinanceWsClient>(
            ioc, cfg, *ring, fatal_cb);
    }

    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code, int) {
        spdlog::info("signal_received shutting_down");
        g_shutdown.store(true);
        // WHY: cancel live orders and (optionally) flatten BEFORE tearing
        // down the WS / io_context so the REST client can still issue calls.
        order_manager.shutdown_cancel_all();
        stop_clients();
        ioc.stop();
    });

    if (okx_client) okx_client->start();
    else binance_client->start();

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
