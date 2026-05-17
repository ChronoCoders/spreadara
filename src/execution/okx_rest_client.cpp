#include "execution/okx_rest_client.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <simdjson.h>
#include <spdlog/spdlog.h>

#include "db/pg_reporter.hpp"
#include "risk/circuit_breaker.hpp"

namespace spreadara::execution::okx {

namespace {

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// ISO 8601 UTC, millisecond precision, "Z" suffix. e.g. 2026-05-16T16:05:00.123Z
std::string iso8601_ms_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    std::time_t tt = static_cast<std::time_t>(ms / 1000);
    int frac = static_cast<int>(ms % 1000);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, frac);
    return buf;
}

std::string base64_encode(const unsigned char* data, std::size_t len) {
    // Worst case: 4 * ceil(len/3) + 1.
    std::string out;
    out.resize(4 * ((len + 2) / 3));
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            data, static_cast<int>(len));
    if (n < 0) return {};
    out.resize(static_cast<std::size_t>(n));
    return out;
}

double round_to_step(double v, double step) {
    if (step <= 0.0) return v;
    return std::round(v / step) * step;
}

// WHY: OKX wraps a 200 OK as {"code":"0","data":[{...,"sCode":"0"|"<err>","sMsg":"..."}]}.
// Top-level code="0" only means the REQUEST was accepted — per-order success is
// in data[0].sCode. Trusting the top-level code alone hides silent order
// rejections (insufficient margin, price band, rate limit, etc.).
struct OkxItemResult {
    bool ok{false};
    int s_code{0};
    std::string s_msg;
    int64_t exchange_order_id{0};
    std::string client_order_id;
};

OkxItemResult parse_data_first(const std::string& body) {
    OkxItemResult r;
    try {
        simdjson::dom::parser p;
        auto doc = p.parse(body);
        auto data = doc["data"];
        if (data.error()) return r;
        for (auto v : data.get_array()) {
            std::string_view sv;
            if (!v["ordId"].get(sv)) {
                try { r.exchange_order_id = std::stoll(std::string(sv)); } catch (...) {}
            }
            if (!v["clOrdId"].get(sv)) r.client_order_id.assign(sv.data(), sv.size());
            std::string_view sc;
            if (!v["sCode"].get(sc)) {
                try { r.s_code = std::stoi(std::string(sc)); } catch (...) {}
            }
            std::string_view sm;
            if (!v["sMsg"].get(sm)) r.s_msg.assign(sm.data(), sm.size());
            r.ok = (r.s_code == 0);
            break;
        }
    } catch (...) {}
    return r;
}

}  // namespace

std::string OkxRestClient::hmac_sha256_b64(const std::string& secret,
                                           const std::string& msg) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         out, &out_len);
    return base64_encode(out, out_len);
}

std::string OkxRestClient::prehash(const std::string& ts, const std::string& method,
                                   const std::string& path, const std::string& body) {
    // method must be UPPERCASE. path includes any '?query' for GET.
    std::string m = method;
    for (char& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return ts + m + path + body;
}

OkxRestClient::OkxRestClient(const infra::Config& cfg, const Credentials& creds,
                             risk::CircuitBreaker* cb)
    : cfg_(cfg), creds_(creds), cb_(cb) {
    h_write_ = curl_easy_init();
    h_read_ = curl_easy_init();
}

OkxRestClient::~OkxRestClient() {
    if (h_write_) curl_easy_cleanup(h_write_);
    if (h_read_) curl_easy_cleanup(h_read_);
}

OkxRestClient::SignedResp OkxRestClient::signed_request(
    CURL* handle, const std::string& method, const std::string& path,
    const std::string& body) {

    const std::string ts = iso8601_ms_now();
    const std::string ph = prehash(ts, method, path, body);
    const std::string sig = hmac_sha256_b64(creds_.api_secret, ph);

    const std::string url = cfg_.execution.rest_base_url + path;
    std::string body_buf;

    curl_easy_reset(handle);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "spreadara/0.1");
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                     static_cast<long>(cfg_.execution.http_timeout_ms));
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body_buf);
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());

    struct curl_slist* hdrs = nullptr;
    const std::string h_key  = "OK-ACCESS-KEY: " + creds_.api_key;
    const std::string h_sign = "OK-ACCESS-SIGN: " + sig;
    const std::string h_ts   = "OK-ACCESS-TIMESTAMP: " + ts;
    const std::string h_pass = "OK-ACCESS-PASSPHRASE: " + creds_.api_passphrase;
    hdrs = curl_slist_append(hdrs, h_key.c_str());
    hdrs = curl_slist_append(hdrs, h_sign.c_str());
    hdrs = curl_slist_append(hdrs, h_ts.c_str());
    hdrs = curl_slist_append(hdrs, h_pass.c_str());
    if (method == "POST") {
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    }
    if (cfg_.testnet.enabled) {
        // OKX testnet shares the prod URL; the simulated-trading flag is a header.
        hdrs = curl_slist_append(hdrs, "x-simulated-trading: 1");
    }

    if (method == "POST") {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else if (method == "GET") {
        // default
    } else {
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, hdrs);

    SignedResp r;
    CURLcode rc = curl_easy_perform(handle);
    if (rc != CURLE_OK) {
        spdlog::warn("okx_rest_fail stage=curl curl_err={} endpoint={}",
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

bool OkxRestClient::process_status(long http_code, const std::string& body,
                                   const std::string& endpoint,
                                   int& exchange_code_out) {
    exchange_code_out = 0;
    if (http_code < 200 || http_code >= 300) {
        if (http_code != 0) {
            spdlog::warn("okx_rest_fail stage=http http={} endpoint={}",
                         http_code, endpoint);
        }
        return false;
    }
    // OKX wraps every 200 OK in {"code":"0","msg":"","data":[...]}.
    int code_int = 0;
    std::string code_str;
    std::string msg;
    try {
        simdjson::dom::parser p;
        auto doc = p.parse(body);
        if (!doc.error()) {
            std::string_view sv;
            if (!doc["code"].get(sv)) { code_str.assign(sv.data(), sv.size()); }
            std::string_view m;
            if (!doc["msg"].get(m)) msg.assign(m.data(), m.size());
        }
    } catch (...) {}
    if (!code_str.empty()) {
        try { code_int = std::stoi(code_str); } catch (...) { code_int = 0; }
    }
    exchange_code_out = code_int;

    if (code_str.empty() || code_str == "0") return true;

    if (code_str == "50114") {
        spdlog::critical("rest_geoblocked stage=geoblock code=50114 endpoint={}", endpoint);
        if (cb_) cb_->notify_exception("rest_geoblocked");
        return false;
    }

    // WHY: when top-level code is non-zero, OKX often puts the per-item
    // reason in data[0].sCode/sMsg (especially for code="1" "All operations
    // failed" on order placement). Pull that out so the log shows the
    // actual cause, not just the generic envelope.
    const auto pr = parse_data_first(body);
    // WHY: prefer per-item sCode over the top-level code when both indicate
    // failure. The per-item code identifies the specific exchange-side error
    // (e.g. 51400 = "order does not exist") that the caller's idempotent
    // handler keys on. Without this, CancelAck.binance_code carries the
    // generic top-level "1" and order_manager's cancel-idempotent path
    // never fires, leaving slots stuck after silent fills.
    if (pr.s_code != 0) {
        exchange_code_out = pr.s_code;
    }
    spdlog::warn(
        "okx_rest_fail stage=app exchange_code={} msg=\"{}\" item_code={} item_msg=\"{}\" endpoint={}",
        code_int, msg, pr.s_code, pr.s_msg, endpoint);
    return false;
}

OrderAck OkxRestClient::place_order(const std::string& side, double qty_btc, double price,
                                    bool post_only,
                                    const std::string& client_order_id) {
    OrderAck a;
    a.client_order_id = client_order_id;

    const double rounded_px = round_to_step(price, cfg_.exchange.tick_size > 0.0
                                                       ? cfg_.exchange.tick_size
                                                       : cfg_.strategy.min_tick);
    // Wire size: integer contracts (1 contract = contract_size BTC).
    double sz_contracts = qty_btc;
    if (cfg_.exchange.contract_size > 0.0) {
        sz_contracts = std::round(qty_btc / cfg_.exchange.contract_size);
    }

    char px_buf[32];
    char sz_buf[32];
    std::snprintf(px_buf, sizeof(px_buf), "%.1f", rounded_px);
    std::snprintf(sz_buf, sizeof(sz_buf), "%.0f", sz_contracts);

    // OKX side is lowercase ("buy"/"sell").
    std::string okx_side = side;
    for (char& c : okx_side) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string body;
    body.reserve(256);
    body += "{\"instId\":\"";
    body += cfg_.exchange.symbol.empty() ? cfg_.market_data.symbol : cfg_.exchange.symbol;
    body += "\",\"tdMode\":\"cross\",\"side\":\"";
    body += okx_side;
    body += "\",\"ordType\":\"limit\",\"px\":\"";
    body += px_buf;
    body += "\",\"sz\":\"";
    body += sz_buf;
    body += "\",\"postOnly\":";
    body += post_only ? "true" : "false";
    body += ",\"clOrdId\":\"";
    body += client_order_id;
    body += "\"}";

    auto r = signed_request(h_write_, "POST", "/api/v5/trade/order", body);
    a.http_code = static_cast<int>(r.http_code);
    if (!process_status(r.http_code, r.body, "/api/v5/trade/order", a.binance_code)) return a;

    const auto pr = parse_data_first(r.body);
    if (!pr.ok) {
        a.binance_code = pr.s_code;
        spdlog::warn("okx_rest_fail stage=order_reject exchange_code={} msg=\"{}\" endpoint=/api/v5/trade/order",
                     pr.s_code, pr.s_msg);
        return a;
    }
    a.exchange_order_id = pr.exchange_order_id;
    if (!pr.client_order_id.empty()) a.client_order_id = pr.client_order_id;
    a.status = "NEW";
    a.price = price;
    a.qty = qty_btc;
    a.ok = true;
    return a;
}

OrderAck OkxRestClient::place_market_order(const std::string& side, double qty_btc,
                                           const std::string& client_order_id) {
    OrderAck a;
    a.client_order_id = client_order_id;

    double sz_contracts = qty_btc;
    if (cfg_.exchange.contract_size > 0.0) {
        sz_contracts = std::round(qty_btc / cfg_.exchange.contract_size);
    }
    char sz_buf[32];
    std::snprintf(sz_buf, sizeof(sz_buf), "%.0f", sz_contracts);

    std::string okx_side = side;
    for (char& c : okx_side) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string body;
    body.reserve(192);
    body += "{\"instId\":\"";
    body += cfg_.exchange.symbol.empty() ? cfg_.market_data.symbol : cfg_.exchange.symbol;
    body += "\",\"tdMode\":\"cross\",\"side\":\"";
    body += okx_side;
    body += "\",\"ordType\":\"market\",\"sz\":\"";
    body += sz_buf;
    body += "\",\"clOrdId\":\"";
    body += client_order_id;
    body += "\"}";

    auto r = signed_request(h_write_, "POST", "/api/v5/trade/order", body);
    a.http_code = static_cast<int>(r.http_code);
    if (!process_status(r.http_code, r.body, "/api/v5/trade/order", a.binance_code)) return a;

    const auto pr = parse_data_first(r.body);
    if (!pr.ok) {
        a.binance_code = pr.s_code;
        spdlog::warn("okx_rest_fail stage=order_reject exchange_code={} msg=\"{}\" endpoint=/api/v5/trade/order(MARKET)",
                     pr.s_code, pr.s_msg);
        return a;
    }
    a.exchange_order_id = pr.exchange_order_id;
    if (!pr.client_order_id.empty()) a.client_order_id = pr.client_order_id;
    a.qty = qty_btc;
    a.status = "NEW";
    a.ok = true;
    return a;
}

CancelAck OkxRestClient::cancel_order(const std::string& client_order_id,
                                      int64_t exchange_order_id) {
    CancelAck a;
    a.client_order_id = client_order_id;

    if (exchange_order_id == 0 && client_order_id.empty()) {
        spdlog::warn("okx_cancel_missing_id");
        return a;
    }

    std::string body;
    body.reserve(128);
    body += "{\"instId\":\"";
    body += cfg_.exchange.symbol.empty() ? cfg_.market_data.symbol : cfg_.exchange.symbol;
    body += "\",";
    if (exchange_order_id != 0) {
        body += "\"ordId\":\"";
        body += std::to_string(exchange_order_id);
        body += "\"";
    } else {
        body += "\"clOrdId\":\"";
        body += client_order_id;
        body += "\"";
    }
    body += "}";

    auto r = signed_request(h_write_, "POST", "/api/v5/trade/cancel-order", body);
    a.http_code = static_cast<int>(r.http_code);
    if (!process_status(r.http_code, r.body, "/api/v5/trade/cancel-order", a.binance_code)) return a;

    const auto pr = parse_data_first(r.body);
    if (!pr.ok) {
        a.binance_code = pr.s_code;
        spdlog::warn("okx_rest_fail stage=cancel_reject exchange_code={} msg=\"{}\" endpoint=/api/v5/trade/cancel-order",
                     pr.s_code, pr.s_msg);
        return a;
    }
    a.exchange_order_id = pr.exchange_order_id;
    a.status = "CANCELED";
    a.ok = true;
    return a;
}

AmendAck OkxRestClient::amend_order(int64_t exchange_order_id, double new_price, double new_qty_btc,
                                    const std::string& side, bool post_only,
                                    const std::string& replacement_client_order_id) {
    AmendAck a;
    a.exchange_order_id = exchange_order_id;

    const double rounded_px = round_to_step(new_price, cfg_.exchange.tick_size > 0.0
                                                           ? cfg_.exchange.tick_size
                                                           : cfg_.strategy.min_tick);
    double sz_contracts = new_qty_btc;
    if (cfg_.exchange.contract_size > 0.0) {
        sz_contracts = std::round(new_qty_btc / cfg_.exchange.contract_size);
    }
    char px_buf[32];
    char sz_buf[32];
    std::snprintf(px_buf, sizeof(px_buf), "%.1f", rounded_px);
    std::snprintf(sz_buf, sizeof(sz_buf), "%.0f", sz_contracts);

    std::string body;
    body.reserve(192);
    body += "{\"instId\":\"";
    body += cfg_.exchange.symbol.empty() ? cfg_.market_data.symbol : cfg_.exchange.symbol;
    body += "\",\"ordId\":\"";
    body += std::to_string(exchange_order_id);
    body += "\",\"newPx\":\"";
    body += px_buf;
    body += "\",\"newSz\":\"";
    body += sz_buf;
    body += "\"}";

    auto r = signed_request(h_write_, "POST", "/api/v5/trade/amend-order", body);
    a.http_code = static_cast<int>(r.http_code);
    if (process_status(r.http_code, r.body, "/api/v5/trade/amend-order", a.binance_code)) {
        const auto pr = parse_data_first(r.body);
        if (pr.ok) {
            a.ok = true;
            a.price = new_price;
            a.qty = new_qty_btc;
            if (pr.exchange_order_id != 0) a.exchange_order_id = pr.exchange_order_id;
            return a;
        }
        spdlog::warn("okx_rest_fail stage=amend_reject exchange_code={} msg=\"{}\" endpoint=/api/v5/trade/amend-order",
                     pr.s_code, pr.s_msg);
        a.binance_code = pr.s_code;
        // Fall through to cancel+place fallback below.
    }

    // WHY: same cancel+place fallback as the Binance adapter, and the same
    // cancelled_only half-state surface when the replacement place fails.
    CancelAck c = cancel_order("", exchange_order_id);
    if (!c.ok) return a;
    OrderAck o = place_order(side, new_qty_btc, new_price, post_only,
                             replacement_client_order_id);
    if (o.ok) {
        a.ok = true;
        a.exchange_order_id = o.exchange_order_id;
        a.price = new_price;
        a.qty = new_qty_btc;
        a.client_order_id = replacement_client_order_id;
        return a;
    }
    a.cancelled_only = true;
    spdlog::critical("okx_amend_half_state original_order_id={} replacement_client_order_id={}",
                     exchange_order_id, replacement_client_order_id);
    return a;
}

PositionsSnapshot OkxRestClient::query_positions() {
    PositionsSnapshot s;
    const std::string sym = cfg_.exchange.symbol.empty() ? cfg_.market_data.symbol
                                                         : cfg_.exchange.symbol;
    const std::string path = "/api/v5/account/positions?instId=" + sym;
    auto r = signed_request(h_read_, "GET", path, "");
    s.http_code = static_cast<int>(r.http_code);
    int bc = 0;
    if (!process_status(r.http_code, r.body, path, bc)) return s;

    try {
        simdjson::dom::parser p;
        auto doc = p.parse(r.body);
        auto data = doc["data"];
        if (!data.error()) {
            for (auto v : data.get_array()) {
                PositionEntry e;
                std::string_view sv;
                if (!v["instId"].get(sv)) e.symbol.assign(sv.data(), sv.size());
                double pos_contracts = 0.0;
                if (!v["pos"].get(sv)) {
                    try { pos_contracts = std::stod(std::string(sv)); } catch (...) {}
                }
                // Convert contracts back to BTC for downstream consumers.
                e.position_amt = cfg_.exchange.contract_size > 0.0
                                     ? pos_contracts * cfg_.exchange.contract_size
                                     : pos_contracts;
                if (!v["avgPx"].get(sv)) {
                    try { e.entry_price = std::stod(std::string(sv)); } catch (...) {}
                }
                if (!v["upl"].get(sv)) {
                    try { e.unrealized_profit = std::stod(std::string(sv)); } catch (...) {}
                }
                s.positions.push_back(std::move(e));
            }
        }
        s.ok = true;
    } catch (...) {
        spdlog::warn("okx_rest_fail stage=parse endpoint=/api/v5/account/positions");
    }
    return s;
}

OpenOrdersSnapshot OkxRestClient::query_open_orders() {
    OpenOrdersSnapshot s;
    const std::string sym = cfg_.exchange.symbol.empty() ? cfg_.market_data.symbol
                                                         : cfg_.exchange.symbol;
    const std::string path = "/api/v5/trade/orders-pending?instId=" + sym;
    auto r = signed_request(h_read_, "GET", path, "");
    s.http_code = static_cast<int>(r.http_code);
    int bc = 0;
    if (!process_status(r.http_code, r.body, path, bc)) return s;

    try {
        simdjson::dom::parser p;
        auto doc = p.parse(r.body);
        auto data = doc["data"];
        if (!data.error()) {
            for (auto v : data.get_array()) {
                OpenOrderEntry e;
                std::string_view sv;
                if (!v["ordId"].get(sv)) {
                    try { e.exchange_order_id = std::stoll(std::string(sv)); } catch (...) {}
                }
                if (!v["clOrdId"].get(sv)) e.client_order_id.assign(sv.data(), sv.size());
                if (!v["instId"].get(sv)) e.symbol.assign(sv.data(), sv.size());
                if (!v["side"].get(sv)) e.side.assign(sv.data(), sv.size());
                if (!v["px"].get(sv)) {
                    try { e.price = std::stod(std::string(sv)); } catch (...) {}
                }
                double sz_contracts = 0.0, fill_contracts = 0.0;
                if (!v["sz"].get(sv)) {
                    try { sz_contracts = std::stod(std::string(sv)); } catch (...) {}
                }
                if (!v["accFillSz"].get(sv)) {
                    try { fill_contracts = std::stod(std::string(sv)); } catch (...) {}
                }
                const double cs = cfg_.exchange.contract_size > 0.0
                                      ? cfg_.exchange.contract_size : 1.0;
                e.orig_qty = sz_contracts * cs;
                e.executed_qty = fill_contracts * cs;
                if (!v["state"].get(sv)) e.status.assign(sv.data(), sv.size());
                s.orders.push_back(std::move(e));
            }
        }
        s.ok = true;
    } catch (...) {
        spdlog::warn("okx_rest_fail stage=parse endpoint=/api/v5/trade/orders-pending");
    }
    return s;
}

}
