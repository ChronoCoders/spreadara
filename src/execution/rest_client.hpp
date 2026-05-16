#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "infra/config.hpp"

typedef void CURL;

namespace spreadara::risk {
class CircuitBreaker;
}

namespace spreadara::db {
class PgReporter;
}

namespace spreadara::execution {

// Credentials struct passes the API key+secret as const refs.
// WHY: keeps the values off any spdlog format-string path and confines
// their lifetime to the components that actually need them.
struct Credentials {
    std::string api_key;
    std::string api_secret;
    // Phase 7: OKX requires a third credential ("passphrase") set when the
    // API key was created. Empty for Binance. Default-initialized so existing
    // {key, secret} brace-init sites don't trigger -Wmissing-field-initializers.
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
    int binance_code{0};
};

struct CancelAck {
    bool ok{false};
    int64_t exchange_order_id{0};
    std::string client_order_id;
    std::string status;
    int http_code{0};
    int binance_code{0};
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
    int binance_code{0};
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
// (backtest) without dragging in libcurl or live state. RestClient implements it.
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

// NOT thread-safe. One instance per thread, or external mutex.
// WHY: persistent CURL* handles are reused across calls and are not safe
// to share across threads without locking.
class RestClient : public IRestClient {
public:
    RestClient(const infra::Config& cfg, const Credentials& creds,
               risk::CircuitBreaker* cb);
    ~RestClient() override;

    RestClient(const RestClient&) = delete;
    RestClient& operator=(const RestClient&) = delete;

    // side: "BUY" or "SELL". post_only=true => timeInForce=GTX.
    OrderAck place_order(const std::string& side, double qty, double price,
                         bool post_only, const std::string& client_order_id) override;
    // WHY: MARKET orders are the on_halt flatten path. Binance futures MARKET
    // implies IOC behavior; no price, no timeInForce field accepted.
    OrderAck place_market_order(const std::string& side, double qty,
                                const std::string& client_order_id) override;
    CancelAck cancel_order(const std::string& client_order_id, int64_t exchange_order_id) override;
    // WHY: Binance amend has narrow eligibility (LIMIT GTC/GTX only, no price+qty
    // jumping a tick band). On any non-2xx we fall back to cancel+place internally.
    AmendAck amend_order(int64_t exchange_order_id, double new_price, double new_qty,
                         const std::string& side, bool post_only,
                         const std::string& replacement_client_order_id) override;

    PositionsSnapshot query_positions() override;
    OpenOrdersSnapshot query_open_orders() override;

    // Test seam: feeds a synthetic (http_code, body, endpoint) tuple through the
    // same response handler the real call uses. Exercises geoblock path without
    // needing live HTTP.
    bool handle_response_for_test(long http_code, const std::string& body,
                                  const std::string& endpoint);

    // Pure HMAC-SHA256 (hex lowercase). Exposed for tests.
    static std::string hmac_sha256_hex(const std::string& secret, const std::string& msg);

    // WHY: optional Phase-5 hook. nullptr by default keeps existing tests intact.
    void set_reporter(db::PgReporter* r) { reporter_ = r; }

private:
    struct SignedResp {
        bool ok{false};
        long http_code{0};
        std::string body;
    };

    std::string url_encode(const std::string& s) const;
    std::string build_query(const std::vector<std::pair<std::string, std::string>>& params) const;

    SignedResp signed_request(CURL* handle, const std::string& method,
                              const std::string& path,
                              std::vector<std::pair<std::string, std::string>> params);

    // Returns true if response is "ok" (2xx); else logs structured failure and
    // triggers CB on 451.
    bool process_status(long http_code, const std::string& body,
                        const std::string& endpoint, int& binance_code_out);

    const infra::Config& cfg_;
    const Credentials& creds_;
    risk::CircuitBreaker* cb_;

    CURL* h_signed_write_{nullptr};
    CURL* h_signed_read_{nullptr};

    db::PgReporter* reporter_{nullptr};
};

}
