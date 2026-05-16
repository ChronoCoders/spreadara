#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <curl/curl.h>
#include <spdlog/spdlog.h>

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

namespace {
std::atomic<bool> g_shutdown{false};
boost::asio::io_context* g_ioc_ptr{nullptr};
}

int main(int argc, char** argv) {
    const std::string cfg_path = (argc > 1) ? argv[1] : "config/config.toml";

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

    // WHY: credentials MUST come from env. Read once here so they never enter
    // any spdlog format call, error message, or config-derived code path.
    if (!spreadara::execution::credentials_present()) {
        spdlog::critical("missing_credentials need_env=SPREADARA_API_KEY,SPREADARA_API_SECRET");
        return 2;
    }
    spreadara::execution::Credentials creds{
        std::getenv("SPREADARA_API_KEY"),
        std::getenv("SPREADARA_API_SECRET"),
    };

    curl_global_init(CURL_GLOBAL_DEFAULT);

    auto ring = std::make_unique<spreadara::market_data::EventRing>();
    auto snap_ring = std::make_unique<spreadara::strategy::SnapshotRing>();
    auto quote_ring = std::make_unique<spreadara::strategy::QuoteRing>();

    spreadara::market_data::TickProcessor processor(cfg, *ring, snap_ring.get());
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
    curl_global_cleanup();
    spdlog::shutdown();
    return 0;
}
