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
    } testnet;

    struct {
        double max_position;
        double max_order_size;
        double price_sanity_pct;
        int rate_limit_threshold;
        double max_daily_loss;
        int max_open_orders;
        double max_drawdown_pct;
        int max_unhedged_seconds;
        int max_consecutive_rejections;
        int circuit_breaker_poll_ms;
    } risk;
};

Config load_config(const std::string& path);

}
