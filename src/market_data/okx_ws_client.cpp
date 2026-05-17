#include "market_data/okx_ws_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <simdjson.h>
#include <spdlog/spdlog.h>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace spreadara::market_data::okx {

namespace {

struct ParsedWsUrl {
    std::string host;
    std::string port;
    std::string path;  // includes leading '/' and any '?query'
};

bool parse_ws_url(const std::string& url, ParsedWsUrl& out) {
    constexpr std::string_view kScheme = "wss://";
    if (url.compare(0, kScheme.size(), kScheme) != 0) return false;
    const std::string rest = url.substr(kScheme.size());
    const auto slash = rest.find('/');
    const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
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

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool parse_double_sv(std::string_view sv, double& out) {
    if (sv.empty()) return false;
    try {
        out = std::stod(std::string(sv));
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_double_field(simdjson::ondemand::value v, double& out) {
    std::string_view sv;
    auto err = v.get_string().get(sv);
    if (err == simdjson::SUCCESS) return parse_double_sv(sv, out);
    double d;
    if (v.get_double().get(d) == simdjson::SUCCESS) { out = d; return true; }
    return false;
}

}  // namespace

class OkxWsConnection : public std::enable_shared_from_this<OkxWsConnection> {
public:
    OkxWsConnection(asio::io_context& ioc, ssl::context& ssl_ctx,
                    std::string host, std::string port, std::string path,
                    std::string symbol,
                    EventRing& ring,
                    const infra::Config& cfg, std::function<void()> on_fatal)
        : strand_(asio::make_strand(ioc)),
          resolver_(strand_),
          ws_(std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(strand_, ssl_ctx)),
          backoff_timer_(strand_),
          ssl_ctx_ref_(&ssl_ctx),
          host_(std::move(host)),
          port_(std::move(port)),
          path_(std::move(path)),
          symbol_(std::move(symbol)),
          ring_(ring),
          cfg_(cfg),
          on_fatal_(std::move(on_fatal)),
          backoff_ms_(cfg_.reconnect.initial_backoff_ms) {
        parser_.threaded = false;
    }

    void run() { do_resolve(); }

    void stop() {
        stopping_.store(true, std::memory_order_release);
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
            beast::bind_front_handler(&OkxWsConnection::on_resolve, shared_from_this()));
    }

    void on_resolve(boost::system::error_code ec, tcp::resolver::results_type results) {
        if (ec) { schedule_reconnect("resolve", ec); return; }
        beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(*ws_).async_connect(results,
            beast::bind_front_handler(&OkxWsConnection::on_connect, shared_from_this()));
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
            beast::bind_front_handler(&OkxWsConnection::on_ssl_handshake, shared_from_this()));
    }

    void on_ssl_handshake(boost::system::error_code ec) {
        if (ec) { schedule_reconnect("ssl_handshake", ec); return; }
        beast::get_lowest_layer(*ws_).expires_never();
        ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(beast::http::field::user_agent, "spreadara/0.1");
            }));
        ws_->async_handshake(host_, path_,
            beast::bind_front_handler(&OkxWsConnection::on_ws_handshake, shared_from_this()));
    }

    void on_ws_handshake(boost::system::error_code ec) {
        if (ec) { schedule_reconnect("ws_handshake", ec); return; }
        attempt_ = 0;
        backoff_ms_ = cfg_.reconnect.initial_backoff_ms;
        send_subscriptions();
        do_read();
    }

    // WHY: Beast disallows concurrent async_write on the same stream. All
    // outbound frames go through this queue so a ping landing during the
    // subscribe-write window can't overlap. Drained on the strand by a
    // chained callback. send_subscriptions and send_pong only enqueue.
    void enqueue_write(std::shared_ptr<std::string> msg) {
        write_queue_.push_back(std::move(msg));
        if (!writing_) drain_write_queue();
    }

    void drain_write_queue() {
        if (write_queue_.empty()) { writing_ = false; return; }
        writing_ = true;
        auto msg = write_queue_.front();
        ws_->async_write(asio::buffer(*msg),
            [self = shared_from_this(), msg](boost::system::error_code wec, std::size_t) {
                if (wec) spdlog::warn("okx_write_fail err={}", wec.message());
                if (!self->write_queue_.empty()) self->write_queue_.pop_front();
                self->drain_write_queue();
            });
    }

    void send_subscriptions() {
        // Multi-arg subscribe still preferred (one round-trip), but the queue
        // also covers correctness if a future caller splits this.
        std::string msg =
            std::string("{\"op\":\"subscribe\",\"args\":[")
            + "{\"channel\":\"tickers\",\"instId\":\"" + symbol_ + "\"},"
            + "{\"channel\":\"books5\",\"instId\":\"" + symbol_ + "\"},"
            + "{\"channel\":\"trades\",\"instId\":\"" + symbol_ + "\"}"
            + "]}";
        enqueue_write(std::make_shared<std::string>(std::move(msg)));
    }

    void send_pong() {
        // OKX /ws/v5/public expects the literal 4-byte text frame "pong".
        enqueue_write(std::make_shared<std::string>("pong"));
    }

    void do_read() {
        ws_->async_read(buffer_,
            beast::bind_front_handler(&OkxWsConnection::on_read, shared_from_this()));
    }

    void on_read(boost::system::error_code ec, std::size_t) {
        if (ec) { schedule_reconnect("read", ec); return; }

        const uint64_t ts_ns = now_ns();
        auto data = buffer_.data();
        const char* p = static_cast<const char*>(data.data());
        const std::size_t n = data.size();

        // WHY: OKX heartbeat is a literal 4-byte text frame "ping" (NOT a
        // JSON envelope and NOT a WS control frame). The server responds to a
        // literal "pong" reply. Anything else lets the 30 s idle timeout
        // close the connection.
        if (n == 4 && std::memcmp(p, "ping", 4) == 0) {
            send_pong();
            buffer_.consume(n);
            if (!stopping_.load(std::memory_order_acquire)) do_read();
            return;
        }

        try {
            simdjson::padded_string padded(p, n);
            simdjson::ondemand::document doc = parser_.iterate(padded);
            dispatch(doc, ts_ns);
        } catch (const std::exception& e) {
            spdlog::warn("okx_parse_error err={}", e.what());
        }

        buffer_.consume(n);
        if (!stopping_.load(std::memory_order_acquire)) do_read();
    }

    void dispatch(simdjson::ondemand::document& doc, uint64_t ts_ns) {
        // Channel comes from arg.channel; data is an array (snapshot or update).
        std::string_view channel_sv;
        {
            auto arg = doc["arg"];
            if (arg.error() != simdjson::SUCCESS) return;  // ack/subscribe response
            if (arg["channel"].get_string().get(channel_sv) != simdjson::SUCCESS) return;
        }
        auto data_arr = doc["data"];
        if (data_arr.error() != simdjson::SUCCESS) return;

        if (channel_sv == "tickers") {
            for (auto entry : data_arr.get_array()) {
                MarketEvent ev;
                ev.ingress_ts_ns = ts_ns;
                ev.type = EventType::BookTicker;
                std::string_view ts_sv;
                if (entry["ts"].get_string().get(ts_sv) == simdjson::SUCCESS) {
                    try { ev.book_ticker.exchange_ts_ms = std::stoull(std::string(ts_sv)); }
                    catch (...) {}
                }
                double bid{}, bidsz{}, ask{}, asksz{};
                auto vb = entry["bidPx"];  if (vb.error() == simdjson::SUCCESS) parse_double_field(vb.value(), bid);
                auto vB = entry["bidSz"];  if (vB.error() == simdjson::SUCCESS) parse_double_field(vB.value(), bidsz);
                auto va = entry["askPx"];  if (va.error() == simdjson::SUCCESS) parse_double_field(va.value(), ask);
                auto vA = entry["askSz"];  if (vA.error() == simdjson::SUCCESS) parse_double_field(vA.value(), asksz);
                ev.book_ticker.best_bid_price = bid;
                ev.book_ticker.best_bid_qty = bidsz;
                ev.book_ticker.best_ask_price = ask;
                ev.book_ticker.best_ask_qty = asksz;
                if (!ring_.push(ev)) {
                    spdlog::warn("okx_ring_full type=tickers");
                }
            }
        } else if (channel_sv == "books5") {
            for (auto entry : data_arr.get_array()) {
                MarketEvent ev;
                ev.ingress_ts_ns = ts_ns;
                ev.type = EventType::Depth;
                std::string_view ts_sv;
                if (entry["ts"].get_string().get(ts_sv) == simdjson::SUCCESS) {
                    try { ev.depth.exchange_ts_ms = std::stoull(std::string(ts_sv)); }
                    catch (...) {}
                }
                // OKX: seqId / prevSeqId are integers. Map onto the existing
                // DepthEvent fields so the OrderBook gap-detection path reuses.
                int64_t seq{}, pseq{};
                [[maybe_unused]] auto e1 = entry["seqId"].get_int64().get(seq);
                [[maybe_unused]] auto e2 = entry["prevSeqId"].get_int64().get(pseq);
                ev.depth.first_update_id = static_cast<uint64_t>(seq);
                ev.depth.final_update_id = static_cast<uint64_t>(seq);
                ev.depth.prev_final_update_id = static_cast<uint64_t>(pseq);
                // WHY: books5 is a snapshot channel. Every message is a full
                // top-5 book, not a delta. Tick processor must NOT run gap
                // detection or trigger REST resync — both fire spurious
                // false positives at 10 Hz since prevSeqId isn't part of
                // books5 in the first place.
                ev.depth.is_snapshot = true;

                std::size_t bi = 0;
                auto bids_a = entry["bids"];
                if (bids_a.error() == simdjson::SUCCESS) {
                    for (auto level : bids_a.get_array()) {
                        if (bi >= 20) break;
                        auto arr = level.get_array();
                        auto it = arr.begin();
                        double px{}, sz{};
                        parse_double_field((*it).value(), px); ++it;
                        parse_double_field((*it).value(), sz);
                        ev.depth.bids[bi++] = {px, sz};
                    }
                }
                ev.depth.bid_count = static_cast<uint8_t>(bi);

                std::size_t ai = 0;
                auto asks_a = entry["asks"];
                if (asks_a.error() == simdjson::SUCCESS) {
                    for (auto level : asks_a.get_array()) {
                        if (ai >= 20) break;
                        auto arr = level.get_array();
                        auto it = arr.begin();
                        double px{}, sz{};
                        parse_double_field((*it).value(), px); ++it;
                        parse_double_field((*it).value(), sz);
                        ev.depth.asks[ai++] = {px, sz};
                    }
                }
                ev.depth.ask_count = static_cast<uint8_t>(ai);
                if (!ring_.push(ev)) {
                    spdlog::warn("okx_ring_full type=books5");
                }
            }
        } else if (channel_sv == "trades") {
            for (auto entry : data_arr.get_array()) {
                MarketEvent ev;
                ev.ingress_ts_ns = ts_ns;
                ev.type = EventType::Trade;
                std::string_view ts_sv;
                if (entry["ts"].get_string().get(ts_sv) == simdjson::SUCCESS) {
                    try { ev.trade.exchange_ts_ms = std::stoull(std::string(ts_sv)); }
                    catch (...) {}
                }
                double px{}, sz{};
                auto vp = entry["px"]; if (vp.error() == simdjson::SUCCESS) parse_double_field(vp.value(), px);
                auto vq = entry["sz"]; if (vq.error() == simdjson::SUCCESS) parse_double_field(vq.value(), sz);
                ev.trade.price = px;
                ev.trade.qty = sz;
                if (!ring_.push(ev)) {
                    spdlog::warn("okx_ring_full type=trades");
                }
            }
        }
    }

    void schedule_reconnect(const char* stage, boost::system::error_code ec) {
        if (stopping_.load(std::memory_order_acquire)) return;
        spdlog::warn("okx_ws_disconnect stage={} err={}", stage, ec.message());

        ++attempt_;
        if (attempt_ > cfg_.reconnect.max_attempts) {
            spdlog::critical("okx_ws_reconnect_exhausted");
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

    asio::strand<asio::io_context::executor_type> strand_;
    tcp::resolver resolver_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    asio::steady_timer backoff_timer_;
    ssl::context* ssl_ctx_ref_{nullptr};
    beast::flat_buffer buffer_;
    simdjson::ondemand::parser parser_;
    std::string host_;
    std::string port_;
    std::string path_;
    std::string symbol_;
    EventRing& ring_;
    const infra::Config& cfg_;
    std::function<void()> on_fatal_;
    int attempt_{0};
    int backoff_ms_;
    std::atomic<bool> stopping_{false};
    // Strand-confined; only touched from on_read / on_ws_handshake / write
    // completion handlers, all of which run on strand_.
    std::deque<std::shared_ptr<std::string>> write_queue_;
    bool writing_{false};
};

OkxWsClient::OkxWsClient(asio::io_context& ioc, const infra::Config& cfg,
                         EventRing& ring, FatalCallback on_fatal)
    : ioc_(ioc),
      ssl_ctx_(ssl::context::tls_client),
      cfg_(cfg),
      ring_(ring),
      on_fatal_(std::move(on_fatal)) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

OkxWsClient::~OkxWsClient() { stop(); }

void OkxWsClient::start() {
    const std::string ws_url = cfg_.testnet.enabled && !cfg_.testnet.ws_base_url.empty()
                                   ? cfg_.testnet.ws_base_url
                                   : cfg_.market_data.ws_base_url;
    ParsedWsUrl parsed;
    if (!parse_ws_url(ws_url, parsed)) {
        spdlog::critical("okx_invalid_ws_base_url url={}", ws_url);
        if (on_fatal_) on_fatal_();
        return;
    }
    const std::string sym = cfg_.exchange.symbol.empty() ? cfg_.market_data.symbol
                                                         : cfg_.exchange.symbol;

    auto conn = std::make_shared<OkxWsConnection>(
        ioc_, ssl_ctx_, parsed.host, parsed.port, parsed.path,
        sym, ring_, cfg_, on_fatal_);
    connections_.push_back(conn);
    conn->run();
}

void OkxWsClient::stop() {
    if (stopped_.exchange(true)) return;
    for (auto& c : connections_) c->stop();
}

}
