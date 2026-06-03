// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "infra/config.hpp"

#include <toml++/toml.hpp>

namespace spreadara::infra {

Config load_config(const std::string& path) {
    auto tbl = toml::parse_file(path);
    Config c{};

    c.exchange.name = tbl["exchange"]["name"].value_or("");
    c.exchange.symbol = tbl["exchange"]["symbol"].value_or("");
    c.exchange.ws_base_url = tbl["exchange"]["ws_base_url"].value_or("");
    c.exchange.rest_base_url = tbl["exchange"]["rest_base_url"].value_or("");
    c.exchange.private_ws_base_url = tbl["exchange"]["private_ws_base_url"].value_or(
        "wss://ws.okx.com:8443/ws/v5/private");
    c.exchange.contract_size = tbl["exchange"]["contract_size"].value_or(0.0);
    c.exchange.tick_size = tbl["exchange"]["tick_size"].value_or(0.0);
    c.exchange.qty_step = tbl["exchange"]["qty_step"].value_or(0.0);

    c.market_data.symbol = tbl["market_data"]["symbol"].value_or("");
    c.market_data.ws_base_url = tbl["market_data"]["ws_base_url"].value_or("");
    c.market_data.rest_depth_url = tbl["market_data"]["rest_depth_url"].value_or("");
    c.market_data.depth_levels = tbl["market_data"]["depth_levels"].value_or(20);
    c.market_data.volatility_window = tbl["market_data"]["volatility_window"].value_or(100);
    c.market_data.private_ws_enabled = tbl["market_data"]["private_ws_enabled"].value_or(true);

    c.transport.ring_buffer_capacity =
        static_cast<std::size_t>(tbl["transport"]["ring_buffer_capacity"].value_or<int64_t>(65536));
    c.transport.snapshot_ring_capacity =
        static_cast<std::size_t>(tbl["transport"]["snapshot_ring_capacity"].value_or<int64_t>(16384));
    c.transport.quote_ring_capacity =
        static_cast<std::size_t>(tbl["transport"]["quote_ring_capacity"].value_or<int64_t>(16384));
    c.transport.risk_event_ring_capacity =
        static_cast<std::size_t>(tbl["transport"]["risk_event_ring_capacity"].value_or<int64_t>(1024));
    c.transport.fill_ring_capacity =
        static_cast<std::size_t>(tbl["transport"]["fill_ring_capacity"].value_or<int64_t>(1024));
    c.transport.db_ring_capacity =
        static_cast<std::size_t>(tbl["transport"]["db_ring_capacity"].value_or<int64_t>(4096));

    c.reporter.core = tbl["reporter"]["core"].value_or(-1);
    c.reporter.batch_size = tbl["reporter"]["batch_size"].value_or(100);
    c.reporter.flush_interval_ms = tbl["reporter"]["flush_interval_ms"].value_or(1000);
    c.reporter.pg_pool_min = tbl["reporter"]["pg_pool_min"].value_or(2);

    c.runtime.ws_cpu_core = tbl["runtime"]["ws_cpu_core"].value_or(-1);
    c.runtime.processor_cpu_core = tbl["runtime"]["processor_cpu_core"].value_or(-1);
    c.runtime.strategy_cpu_core = tbl["runtime"]["strategy_cpu_core"].value_or(-1);
    c.runtime.risk_cpu_core = tbl["runtime"]["risk_cpu_core"].value_or(-1);
    c.runtime.execution_cpu_core = tbl["runtime"]["execution_cpu_core"].value_or(-1);
    c.runtime.private_ws_cpu_core = tbl["runtime"]["private_ws_cpu_core"].value_or(-1);

    c.execution.rest_base_url = tbl["execution"]["rest_base_url"].value_or("");
    c.execution.recv_window_ms = tbl["execution"]["recv_window_ms"].value_or(5000);
    c.execution.ack_timeout_ms = tbl["execution"]["ack_timeout_ms"].value_or(2000);
    c.execution.reconcile_interval_seconds =
        tbl["execution"]["reconcile_interval_seconds"].value_or(300);
    c.execution.position_divergence_tolerance =
        tbl["execution"]["position_divergence_tolerance"].value_or(0.001);
    c.execution.flatten_threshold = tbl["execution"]["flatten_threshold"].value_or(0.001);
    c.execution.http_timeout_ms = tbl["execution"]["http_timeout_ms"].value_or(3000);

    c.logging.level = tbl["logging"]["level"].value_or("info");
    c.logging.file = tbl["logging"]["file"].value_or("spreadara.log");

    c.reconnect.initial_backoff_ms = tbl["reconnect"]["initial_backoff_ms"].value_or(500);
    c.reconnect.max_backoff_ms = tbl["reconnect"]["max_backoff_ms"].value_or(30000);
    c.reconnect.max_attempts = tbl["reconnect"]["max_attempts"].value_or(5);

    c.strategy.gamma = tbl["strategy"]["gamma"].value_or(0.1);
    c.strategy.k = tbl["strategy"]["k"].value_or(1.5);
    c.strategy.horizon = tbl["strategy"]["horizon"].value_or(1.0);
    c.strategy.min_tick = tbl["strategy"]["min_tick"].value_or(0.1);
    c.strategy.qty_step = tbl["strategy"]["qty_step"].value_or(0.001);
    c.strategy.volatility_floor = tbl["strategy"]["volatility_floor"].value_or(0.0001);
    c.strategy.baseline_volatility = tbl["strategy"]["baseline_volatility"].value_or(0.0002);
    c.strategy.vol_widen_multiplier = tbl["strategy"]["vol_widen_multiplier"].value_or(1.5);
    c.strategy.depth_threshold = tbl["strategy"]["depth_threshold"].value_or(10.0);
    c.strategy.inventory_skew_threshold_pct =
        tbl["strategy"]["inventory_skew_threshold_pct"].value_or(30.0);
    c.strategy.max_inventory = tbl["strategy"]["max_inventory"].value_or(5.0);
    c.strategy.max_skew_bps = tbl["strategy"]["max_skew_bps"].value_or(25.0);
    c.strategy.emergency_unwind_pct = tbl["strategy"]["emergency_unwind_pct"].value_or(90.0);
    c.strategy.funding_rate = tbl["strategy"]["funding_rate"].value_or(0.0);
    c.strategy.min_requote_ms = tbl["strategy"]["min_requote_ms"].value_or(50);
    c.strategy.price_move_ticks_threshold = tbl["strategy"]["price_move_ticks_threshold"].value_or(2);
    c.strategy.quote_lifetime_ms = tbl["strategy"]["quote_lifetime_ms"].value_or(5000);
    c.strategy.quote_qty = tbl["strategy"]["quote_qty"].value_or(0.01);

    c.backtest.enabled = tbl["backtest"]["enabled"].value_or(false);
    c.backtest.data_dir = tbl["backtest"]["data_dir"].value_or("data/historical");
    c.backtest.replay_speed = tbl["backtest"]["replay_speed"].value_or(100);
    c.backtest.start_date = tbl["backtest"]["start_date"].value_or("");
    c.backtest.end_date = tbl["backtest"]["end_date"].value_or("");
    c.backtest.initial_capital = tbl["backtest"]["initial_capital"].value_or(10000.0);
    c.backtest.risk_free_rate = tbl["backtest"]["risk_free_rate"].value_or(0.05);
    c.backtest.enable_calibration = tbl["backtest"]["enable_calibration"].value_or(false);

    c.testnet.enabled = tbl["testnet"]["enabled"].value_or(false);
    c.testnet.ws_base_url = tbl["testnet"]["ws_base_url"].value_or("");
    c.testnet.rest_base_url = tbl["testnet"]["rest_base_url"].value_or("");
    c.testnet.private_ws_base_url = tbl["testnet"]["private_ws_base_url"].value_or(
        "wss://wspap.okx.com:8443/ws/v5/private?brokerId=9999");

    c.risk.max_position = tbl["risk"]["max_position"].value_or(0.1);
    c.risk.max_order_size = tbl["risk"]["max_order_size"].value_or(0.05);
    c.risk.price_sanity_pct = tbl["risk"]["price_sanity_pct"].value_or(0.5);
    c.risk.rate_limit_threshold = tbl["risk"]["rate_limit_threshold"].value_or(1000);
    c.risk.max_daily_loss = tbl["risk"]["max_daily_loss"].value_or(500.0);
    c.risk.max_open_orders = tbl["risk"]["max_open_orders"].value_or(10);
    c.risk.max_drawdown_pct = tbl["risk"]["max_drawdown_pct"].value_or(5.0);
    c.risk.drawdown_min_equity = tbl["risk"]["drawdown_min_equity"].value_or(100.0);
    c.risk.max_unhedged_seconds = tbl["risk"]["max_unhedged_seconds"].value_or(30);
    c.risk.max_consecutive_rejections = tbl["risk"]["max_consecutive_rejections"].value_or(5);
    c.risk.circuit_breaker_poll_ms = tbl["risk"]["circuit_breaker_poll_ms"].value_or(100);

    // When [exchange] is present, mirror its values into the legacy
    // fields so the existing market_data / execution / strategy code keeps
    // reading the same struct slots. Test fixtures that build Config directly
    // leave exchange.name="" and are unaffected.
    if (!c.exchange.name.empty()) {
        if (!c.exchange.symbol.empty())       c.market_data.symbol = c.exchange.symbol;
        if (!c.exchange.ws_base_url.empty())  c.market_data.ws_base_url = c.exchange.ws_base_url;
        if (!c.exchange.rest_base_url.empty()) c.execution.rest_base_url = c.exchange.rest_base_url;
        if (c.exchange.tick_size > 0.0)       c.strategy.min_tick = c.exchange.tick_size;
        if (c.exchange.qty_step > 0.0)        c.strategy.qty_step = c.exchange.qty_step;
    }

    return c;
}

}
