#pragma once

// Phase 7: OKX REST adapter. Implements the same IRestClient seam Binance
// implements so OrderManager is swap-mechanical.
//
// Unit conventions:
//   Internal trading code expresses qty in BTC. The OKX wire format expresses
//   qty in **contracts** (1 contract = cfg.exchange.contract_size BTC for the
//   linear BTC-USDT-SWAP instrument). This adapter converts at the boundary:
//     - place_order:   wire_sz   = round(qty_btc / contract_size)
//     - query_positions: pos_btc = pos_contracts * contract_size
//   Everything caller-visible stays in BTC. Document any deviation in the
//   per-method body.
//
// Signing: HMAC-SHA256 base64 (NOT hex). Prehash = ts + METHOD + path + body.
// See okx_rest_client.cpp for the canonical implementation.

#include <cstdint>
#include <string>

#include "execution/i_rest_client.hpp"  // IRestClient + ack POD types
#include "infra/config.hpp"

typedef void CURL;

namespace spreadara::risk { class CircuitBreaker; }
namespace spreadara::db { class PgReporter; }

namespace spreadara::execution::okx {

// NOT thread-safe — same single-thread constraint as the Binance RestClient.
class OkxRestClient : public IRestClient {
public:
    OkxRestClient(const infra::Config& cfg, const Credentials& creds,
                  risk::CircuitBreaker* cb);
    ~OkxRestClient() override;

    OkxRestClient(const OkxRestClient&) = delete;
    OkxRestClient& operator=(const OkxRestClient&) = delete;

    OrderAck place_order(const std::string& side, double qty_btc, double price,
                         bool post_only, const std::string& client_order_id) override;
    OrderAck place_market_order(const std::string& side, double qty_btc,
                                const std::string& client_order_id) override;
    CancelAck cancel_order(const std::string& client_order_id,
                           int64_t exchange_order_id) override;
    AmendAck amend_order(int64_t exchange_order_id, double new_price, double new_qty_btc,
                         const std::string& side, bool post_only,
                         const std::string& replacement_client_order_id) override;
    PositionsSnapshot query_positions() override;
    OpenOrdersSnapshot query_open_orders() override;

    void set_reporter(db::PgReporter* r) { reporter_ = r; }

    // HMAC-SHA256, base64-encoded (with '=' padding). Exposed for tests.
    static std::string hmac_sha256_b64(const std::string& secret, const std::string& msg);

    // Build the OKX prehash string per spec: ts + UPPER(method) + path + body.
    static std::string prehash(const std::string& ts, const std::string& method,
                               const std::string& path, const std::string& body);

private:
    struct SignedResp { bool ok{false}; long http_code{0}; std::string body; };

    SignedResp signed_request(CURL* handle, const std::string& method,
                              const std::string& path, const std::string& body);

    // Returns true on code=="0"; logs / triggers CB on geoblock (50114).
    bool process_status(long http_code, const std::string& body,
                        const std::string& endpoint, int& exchange_code_out);

    const infra::Config& cfg_;
    const Credentials& creds_;
    risk::CircuitBreaker* cb_;

    CURL* h_write_{nullptr};
    CURL* h_read_{nullptr};

    db::PgReporter* reporter_{nullptr};
};

}
