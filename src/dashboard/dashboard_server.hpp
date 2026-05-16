#pragma once

// WHY: The HTTP/WebSocket dashboard server is implemented in
// dashboard_backend/main.go (Go sidecar) and reads only from Postgres.
// This C++ class exists as a placeholder for an in-process metrics endpoint
// if ever needed. All methods log "dashboard_server_not_implemented" and
// return false. ~30 lines, no real code.

#include <string>

namespace spreadara::dashboard {

class DashboardServer {
public:
    DashboardServer() = default;
    ~DashboardServer() = default;

    DashboardServer(const DashboardServer&) = delete;
    DashboardServer& operator=(const DashboardServer&) = delete;

    bool start(const std::string& bind_host, int port);
    bool stop();
};

}
