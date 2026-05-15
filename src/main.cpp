#include <atomic>
#include <csignal>
#include <cstdlib>
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

    curl_global_init(CURL_GLOBAL_DEFAULT);

    auto ring = std::make_unique<spreadara::market_data::EventRing>();
    spreadara::market_data::TickProcessor processor(cfg, *ring);
    processor.start();

    boost::asio::io_context ioc;
    g_ioc_ptr = &ioc;

    auto fatal_cb = [] {
        spdlog::critical("fatal_callback_triggered exiting");
        if (g_ioc_ptr) g_ioc_ptr->stop();
        g_shutdown.store(true);
    };

    spreadara::market_data::BinanceWsClient client(ioc, cfg, *ring, fatal_cb);

    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code, int) {
        spdlog::info("signal_received shutting_down");
        g_shutdown.store(true);
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
    curl_global_cleanup();
    spdlog::shutdown();
    return 0;
}
