#include "execution/fix_client.hpp"

#include <spdlog/spdlog.h>

namespace spreadara::execution {

FixClient::FixClient(const infra::Config& cfg, const Credentials& creds)
    : cfg_(cfg), creds_(creds) {
    (void)cfg_;
    (void)creds_;
}

OrderAck FixClient::place_order(const std::string&, double, double, bool,
                                const std::string&) {
    spdlog::error("fix_client_not_implemented method={}", __func__);
    return OrderAck{};
}

CancelAck FixClient::cancel_order(const std::string&, int64_t) {
    spdlog::error("fix_client_not_implemented method={}", __func__);
    return CancelAck{};
}

AmendAck FixClient::amend_order(int64_t, double, double, const std::string&, bool,
                                const std::string&) {
    spdlog::error("fix_client_not_implemented method={}", __func__);
    return AmendAck{};
}

PositionsSnapshot FixClient::query_positions() {
    spdlog::error("fix_client_not_implemented method={}", __func__);
    return PositionsSnapshot{};
}

OpenOrdersSnapshot FixClient::query_open_orders() {
    spdlog::error("fix_client_not_implemented method={}", __func__);
    return OpenOrdersSnapshot{};
}

}
