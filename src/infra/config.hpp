#pragma once

#include <cstdint>
#include <string>

namespace spreadara::infra {

struct Config {
    // Phase 7: generic exchange section. When [exchange].name is non-empty
    // load_config() also populates the legacy market_data / execution / strategy
    // fields below from these values, so downstream code paths keep working
    // unchanged. cfg.exchange.name dispatches the adapter choice in main.cpp.
    struct {
        std::string name;            // "binance" | "okx" | ""
        std::string symbol;
        std::string ws_base_url;
        std::string rest_base_url;
        std::string private_ws_base_url;  // Phase 9: OKX private user-data WS
        double contract_size{0.0};   // BTC per 1 contract (OKX only; 0 = N/A)
        double tick_size{0.0};
        double qty_step{0.0};
    } exchange;

    struct {
        std::string symbol;
        std::string ws_base_url;
        std::string rest_depth_url;
        int depth_levels;
        int volatility_window;
        bool private_ws_enabled{true};  // Phase 9
    } market_data;

    struct {
        std::size_t ring_buffer_capacity;
        std::size_t snapshot_ring_capacity;
        std::size_t quote_ring_capacity;
        std::size_t risk_event_ring_capacity;
        std::size_t fill_ring_capacity;
        std::size_t db_ring_capacity;
    } transport;

    struct {
        int core;
        int batch_size;
        int flush_interval_ms;
        int pg_pool_min;
    } reporter;

    struct {
        int ws_cpu_core;
        int processor_cpu_core;
        int strategy_cpu_core;
        int risk_cpu_core;
        int execution_cpu_core;
        int private_ws_cpu_core;  // Phase 9
    } runtime;

    struct {
        std::string rest_base_url;
        int recv_window_ms;
        int ack_timeout_ms;
        int reconcile_interval_seconds;
        double position_divergence_tolerance;
        double flatten_threshold;
        int http_timeout_ms;
    } execution;

    struct {
        std::string level;
        std::string file;
    } logging;

    struct {
        int initial_backoff_ms;
        int max_backoff_ms;
        int max_attempts;
    } reconnect;

    struct {
        double gamma;
        double k;
        double horizon;
        double min_tick;
        double qty_step;
        double volatility_floor;
        double baseline_volatility;
        double vol_widen_multiplier;
        double depth_threshold;
        double inventory_skew_threshold_pct;
        double max_inventory;
        double max_skew_bps;
        double emergency_unwind_pct;
        double funding_rate;
        int min_requote_ms;
        int price_move_ticks_threshold;
        int quote_lifetime_ms;
        double quote_qty;
    } strategy;

    struct {
        bool enabled;
        std::string data_dir;
        int replay_speed;
        std::string start_date;
        std::string end_date;
        double initial_capital;
        double risk_free_rate;
        bool enable_calibration;
    } backtest;

    struct {
        bool enabled;
        std::string ws_base_url;
        std::string rest_base_url;
        std::string private_ws_base_url;  // Phase 9
    } testnet;

    struct {
        double max_position;
        double max_order_size;
        double price_sanity_pct;
        int rate_limit_threshold;
        double max_daily_loss;
        int max_open_orders;
        double max_drawdown_pct;
        // WHY: percent-drawdown is meaningless when peak equity is tiny — a
        // $0.20 dip on a $0.45 peak reads as 46% but is just intraday noise
        // on a low-volume session. Gate the percent check on peak_equity_
        // exceeding this floor so the CB doesn't false-alarm at session
        // start.
        double drawdown_min_equity;
        int max_unhedged_seconds;
        int max_consecutive_rejections;
        int circuit_breaker_poll_ms;
    } risk;
};

Config load_config(const std::string& path);

}
