#include "infra/config.hpp"

#include <toml++/toml.hpp>

namespace spreadara::infra {

Config load_config(const std::string& path) {
    auto tbl = toml::parse_file(path);
    Config c{};

    c.market_data.symbol = tbl["market_data"]["symbol"].value_or("");
    c.market_data.ws_base_url = tbl["market_data"]["ws_base_url"].value_or("");
    c.market_data.rest_depth_url = tbl["market_data"]["rest_depth_url"].value_or("");
    c.market_data.depth_levels = tbl["market_data"]["depth_levels"].value_or(20);
    c.market_data.volatility_window = tbl["market_data"]["volatility_window"].value_or(100);

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

    c.risk.max_position = tbl["risk"]["max_position"].value_or(0.1);
    c.risk.max_order_size = tbl["risk"]["max_order_size"].value_or(0.05);
    c.risk.price_sanity_pct = tbl["risk"]["price_sanity_pct"].value_or(0.5);
    c.risk.rate_limit_threshold = tbl["risk"]["rate_limit_threshold"].value_or(1000);
    c.risk.max_daily_loss = tbl["risk"]["max_daily_loss"].value_or(500.0);
    c.risk.max_open_orders = tbl["risk"]["max_open_orders"].value_or(10);
    c.risk.max_drawdown_pct = tbl["risk"]["max_drawdown_pct"].value_or(5.0);
    c.risk.max_unhedged_seconds = tbl["risk"]["max_unhedged_seconds"].value_or(30);
    c.risk.max_consecutive_rejections = tbl["risk"]["max_consecutive_rejections"].value_or(5);
    c.risk.circuit_breaker_poll_ms = tbl["risk"]["circuit_breaker_poll_ms"].value_or(100);

    return c;
}

}
