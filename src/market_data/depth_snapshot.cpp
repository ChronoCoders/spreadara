// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "market_data/ws_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#include <curl/curl.h>
#include <simdjson.h>
#include <spdlog/spdlog.h>

namespace spreadara::market_data {

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool parse_double(simdjson::ondemand::value v, double& out) {
    std::string_view sv;
    auto err = v.get_string().get(sv);
    if (err == simdjson::SUCCESS) {
        try {
            out = std::stod(std::string(sv));
            return true;
        } catch (...) {
            return false;
        }
    }
    double d;
    if (v.get_double().get(d) == simdjson::SUCCESS) {
        out = d;
        return true;
    }
    return false;
}

size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    auto* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

}  // namespace

// WHY: Binance-shape REST depth snapshot. The pipeline tolerates failure via
// the WS-only resync fallback in TickProcessor. OKX uses the books5 snapshot
// channel and does not exercise this path; retained for back-compat with the
// non-snapshot depth fallback.
bool fetch_depth_snapshot(const infra::Config& cfg, DepthSnapshot& out) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        spdlog::warn("rest_snapshot_fail stage=init reason=curl_easy_init_null");
        return false;
    }
    const std::string url = cfg.market_data.rest_depth_url +
                            "?symbol=" + upper(cfg.market_data.symbol) +
                            "&limit=" + std::to_string(cfg.market_data.depth_levels);
    std::string body;
    char err_buf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "spreadara/0.1");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err_buf);
    const CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        spdlog::warn("rest_snapshot_fail stage=transport http={} curl_rc={} curl_err=\"{}\" detail=\"{}\"",
                     http_code, static_cast<int>(rc), curl_easy_strerror(rc),
                     err_buf[0] ? err_buf : "");
        return false;
    }

    if (http_code < 200 || http_code >= 300) {
        int64_t bcode = 0;
        std::string bmsg;
        try {
            std::string err_body = body;
            err_body.reserve(err_body.size() + simdjson::SIMDJSON_PADDING);
            simdjson::ondemand::parser p;
            auto d = p.iterate(err_body.data(), err_body.size(), err_body.capacity());
            [[maybe_unused]] auto ec1 = d["code"].get_int64().get(bcode);
            std::string_view sv;
            // WHY: copy into a std::string before err_body / parser leave scope —
            // simdjson's string_view points into the parser's buffer.
            if (d["msg"].get_string().get(sv) == simdjson::SUCCESS) {
                bmsg.assign(sv);
            }
        } catch (...) {}
        spdlog::warn("rest_snapshot_fail stage=http http={} exchange_code={} exchange_msg=\"{}\" url=\"{}\"",
                     http_code, bcode, bmsg, url);
        return false;
    }

    try {
        body.reserve(body.size() + simdjson::SIMDJSON_PADDING);
        simdjson::ondemand::parser parser;
        simdjson::ondemand::document doc = parser.iterate(body.data(), body.size(), body.capacity());

        int64_t lu_signed{};
        if (doc["lastUpdateId"].get_int64().get(lu_signed) != simdjson::SUCCESS) {
            spdlog::warn("rest_snapshot_fail stage=parse http={} reason=missing_lastUpdateId", http_code);
            return false;
        }
        out.last_update_id = static_cast<uint64_t>(lu_signed);

        std::size_t bi = 0;
        for (auto level : doc["bids"].get_array()) {
            if (bi >= out.bids.size()) break;
            auto arr = level.get_array();
            auto it = arr.begin();
            double price{}, qty{};
            parse_double((*it).value(), price); ++it;
            parse_double((*it).value(), qty);
            out.bids[bi++] = {price, qty};
        }
        out.bid_count = bi;

        std::size_t ai = 0;
        for (auto level : doc["asks"].get_array()) {
            if (ai >= out.asks.size()) break;
            auto arr = level.get_array();
            auto it = arr.begin();
            double price{}, qty{};
            parse_double((*it).value(), price); ++it;
            parse_double((*it).value(), qty);
            out.asks[ai++] = {price, qty};
        }
        out.ask_count = ai;
        if (out.bid_count == 0 || out.ask_count == 0) {
            spdlog::warn("rest_snapshot_fail stage=parse http={} reason=empty_book bid_count={} ask_count={}",
                         http_code, out.bid_count, out.ask_count);
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("rest_snapshot_fail stage=parse http={} err=\"{}\"", http_code, e.what());
        return false;
    }
}

}
