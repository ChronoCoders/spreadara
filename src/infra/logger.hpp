#pragma once

#include <memory>
#include <string>

#include <spdlog/spdlog.h>

namespace spreadara::infra {

void init_logger(const std::string& level, const std::string& file);
std::shared_ptr<spdlog::logger> logger();

}
