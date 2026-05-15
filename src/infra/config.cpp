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

    c.runtime.ws_cpu_core = tbl["runtime"]["ws_cpu_core"].value_or(-1);
    c.runtime.processor_cpu_core = tbl["runtime"]["processor_cpu_core"].value_or(-1);

    c.logging.level = tbl["logging"]["level"].value_or("info");
    c.logging.file = tbl["logging"]["file"].value_or("spreadara.log");

    c.reconnect.initial_backoff_ms = tbl["reconnect"]["initial_backoff_ms"].value_or(500);
    c.reconnect.max_backoff_ms = tbl["reconnect"]["max_backoff_ms"].value_or(30000);
    c.reconnect.max_attempts = tbl["reconnect"]["max_attempts"].value_or(5);

    return c;
}

}
