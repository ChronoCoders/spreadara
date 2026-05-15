#pragma once

#include <cstdint>
#include <string>

namespace spreadara::infra {

struct Config {
    struct {
        std::string symbol;
        std::string ws_base_url;
        std::string rest_depth_url;
        int depth_levels;
        int volatility_window;
    } market_data;

    struct {
        std::size_t ring_buffer_capacity;
    } transport;

    struct {
        int ws_cpu_core;
        int processor_cpu_core;
    } runtime;

    struct {
        std::string level;
        std::string file;
    } logging;

    struct {
        int initial_backoff_ms;
        int max_backoff_ms;
        int max_attempts;
    } reconnect;
};

Config load_config(const std::string& path);

}
