// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "infra/logger.hpp"

#include <filesystem>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace spreadara::infra {

namespace {
std::shared_ptr<spdlog::logger> g_logger;

spdlog::level::level_enum parse_level(const std::string& s) {
    if (s == "trace") return spdlog::level::trace;
    if (s == "debug") return spdlog::level::debug;
    if (s == "info") return spdlog::level::info;
    if (s == "warn" || s == "warning") return spdlog::level::warn;
    if (s == "error") return spdlog::level::err;
    if (s == "critical") return spdlog::level::critical;
    return spdlog::level::info;
}
}

void init_logger(const std::string& level, const std::string& file) {
    if (!file.empty()) {
        std::filesystem::path p(file);
        if (p.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
        }
    }

    spdlog::init_thread_pool(8192, 1);

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    if (!file.empty()) {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file, false));
    }

    g_logger = std::make_shared<spdlog::async_logger>(
        "spreadara", sinks.begin(), sinks.end(),
        spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);

    g_logger->set_level(parse_level(level));
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%f] [%^%l%$] %v");
    spdlog::register_logger(g_logger);
    spdlog::set_default_logger(g_logger);
}

std::shared_ptr<spdlog::logger> logger() {
    return g_logger;
}

}
