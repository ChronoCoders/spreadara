#include "execution/rest_client.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <simdjson.h>
#include <spdlog/spdlog.h>

#include "risk/circuit_breaker.hpp"

namespace spreadara::execution {

bool credentials_present() {
    const char* k = std::getenv("SPREADARA_API_KEY");
    const char* s = std::getenv("SPREADARA_API_SECRET");
    return k != nullptr && s != nullptr && *k != '\0' && *s != '\0';
}

namespace {

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// WHY: Binance enforces PRICE_FILTER.tickSize and LOT_SIZE.stepSize; sending
// raw doubles via %.8f silently fails with -1111. Round here so callers can
// pass natural values and live orders match exchange filters.
double round_to_step(double v, double step) {
    if (step <= 0.0) return v;
    return std::round(v / step) * step;
}

}  // namespace

std::string RestClient::hmac_sha256_hex(const std::string& secret, const std::string& msg) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         out, &out_len);
    static const char hex[] = "0123456789abcdef";
    std::string r;
    r.resize(out_len * 2);
    for (unsigned i = 0; i < out_len; ++i) {
        r[2 * i] = hex[(out[i] >> 4) & 0xF];
        r[2 * i + 1] = hex[out[i] & 0xF];
    }
    return r;
}

RestClient::RestClient(const infra::Config& cfg, const Credentials& creds,
                       risk::CircuitBreaker* cb)
    : cfg_(cfg), creds_(creds), cb_(cb) {
    h_signed_write_ = curl_easy_init();
    h_signed_read_ = curl_easy_init();
}

RestClient::~RestClient() {
    if (h_signed_write_) curl_easy_cleanup(h_signed_write_);
    if (h_signed_read_) curl_easy_cleanup(h_signed_read_);
}

std::string RestClient::url_encode(const std::string& s) const {
    // Use libcurl's url-encoder so the signature query matches exactly what gets sent.
    char* enc = curl_easy_escape(h_signed_read_, s.data(), static_cast<int>(s.size()));
    std::string out = enc ? enc : "";
    if (enc) curl_free(enc);
    return out;
}

std::string RestClient::build_query(
    const std::vector<std::pair<std::string, std::string>>& params) const {
    std::string q;
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) q += '&';
        q += params[i].first;
        q += '=';
        q += url_encode(params[i].second);
    }
    return q;
}

RestClient::SignedResp RestClient::signed_request(
    CURL* handle, const std::string& method, const std::string& path,
    std::vector<std::pair<std::string, std::string>> params) {

    params.emplace_back("recvWindow", std::to_string(cfg_.execution.recv_window_ms));
    params.emplace_back("timestamp", std::to_string(now_ms()));

    const std::string qs_unsigned = build_query(params);
    const std::string sig = hmac_sha256_hex(creds_.api_secret, qs_unsigned);
    const std::string full_qs = qs_unsigned + "&signature=" + sig;

    std::string url = cfg_.execution.rest_base_url + path;
    std::string body_buf;

    curl_easy_reset(handle);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "spreadara/0.1");
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(cfg_.execution.http_timeout_ms));
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body_buf);

    struct curl_slist* hdrs = nullptr;
    const std::string h_apikey = "X-MBX-APIKEY: " + creds_.api_key;
    hdrs = curl_slist_append(hdrs, h_apikey.c_str());

    std::string full_url;
    if (method == "GET" || method == "DELETE") {
        full_url = url + "?" + full_qs;
        curl_easy_setopt(handle, CURLOPT_URL, full_url.c_str());
        if (method == "DELETE") {
            curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
        }
    } else if (method == "POST") {
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, full_qs.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(full_qs.size()));
    } else if (method == "PUT") {
        full_url = url + "?" + full_qs;
        curl_easy_setopt(handle, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
    }

    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, hdrs);

    SignedResp r;
    CURLcode rc = curl_easy_perform(handle);
    if (rc != CURLE_OK) {
        spdlog::warn("rest_request_fail stage=curl curl_err={} endpoint={}",
                     static_cast<int>(rc), path);
        curl_slist_free_all(hdrs);
        return r;
    }
    long http_code = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);

    r.http_code = http_code;
    r.body = std::move(body_buf);
    r.ok = (http_code >= 200 && http_code < 300);
    return r;
}

bool RestClient::process_status(long http_code, const std::string& body,
                                const std::string& endpoint,
                                int& binance_code_out) {
    binance_code_out = 0;
    if (http_code >= 200 && http_code < 300) return true;

    // WHY: 451 = geo-block. CB has no dedicated geoblock trigger; reuse
    // notify_exception. Proper proxy / regional routing is Phase 7.
    if (http_code == 451) {
        spdlog::critical("rest_geoblocked http=451 endpoint={}", endpoint);
        if (cb_) cb_->notify_exception("rest_geoblocked");
        return false;
    }

    int bcode = 0;
    std::string bmsg;
    try {
        simdjson::dom::parser p;
        auto doc = p.parse(body);
        if (!doc.error()) {
            int64_t c;
            if (!doc["code"].get(c)) bcode = static_cast<int>(c);
            std::string_view m;
            if (!doc["msg"].get(m)) bmsg.assign(m.data(), m.size());
        }
    } catch (...) {
        // ignore parse errors
    }
    binance_code_out = bcode;
    spdlog::warn("rest_request_fail stage=http http={} binance_code={} binance_msg=\"{}\" endpoint={}",
                 http_code, bcode, bmsg, endpoint);
    return false;
}

bool RestClient::handle_response_for_test(long http_code, const std::string& body,
                                          const std::string& endpoint) {
    int bc = 0;
    return process_status(http_code, body, endpoint, bc);
}

OrderAck RestClient::place_order(const std::string& side, double qty, double price,
                                 bool post_only,
                                 const std::string& client_order_id) {
    OrderAck a;
    a.client_order_id = client_order_id;

    const double rounded_qty = round_to_step(qty, cfg_.strategy.qty_step);
    const double rounded_px = round_to_step(price, cfg_.strategy.min_tick);
    char qty_buf[32];
    char px_buf[32];
    std::snprintf(qty_buf, sizeof(qty_buf), "%.8f", rounded_qty);
    std::snprintf(px_buf, sizeof(px_buf), "%.8f", rounded_px);

    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", cfg_.market_data.symbol},
        {"side", side},
        {"type", "LIMIT"},
        {"timeInForce", post_only ? "GTX" : "GTC"},
        {"quantity", qty_buf},
        {"price", px_buf},
        {"newClientOrderId", client_order_id},
    };

    auto r = signed_request(h_signed_write_, "POST", "/fapi/v1/order", std::move(params));
    a.http_code = static_cast<int>(r.http_code);
    if (!process_status(r.http_code, r.body, "/fapi/v1/order", a.binance_code)) return a;

    try {
        simdjson::dom::parser p;
        auto doc = p.parse(r.body);
        int64_t oid;
        if (!doc["orderId"].get(oid)) a.exchange_order_id = oid;
        std::string_view st;
        if (!doc["status"].get(st)) a.status.assign(st.data(), st.size());
        a.price = price;
        a.qty = qty;
        a.ok = true;
    } catch (...) {
        spdlog::warn("rest_request_fail stage=parse endpoint=/fapi/v1/order");
    }
    return a;
}

OrderAck RestClient::place_market_order(const std::string& side, double qty,
                                        const std::string& client_order_id) {
    OrderAck a;
    a.client_order_id = client_order_id;

    const double rounded_qty = round_to_step(qty, cfg_.strategy.qty_step);
    char qty_buf[32];
    std::snprintf(qty_buf, sizeof(qty_buf), "%.8f", rounded_qty);

    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", cfg_.market_data.symbol},
        {"side", side},
        {"type", "MARKET"},
        {"quantity", qty_buf},
        {"newClientOrderId", client_order_id},
    };

    auto r = signed_request(h_signed_write_, "POST", "/fapi/v1/order", std::move(params));
    a.http_code = static_cast<int>(r.http_code);
    if (!process_status(r.http_code, r.body, "/fapi/v1/order", a.binance_code)) return a;

    try {
        simdjson::dom::parser p;
        auto doc = p.parse(r.body);
        int64_t oid;
        if (!doc["orderId"].get(oid)) a.exchange_order_id = oid;
        std::string_view st;
        if (!doc["status"].get(st)) a.status.assign(st.data(), st.size());
        a.qty = rounded_qty;
        a.ok = true;
    } catch (...) {
        spdlog::warn("rest_request_fail stage=parse endpoint=/fapi/v1/order(MARKET)");
    }
    return a;
}

CancelAck RestClient::cancel_order(const std::string& client_order_id,
                                   int64_t exchange_order_id) {
    CancelAck a;
    a.client_order_id = client_order_id;

    // WHY: Binance rejects with -1106 if neither orderId nor origClientOrderId
    // is provided. Catch the empty-ID case locally so we don't burn a request.
    if (exchange_order_id == 0 && client_order_id.empty()) {
        spdlog::warn("cancel_order_missing_id");
        return a;
    }

    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", cfg_.market_data.symbol},
    };
    if (exchange_order_id != 0) {
        params.emplace_back("orderId", std::to_string(exchange_order_id));
    } else {
        params.emplace_back("origClientOrderId", client_order_id);
    }

    auto r = signed_request(h_signed_write_, "DELETE", "/fapi/v1/order", std::move(params));
    a.http_code = static_cast<int>(r.http_code);
    if (!process_status(r.http_code, r.body, "/fapi/v1/order", a.binance_code)) return a;

    try {
        simdjson::dom::parser p;
        auto doc = p.parse(r.body);
        int64_t oid;
        if (!doc["orderId"].get(oid)) a.exchange_order_id = oid;
        std::string_view st;
        if (!doc["status"].get(st)) a.status.assign(st.data(), st.size());
        a.ok = true;
    } catch (...) {
        spdlog::warn("rest_request_fail stage=parse endpoint=/fapi/v1/order(DEL)");
    }
    return a;
}

AmendAck RestClient::amend_order(int64_t exchange_order_id, double new_price, double new_qty,
                                 const std::string& side, bool post_only,
                                 const std::string& replacement_client_order_id) {
    AmendAck a;
    a.exchange_order_id = exchange_order_id;

    const double rounded_qty = round_to_step(new_qty, cfg_.strategy.qty_step);
    const double rounded_px = round_to_step(new_price, cfg_.strategy.min_tick);
    char qty_buf[32];
    char px_buf[32];
    std::snprintf(qty_buf, sizeof(qty_buf), "%.8f", rounded_qty);
    std::snprintf(px_buf, sizeof(px_buf), "%.8f", rounded_px);

    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", cfg_.market_data.symbol},
        {"orderId", std::to_string(exchange_order_id)},
        {"side", side},
        {"quantity", qty_buf},
        {"price", px_buf},
    };

    auto r = signed_request(h_signed_write_, "PUT", "/fapi/v1/order", std::move(params));
    a.http_code = static_cast<int>(r.http_code);
    if (process_status(r.http_code, r.body, "/fapi/v1/order(PUT)", a.binance_code)) {
        try {
            simdjson::dom::parser p;
            auto doc = p.parse(r.body);
            int64_t oid;
            if (!doc["orderId"].get(oid)) a.exchange_order_id = oid;
            a.price = new_price;
            a.qty = new_qty;
            a.ok = true;
            return a;
        } catch (...) {}
    }

    // WHY: amend ineligibility / non-2xx -> cancel + place. Eliminates client-side
    // bookkeeping of "which orders can be amended in place" since Binance's amend
    // rules are restrictive (price band, no qty increase under certain TIFs).
    CancelAck c = cancel_order("", exchange_order_id);
    if (!c.ok) return a;
    OrderAck o = place_order(side, new_qty, new_price, post_only,
                             replacement_client_order_id);
    if (o.ok) {
        a.ok = true;
        a.exchange_order_id = o.exchange_order_id;
        a.price = new_price;
        a.qty = new_qty;
        a.client_order_id = replacement_client_order_id;
    }
    return a;
}

PositionsSnapshot RestClient::query_positions() {
    PositionsSnapshot s;
    std::vector<std::pair<std::string, std::string>> params;
    auto r = signed_request(h_signed_read_, "GET", "/fapi/v2/positionRisk", std::move(params));
    s.http_code = static_cast<int>(r.http_code);
    int bc = 0;
    if (!process_status(r.http_code, r.body, "/fapi/v2/positionRisk", bc)) return s;

    try {
        simdjson::dom::parser p;
        auto doc = p.parse(r.body);
        for (auto v : doc.get_array()) {
            PositionEntry e;
            std::string_view sv;
            if (!v["symbol"].get(sv)) e.symbol.assign(sv.data(), sv.size());
            if (!v["positionAmt"].get(sv)) e.position_amt = std::strtod(std::string(sv).c_str(), nullptr);
            if (!v["entryPrice"].get(sv)) e.entry_price = std::strtod(std::string(sv).c_str(), nullptr);
            if (!v["unRealizedProfit"].get(sv)) e.unrealized_profit = std::strtod(std::string(sv).c_str(), nullptr);
            s.positions.push_back(std::move(e));
        }
        s.ok = true;
    } catch (...) {
        spdlog::warn("rest_request_fail stage=parse endpoint=/fapi/v2/positionRisk");
    }
    return s;
}

OpenOrdersSnapshot RestClient::query_open_orders() {
    OpenOrdersSnapshot s;
    std::vector<std::pair<std::string, std::string>> params = {
        {"symbol", cfg_.market_data.symbol},
    };
    auto r = signed_request(h_signed_read_, "GET", "/fapi/v1/openOrders", std::move(params));
    s.http_code = static_cast<int>(r.http_code);
    int bc = 0;
    if (!process_status(r.http_code, r.body, "/fapi/v1/openOrders", bc)) return s;

    try {
        simdjson::dom::parser p;
        auto doc = p.parse(r.body);
        for (auto v : doc.get_array()) {
            OpenOrderEntry e;
            int64_t i;
            if (!v["orderId"].get(i)) e.exchange_order_id = i;
            std::string_view sv;
            if (!v["clientOrderId"].get(sv)) e.client_order_id.assign(sv.data(), sv.size());
            if (!v["symbol"].get(sv)) e.symbol.assign(sv.data(), sv.size());
            if (!v["side"].get(sv)) e.side.assign(sv.data(), sv.size());
            if (!v["price"].get(sv)) e.price = std::strtod(std::string(sv).c_str(), nullptr);
            if (!v["origQty"].get(sv)) e.orig_qty = std::strtod(std::string(sv).c_str(), nullptr);
            if (!v["executedQty"].get(sv)) e.executed_qty = std::strtod(std::string(sv).c_str(), nullptr);
            if (!v["status"].get(sv)) e.status.assign(sv.data(), sv.size());
            s.orders.push_back(std::move(e));
        }
        s.ok = true;
    } catch (...) {
        spdlog::warn("rest_request_fail stage=parse endpoint=/fapi/v1/openOrders");
    }
    return s;
}

}
