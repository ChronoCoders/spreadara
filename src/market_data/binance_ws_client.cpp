#include "market_data/binance_ws_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <curl/curl.h>
#include <simdjson.h>
#include <spdlog/spdlog.h>

#include "infra/rdtsc.hpp"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace spreadara::market_data {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string stream_path(StreamKind k, const std::string& prefix, const std::string& sym_lower) {
    switch (k) {
        case StreamKind::BookTicker: return prefix + "/" + sym_lower + "@bookTicker";
        case StreamKind::Depth:      return prefix + "/" + sym_lower + "@depth20@100ms";
        case StreamKind::Trade:      return prefix + "/" + sym_lower + "@trade";
    }
    return "";
}

struct ParsedWsUrl {
    std::string host;
    std::string port;
    std::string path_prefix;
};

bool parse_ws_url(const std::string& url, ParsedWsUrl& out) {
    constexpr std::string_view kScheme = "wss://";
    if (url.compare(0, kScheme.size(), kScheme) != 0) return false;
    const std::string rest = url.substr(kScheme.size());
    const auto slash = rest.find('/');
    const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path_prefix = slash == std::string::npos ? "" : rest.substr(slash);
    const auto colon = host_port.find(':');
    if (colon == std::string::npos) {
        out.host = host_port;
        out.port = "443";
    } else {
        out.host = host_port.substr(0, colon);
        out.port = host_port.substr(colon + 1);
    }
    return !out.host.empty();
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
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

}

class WsConnection : public std::enable_shared_from_this<WsConnection> {
public:
    WsConnection(asio::io_context& ioc, ssl::context& ssl_ctx,
                 std::string host, std::string port, std::string target,
                 StreamKind kind, EventRing& ring,
                 const infra::Config& cfg, std::function<void()> on_fatal)
        : strand_(asio::make_strand(ioc)),
          resolver_(strand_),
          ws_(std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(strand_, ssl_ctx)),
          backoff_timer_(strand_),
          host_(std::move(host)),
          port_(std::move(port)),
          target_(std::move(target)),
          kind_(kind),
          ring_(ring),
          cfg_(cfg),
          on_fatal_(std::move(on_fatal)),
          backoff_ms_(cfg_.reconnect.initial_backoff_ms) {
        parser_.threaded = false;
    }

    void run() {
        do_resolve();
    }

    void stop() {
        stopping_.store(true, std::memory_order_release);
        // WHY: async_close / timer.cancel must run on the strand. stop() may be
        // called from any thread (destructor, signal handler), so post the work.
        asio::post(strand_, [self = shared_from_this()] {
            boost::system::error_code ec;
            self->backoff_timer_.cancel(ec);
            if (self->ws_ && self->ws_->is_open()) {
                self->ws_->async_close(websocket::close_code::normal,
                    [self2 = self](boost::system::error_code) { (void)self2; });
            }
        });
    }

private:
    void do_resolve() {
        resolver_.async_resolve(host_, port_,
            beast::bind_front_handler(&WsConnection::on_resolve, shared_from_this()));
    }

    void on_resolve(boost::system::error_code ec, tcp::resolver::results_type results) {
        if (ec) { schedule_reconnect("resolve", ec); return; }
        beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(*ws_).async_connect(results,
            beast::bind_front_handler(&WsConnection::on_connect, shared_from_this()));
    }

    void on_connect(boost::system::error_code ec, tcp::resolver::results_type::endpoint_type) {
        if (ec) { schedule_reconnect("connect", ec); return; }
        if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
            boost::system::error_code sec{static_cast<int>(::ERR_get_error()),
                                          boost::asio::error::get_ssl_category()};
            schedule_reconnect("sni", sec);
            return;
        }
        ws_->next_layer().async_handshake(ssl::stream_base::client,
            beast::bind_front_handler(&WsConnection::on_ssl_handshake, shared_from_this()));
    }

    void on_ssl_handshake(boost::system::error_code ec) {
        if (ec) { schedule_reconnect("ssl_handshake", ec); return; }
        beast::get_lowest_layer(*ws_).expires_never();
        ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(beast::http::field::user_agent, "spreadara/0.1");
            }));
        ws_->async_handshake(host_, target_,
            beast::bind_front_handler(&WsConnection::on_ws_handshake, shared_from_this()));
    }

    void on_ws_handshake(boost::system::error_code ec) {
        if (ec) { schedule_reconnect("ws_handshake", ec); return; }
        attempt_ = 0;
        backoff_ms_ = cfg_.reconnect.initial_backoff_ms;
        do_read();
    }

    void do_read() {
        ws_->async_read(buffer_,
            beast::bind_front_handler(&WsConnection::on_read, shared_from_this()));
    }

    void on_read(boost::system::error_code ec, std::size_t) {
        if (ec) { schedule_reconnect("read", ec); return; }

        const uint64_t ts_ns = now_ns();
        auto data = buffer_.data();
        const char* p = static_cast<const char*>(data.data());
        const std::size_t n = data.size();

        json_buf_.assign(p, n);
        json_buf_.reserve(n + simdjson::SIMDJSON_PADDING);

        try {
            simdjson::ondemand::document doc = parser_.iterate(json_buf_.data(), n, json_buf_.capacity());
            dispatch(doc, ts_ns);
        } catch (const std::exception& e) {
            spdlog::warn("parse_error stream={} err={}", static_cast<int>(kind_), e.what());
        }

        buffer_.consume(n);
        if (!stopping_.load(std::memory_order_acquire)) {
            do_read();
        }
    }

    void dispatch(simdjson::ondemand::document& doc, uint64_t ts_ns) {
        MarketEvent ev;
        ev.ingress_ts_ns = ts_ns;

        if (kind_ == StreamKind::BookTicker) {
            ev.type = EventType::BookTicker;
            uint64_t E = 0;
            int64_t e_signed;
            if (doc["E"].get_int64().get(e_signed) == simdjson::SUCCESS) {
                E = static_cast<uint64_t>(e_signed);
            }
            ev.book_ticker.exchange_ts_ms = E;
            double b{}, B{}, a{}, A{};
            auto vb = doc["b"]; parse_double(vb.value(), b);
            auto vB = doc["B"]; parse_double(vB.value(), B);
            auto va = doc["a"]; parse_double(va.value(), a);
            auto vA = doc["A"]; parse_double(vA.value(), A);
            ev.book_ticker.best_bid_price = b;
            ev.book_ticker.best_bid_qty = B;
            ev.book_ticker.best_ask_price = a;
            ev.book_ticker.best_ask_qty = A;
        } else if (kind_ == StreamKind::Depth) {
            ev.type = EventType::Depth;
            uint64_t E = 0;
            int64_t e_signed;
            if (doc["E"].get_int64().get(e_signed) == simdjson::SUCCESS) {
                E = static_cast<uint64_t>(e_signed);
            }
            ev.depth.exchange_ts_ms = E;
            int64_t U{}, u{}, pu{};
            auto eU [[maybe_unused]] = doc["U"].get_int64().get(U);
            auto eu [[maybe_unused]] = doc["u"].get_int64().get(u);
            auto epu [[maybe_unused]] = doc["pu"].get_int64().get(pu);
            ev.depth.first_update_id = static_cast<uint64_t>(U);
            ev.depth.final_update_id = static_cast<uint64_t>(u);
            ev.depth.prev_final_update_id = static_cast<uint64_t>(pu);

            std::size_t bi = 0;
            for (auto level : doc["b"].get_array()) {
                if (bi >= 20) break;
                auto arr = level.get_array();
                auto it = arr.begin();
                double price{}, qty{};
                parse_double((*it).value(), price); ++it;
                parse_double((*it).value(), qty);
                ev.depth.bids[bi++] = {price, qty};
            }
            ev.depth.bid_count = static_cast<uint8_t>(bi);

            std::size_t ai = 0;
            for (auto level : doc["a"].get_array()) {
                if (ai >= 20) break;
                auto arr = level.get_array();
                auto it = arr.begin();
                double price{}, qty{};
                parse_double((*it).value(), price); ++it;
                parse_double((*it).value(), qty);
                ev.depth.asks[ai++] = {price, qty};
            }
            ev.depth.ask_count = static_cast<uint8_t>(ai);
        } else {
            ev.type = EventType::Trade;
            uint64_t E = 0;
            int64_t e_signed;
            if (doc["E"].get_int64().get(e_signed) == simdjson::SUCCESS) {
                E = static_cast<uint64_t>(e_signed);
            }
            ev.trade.exchange_ts_ms = E;
            double price{}, qty{};
            auto vp = doc["p"]; parse_double(vp.value(), price);
            auto vq = doc["q"]; parse_double(vq.value(), qty);
            ev.trade.price = price;
            ev.trade.qty = qty;
        }

        if (!ring_.push(ev)) {
            spdlog::warn("ring_full dropping_event type={}", static_cast<int>(ev.type));
        }
    }

    void schedule_reconnect(const char* stage, boost::system::error_code ec) {
        if (stopping_.load(std::memory_order_acquire)) return;

        spdlog::warn("ws_disconnect stage={} stream={} err={}", stage, static_cast<int>(kind_), ec.message());

        ++attempt_;
        if (attempt_ > cfg_.reconnect.max_attempts) {
            spdlog::critical("ws_reconnect_exhausted stream={}", static_cast<int>(kind_));
            if (on_fatal_) on_fatal_();
            return;
        }

        const int ms = std::min(backoff_ms_, cfg_.reconnect.max_backoff_ms);
        backoff_ms_ = std::min(backoff_ms_ * 2, cfg_.reconnect.max_backoff_ms);

        backoff_timer_.expires_after(std::chrono::milliseconds(ms));
        backoff_timer_.async_wait([self = shared_from_this()](boost::system::error_code wec) {
            if (wec) return;
            self->reset_and_reconnect();
        });
    }

    void reset_and_reconnect() {
        if (stopping_.load(std::memory_order_acquire)) return;
        ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            strand_, *ssl_ctx_ref_);
        do_resolve();
    }

public:
    void set_ssl_ctx(ssl::context& ctx) { ssl_ctx_ref_ = &ctx; }

private:
    asio::strand<asio::io_context::executor_type> strand_;
    tcp::resolver resolver_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    asio::steady_timer backoff_timer_;
    ssl::context* ssl_ctx_ref_{nullptr};
    beast::flat_buffer buffer_;
    simdjson::ondemand::parser parser_;
    std::string json_buf_;
    std::string host_;
    std::string port_;
    std::string target_;
    StreamKind kind_;
    EventRing& ring_;
    const infra::Config& cfg_;
    std::function<void()> on_fatal_;
    int attempt_{0};
    int backoff_ms_;
    std::atomic<bool> stopping_{false};
};

BinanceWsClient::BinanceWsClient(asio::io_context& ioc,
                                 const infra::Config& cfg,
                                 EventRing& ring,
                                 FatalCallback on_fatal)
    : ioc_(ioc),
      ssl_ctx_(ssl::context::tls_client),
      cfg_(cfg),
      ring_(ring),
      on_fatal_(std::move(on_fatal)) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

BinanceWsClient::~BinanceWsClient() {
    stop();
}

void BinanceWsClient::start() {
    ParsedWsUrl parsed;
    if (!parse_ws_url(cfg_.market_data.ws_base_url, parsed)) {
        spdlog::critical("invalid_ws_base_url url={}", cfg_.market_data.ws_base_url);
        if (on_fatal_) on_fatal_();
        return;
    }
    const std::string sym = lower(cfg_.market_data.symbol);

    const std::array<StreamKind, 3> kinds = {
        StreamKind::BookTicker, StreamKind::Depth, StreamKind::Trade,
    };

    for (StreamKind k : kinds) {
        auto conn = std::make_shared<WsConnection>(
            ioc_, ssl_ctx_, parsed.host, parsed.port,
            stream_path(k, parsed.path_prefix, sym),
            k, ring_, cfg_, on_fatal_);
        conn->set_ssl_ctx(ssl_ctx_);
        connections_.push_back(conn);
        conn->run();
    }
}

void BinanceWsClient::stop() {
    if (stopped_.exchange(true)) return;
    for (auto& c : connections_) c->stop();
}

// WHY: Binance REST endpoints are geo-blocked from many regions (HTTP 451). The
// pipeline tolerates this via the WS-only resync fallback in TickProcessor.
// Phase 7 production deploy requires routing this call through a proxy in an
// unrestricted region. See [[spreadara]] memory.
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
        spdlog::warn("rest_snapshot_fail stage=http http={} binance_code={} binance_msg=\"{}\" url=\"{}\"",
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
