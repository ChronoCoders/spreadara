// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "dashboard/dashboard_server.hpp"

#include <spdlog/spdlog.h>

namespace spreadara::dashboard {

bool DashboardServer::start(const std::string& bind_host, int port) {
    (void)bind_host;
    (void)port;
    spdlog::info("dashboard_server_not_implemented use_go_sidecar=dashboard_backend");
    return false;
}

bool DashboardServer::stop() {
    spdlog::info("dashboard_server_not_implemented");
    return false;
}

}
