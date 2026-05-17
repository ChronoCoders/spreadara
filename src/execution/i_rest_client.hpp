#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spreadara::execution {

// Credentials struct passes the API key+secret as const refs.
// WHY: keeps the values off any spdlog format-string path and confines
// their lifetime to the components that actually need them.
struct Credentials {
    std::string api_key;
    std::string api_secret;
    // Phase 7: OKX requires a third credential ("passphrase") set when the
    // API key was created. Default-initialized so existing {key, secret}
    // brace-init sites don't trigger -Wmissing-field-initializers.
    std::string api_passphrase{};
};

bool credentials_present();

struct OrderAck {
    bool ok{false};
    int64_t exchange_order_id{0};
    std::string client_order_id;
    std::string status;  // NEW, PARTIALLY_FILLED, FILLED, ...
    double price{0.0};
    double qty{0.0};
    int http_code{0};
    int exchange_code{0};
};

struct CancelAck {
    bool ok{false};
    int64_t exchange_order_id{0};
    std::string client_order_id;
    std::string status;
    int http_code{0};
    int exchange_code{0};
};

struct AmendAck {
    bool ok{false};
    // WHY: set when cancel succeeded but the replacement place_order failed —
    // original is gone but no new order exists. Caller must transition the
    // slot to CANCELED rather than the would-be-replacement state.
    bool cancelled_only{false};
    int64_t exchange_order_id{0};
    std::string client_order_id;
    double price{0.0};
    double qty{0.0};
    int http_code{0};
    int exchange_code{0};
};

struct PositionEntry {
    std::string symbol;
    double position_amt{0.0};
    double entry_price{0.0};
    double unrealized_profit{0.0};
};

struct PositionsSnapshot {
    bool ok{false};
    std::vector<PositionEntry> positions;
    int http_code{0};
};

struct OpenOrderEntry {
    int64_t exchange_order_id{0};
    std::string client_order_id;
    std::string symbol;
    std::string side;
    double price{0.0};
    double orig_qty{0.0};
    double executed_qty{0.0};
    std::string status;
};

struct OpenOrdersSnapshot {
    bool ok{false};
    std::vector<OpenOrderEntry> orders;
    int http_code{0};
};

// WHY: pure-virtual seam so OrderManager can be wired to a SimulatedRestClient
// (backtest) without dragging in libcurl or live state.
class IRestClient {
public:
    virtual ~IRestClient() = default;
    virtual OrderAck place_order(const std::string& side, double qty, double price,
                                 bool post_only, const std::string& client_order_id) = 0;
    virtual OrderAck place_market_order(const std::string& side, double qty,
                                        const std::string& client_order_id) = 0;
    virtual CancelAck cancel_order(const std::string& client_order_id,
                                   int64_t exchange_order_id) = 0;
    virtual AmendAck amend_order(int64_t exchange_order_id, double new_price, double new_qty,
                                 const std::string& side, bool post_only,
                                 const std::string& replacement_client_order_id) = 0;
    virtual PositionsSnapshot query_positions() = 0;
    virtual OpenOrdersSnapshot query_open_orders() = 0;
};

}
