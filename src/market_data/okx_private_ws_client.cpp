#include "market_data/okx_private_ws_client.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <cmath>
#include <cstring>
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

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <simdjson.h>
#include <spdlog/spdlog.h>

#include "execution/order_manager.hpp"
#include "infra/cpu_affinity.hpp"

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
    std::string path;
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

// WHY: small dedicated base64 helper. We deliberately do NOT reuse
// OkxRestClient::hmac_sha256_b64 here even though the algorithm matches —
// adding a public dependency from market_data/ to execution/ inverts the
// layering. The helper is six lines and identical in shape.
std::string base64_encode(const unsigned char* data, std::size_t len) {
    std::string out;
    out.resize(4 * ((len + 2) / 3));
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                            data, static_cast<int>(len));
    if (n < 0) return {};
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::string hmac_sha256_b64(const std::string& secret, const std::string& msg) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int out_len = 0;
    HMAC(EVP_sha256(),
         secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         out, &out_len);
    return base64_encode(out, out_len);
}

// WHY: login uses unix EPOCH SECONDS as a string, NOT the REST signer's ISO
// 8601 millisecond form. Getting this wrong returns code 60008 / 60014.
std::string unix_seconds_now_str() {
    using namespace std::chrono;
    const auto s = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    return std::to_string(s);
}

// WHY: api_key and passphrase are interpolated into a JSON message. A
// passphrase containing `"` or `\` would break the JSON and OKX returns a
// generic parse error. Conservative escape: backslash both, and replace
// any control char (which OKX keys never contain) with a space to keep the
// output ASCII-safe.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

bool parse_double_sv(std::string_view sv, double& out) {
    if (sv.empty()) return false;
    try { out = std::stod(std::string(sv)); return true; }
    catch (...) { return false; }
}

}  // namespace

namespace detail {

std::string build_subscribe_message(const std::string& symbol) {
    // WHY: single multi-arg subscribe — Beast disallows concurrent async_write
    // on the same stream (lesson copied from OkxWsClient::send_subscriptions).
    return std::string("{\"op\":\"subscribe\",\"args\":[")
         + "{\"channel\":\"orders\",\"instType\":\"SWAP\",\"instId\":\"" + symbol + "\"},"
         + "{\"channel\":\"positions\",\"instType\":\"SWAP\",\"instId\":\"" + symbol + "\"}"
         + "]}";
}

// WHY: extracts the FIRST fillable entry from a batched orders-channel
// message. Production code uses handle_orders directly (iterates ALL
// entries); this helper exists for unit-test consumption. The two paths
// share intent but not code — if OKX changes a field name, both need
// updating. Keeping them aligned is a small ongoing cost.
bool parse_orders_fill(const std::string& json,
                       const infra::Config& cfg,
                       risk::FillInput& out) {
    try {
        simdjson::ondemand::parser parser;
        // WHY: padded_string required — simdjson reads past the logical end
        // for SIMD reasons. std::string's internal buffer is NOT padded; using
        // it directly is UB.
        simdjson::padded_string padded(json);
        simdjson::ondemand::document doc = parser.iterate(padded);
        auto data = doc["data"];
        if (data.error() != simdjson::SUCCESS) return false;
        for (auto entry : data.get_array()) {
            std::string_view state_sv;
            if (entry["state"].get_string().get(state_sv) != simdjson::SUCCESS) continue;
            if (state_sv != "filled" && state_sv != "partially_filled") continue;

            std::string_view fill_sz_sv;
            if (entry["fillSz"].get_string().get(fill_sz_sv) != simdjson::SUCCESS) continue;
            double fill_sz = 0.0;
            if (!parse_double_sv(fill_sz_sv, fill_sz) || fill_sz <= 0.0) continue;

            std::string_view ord_id_sv, cl_ord_id_sv, inst_id_sv, side_sv;
            std::string_view fill_px_sv, fee_sv, fee_ccy_sv, u_time_sv;
            auto _e1 = entry["ordId"].get_string().get(ord_id_sv); (void)_e1;
            auto _e2 = entry["clOrdId"].get_string().get(cl_ord_id_sv); (void)_e2;
            auto _e3 = entry["instId"].get_string().get(inst_id_sv); (void)_e3;
            auto _e4 = entry["side"].get_string().get(side_sv); (void)_e4;
            auto _e5 = entry["fillPx"].get_string().get(fill_px_sv); (void)_e5;
            auto _e6 = entry["fee"].get_string().get(fee_sv); (void)_e6;
            auto _e7 = entry["feeCcy"].get_string().get(fee_ccy_sv); (void)_e7;
            auto _e8 = entry["uTime"].get_string().get(u_time_sv); (void)_e8;

            out.order_id = cl_ord_id_sv.empty()
                ? std::string(ord_id_sv)
                : std::string(cl_ord_id_sv);
            out.symbol.assign(inst_id_sv);
            out.side = (side_sv == "buy") ? +1 : -1;

            double px = 0.0, fee = 0.0;
            parse_double_sv(fill_px_sv, px);
            parse_double_sv(fee_sv, fee);
            out.price = px;
            // OKX fillSz for SWAP is in CONTRACTS; PositionTracker accounts in
            // BTC. Convert at the wire boundary.
            out.qty = fill_sz * cfg.exchange.contract_size;
            out.fee = std::fabs(fee);
            out.fee_asset.assign(fee_ccy_sv);
            try { out.timestamp_ns = std::stoull(std::string(u_time_sv)) * 1'000'000ULL; }
            catch (...) { out.timestamp_ns = 0; }
            return true;
        }
    } catch (...) {}
    return false;
}

}  // namespace detail

class OkxPrivateWsConnection : public std::enable_shared_from_this<OkxPrivateWsConnection> {
public:
    OkxPrivateWsConnection(asio::io_context& ioc, ssl::context& ssl_ctx,
                           std::string host, std::string port, std::string path,
                           std::string symbol,
                           const execution::Credentials& creds,
                           execution::OrderManager& om,
                           const infra::Config& cfg,
                           std::function<void()> on_fatal)
        : strand_(asio::make_strand(ioc)),
          resolver_(strand_),
          ws_(std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(strand_, ssl_ctx)),
          backoff_timer_(strand_),
          ping_timer_(strand_),
          ssl_ctx_ref_(&ssl_ctx),
          host_(std::move(host)),
          port_(std::move(port)),
          path_(std::move(path)),
          symbol_(std::move(symbol)),
          creds_(creds),
          om_(om),
          cfg_(cfg),
          on_fatal_(std::move(on_fatal)),
          backoff_ms_(cfg_.reconnect.initial_backoff_ms) {}

    void run() { do_resolve(); }

    void stop() {
        stopping_.store(true, std::memory_order_release);
        asio::post(strand_, [self = shared_from_this()] {
            boost::system::error_code ec;
            self->backoff_timer_.cancel(ec);
            self->ping_timer_.cancel(ec);
            if (self->ws_ && self->ws_->is_open()) {
                self->ws_->async_close(websocket::close_code::normal,
                    [self2 = self](boost::system::error_code) { (void)self2; });
            }
        });
    }

private:
    void do_resolve() {
        resolver_.async_resolve(host_, port_,
            beast::bind_front_handler(&OkxPrivateWsConnection::on_resolve, shared_from_this()));
    }

    void on_resolve(boost::system::error_code ec, tcp::resolver::results_type results) {
        if (ec) { schedule_reconnect("resolve", ec); return; }
        beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(*ws_).async_connect(results,
            beast::bind_front_handler(&OkxPrivateWsConnection::on_connect, shared_from_this()));
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
            beast::bind_front_handler(&OkxPrivateWsConnection::on_ssl_handshake, shared_from_this()));
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
            beast::bind_front_handler(&OkxPrivateWsConnection::on_ws_handshake, shared_from_this()));
    }

    void on_ws_handshake(boost::system::error_code ec) {
        if (ec) { schedule_reconnect("ws_handshake", ec); return; }
        attempt_ = 0;
        backoff_ms_ = cfg_.reconnect.initial_backoff_ms;
        logged_in_ = false;
        send_login();
        do_read();
    }

    // WHY: Beast disallows concurrent async_write on the same stream. The
    // private path has three potential writers (login, subscribe, pong) and
    // they can race in the first second after WS handshake when OKX sends a
    // ping mid-login. All writes go through this queue. Strand-confined.
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
                // WHY: never log msg contents — login frame contains the API key.
                if (wec) spdlog::warn("okx_private_write_fail err={}", wec.message());
                if (!self->write_queue_.empty()) self->write_queue_.pop_front();
                self->drain_write_queue();
            });
    }

    void send_login() {
        // WHY: unix epoch SECONDS as a string. Spec deviation from REST signer
        // (which uses ISO 8601 ms). Sign payload: ts + "GET" + "/users/self/verify".
        const std::string ts = unix_seconds_now_str();
        const std::string sig = hmac_sha256_b64(
            creds_.api_secret, ts + "GET" + "/users/self/verify");

        std::string msg =
            std::string("{\"op\":\"login\",\"args\":[{")
            + "\"apiKey\":\""     + json_escape(creds_.api_key)        + "\","
            + "\"passphrase\":\"" + json_escape(creds_.api_passphrase) + "\","
            + "\"timestamp\":\""  + ts                                 + "\","
            + "\"sign\":\""       + sig                                + "\""
            + "}]}";
        enqueue_write(std::make_shared<std::string>(std::move(msg)));
    }

    void send_subscriptions() {
        enqueue_write(std::make_shared<std::string>(
            detail::build_subscribe_message(symbol_)));
    }

    void send_pong() {
        enqueue_write(std::make_shared<std::string>("pong"));
    }

    // WHY: OKX private stream closes after 30s of client silence — and on a
    // quiet account (no fills, no orders) we'd otherwise never write. The
    // server does not initiate pings on the private channel the way it does
    // on public (where book updates keep traffic flowing), so we send our
    // own literal 4-byte "ping" frame every 25s. Re-armed after each tick.
    // Cancelled on disconnect/stop; re-armed by subscribe-ack on reconnect.
    void arm_ping_timer() {
        if (stopping_.load(std::memory_order_acquire)) return;
        ping_timer_.expires_after(std::chrono::seconds(25));
        ping_timer_.async_wait([self = shared_from_this()](boost::system::error_code wec) {
            if (wec) return;  // cancelled by reconnect / stop
            if (self->stopping_.load(std::memory_order_acquire)) return;
            self->enqueue_write(std::make_shared<std::string>("ping"));
            self->arm_ping_timer();
        });
    }

    void do_read() {
        ws_->async_read(buffer_,
            beast::bind_front_handler(&OkxPrivateWsConnection::on_read, shared_from_this()));
    }

    void on_read(boost::system::error_code ec, std::size_t) {
        if (ec) { schedule_reconnect("read", ec); return; }

        auto data = buffer_.data();
        const char* p = static_cast<const char*>(data.data());
        const std::size_t n = data.size();

        if (n == 4 && std::memcmp(p, "ping", 4) == 0) {
            send_pong();
            buffer_.consume(n);
            if (!stopping_.load(std::memory_order_acquire)) do_read();
            return;
        }

        try {
            simdjson::padded_string padded(p, n);
            simdjson::ondemand::document doc = parser_.iterate(padded);
            dispatch(doc);
        } catch (const std::exception& e) {
            spdlog::warn("okx_private_parse_error err={}", e.what());
        }

        buffer_.consume(n);
        if (!stopping_.load(std::memory_order_acquire)) do_read();
    }

    void dispatch(simdjson::ondemand::document& doc) {
        // Event envelope: login ack / subscribe ack / error.
        std::string_view event_sv;
        if (doc["event"].get_string().get(event_sv) == simdjson::SUCCESS) {
            if (event_sv == "login") {
                std::string_view code_sv;
                { auto _e = doc["code"].get_string().get(code_sv); (void)_e; }
                if (code_sv == "0") {
                    logged_in_ = true;
                    spdlog::info("okx_private_login_ok");
                    send_subscriptions();
                } else {
                    std::string_view msg_sv;
                    { auto _e = doc["msg"].get_string().get(msg_sv); (void)_e; }
                    spdlog::critical("okx_private_login_fail code={} msg={}",
                                     code_sv, msg_sv);
                    handle_login_failure();
                }
                return;
            }
            if (event_sv == "error") {
                std::string_view code_sv, msg_sv;
                { auto _e = doc["code"].get_string().get(code_sv); (void)_e; }
                { auto _e = doc["msg"].get_string().get(msg_sv); (void)_e; }
                spdlog::critical("okx_private_error code={} msg={}", code_sv, msg_sv);
                if (!logged_in_) handle_login_failure();
                return;
            }
            if (event_sv == "subscribe") {
                spdlog::info("okx_private_subscribe_ok");
                // WHY: re-arm on every subscribe ack — both orders and
                // positions channels each return one, so this fires twice
                // on initial connect. expires_after cancels any prior
                // pending wait, so the net effect is "25s from the most
                // recent subscribe ack."
                arm_ping_timer();
                return;
            }
        }

        // Channel update: route on arg.channel.
        std::string_view channel_sv;
        {
            auto arg = doc["arg"];
            if (arg.error() != simdjson::SUCCESS) return;
            if (arg["channel"].get_string().get(channel_sv) != simdjson::SUCCESS) return;
        }

        if (channel_sv == "orders") {
            handle_orders(doc);
        } else if (channel_sv == "positions") {
            handle_positions(doc);
        }
    }

    void handle_orders(simdjson::ondemand::document& doc) {
        auto data_arr = doc["data"];
        if (data_arr.error() != simdjson::SUCCESS) return;
        for (auto entry : data_arr.get_array()) {
            std::string_view state_sv;
            if (entry["state"].get_string().get(state_sv) != simdjson::SUCCESS) continue;
            if (state_sv != "filled" && state_sv != "partially_filled") continue;

            std::string_view fill_sz_sv;
            if (entry["fillSz"].get_string().get(fill_sz_sv) != simdjson::SUCCESS) continue;
            double fill_sz = 0.0;
            if (!parse_double_sv(fill_sz_sv, fill_sz) || fill_sz <= 0.0) continue;

            std::string_view ord_id_sv, cl_ord_id_sv, inst_id_sv, side_sv;
            std::string_view fill_px_sv, fee_sv, fee_ccy_sv, u_time_sv;
            { auto _e = entry["ordId"].get_string().get(ord_id_sv); (void)_e; }
            { auto _e = entry["clOrdId"].get_string().get(cl_ord_id_sv); (void)_e; }
            { auto _e = entry["instId"].get_string().get(inst_id_sv); (void)_e; }
            { auto _e = entry["side"].get_string().get(side_sv); (void)_e; }
            { auto _e = entry["fillPx"].get_string().get(fill_px_sv); (void)_e; }
            { auto _e = entry["fee"].get_string().get(fee_sv); (void)_e; }
            { auto _e = entry["feeCcy"].get_string().get(fee_ccy_sv); (void)_e; }
            { auto _e = entry["uTime"].get_string().get(u_time_sv); (void)_e; }

            risk::FillInput f;
            f.order_id = cl_ord_id_sv.empty()
                ? std::string(ord_id_sv)
                : std::string(cl_ord_id_sv);
            f.symbol.assign(inst_id_sv);
            f.side = (side_sv == "buy") ? +1 : -1;
            double px = 0.0, fee = 0.0;
            parse_double_sv(fill_px_sv, px);
            parse_double_sv(fee_sv, fee);
            f.price = px;
            f.qty = fill_sz * cfg_.exchange.contract_size;
            f.fee = std::fabs(fee);
            f.fee_asset.assign(fee_ccy_sv);
            try { f.timestamp_ns = std::stoull(std::string(u_time_sv)) * 1'000'000ULL; }
            catch (...) { f.timestamp_ns = 0; }

            spdlog::info("okx_private_fill cid={} side={} qty={:.4f} price={:.2f} fee={:.6f} {}",
                         f.order_id, (f.side > 0 ? "BUY" : "SELL"),
                         f.qty, f.price, f.fee, f.fee_asset);
            (void)om_.push_external_fill(f);
        }
    }

    void handle_positions(simdjson::ondemand::document& doc) {
        auto data_arr = doc["data"];
        if (data_arr.error() != simdjson::SUCCESS) return;
        for (auto entry : data_arr.get_array()) {
            std::string_view inst_sv, pos_sv, avg_sv, upl_sv;
            auto _p1 = entry["instId"].get_string().get(inst_sv); (void)_p1;
            auto _p2 = entry["pos"].get_string().get(pos_sv); (void)_p2;
            auto _p3 = entry["avgPx"].get_string().get(avg_sv); (void)_p3;
            auto _p4 = entry["upl"].get_string().get(upl_sv); (void)_p4;
            spdlog::info("okx_positions_update instId={} pos={} avgPx={} upl={}",
                         inst_sv, pos_sv, avg_sv, upl_sv);
        }
    }

    void handle_login_failure() {
        // WHY: single counter — schedule_reconnect's attempt_ + max_attempts
        // gating covers exhaustion. login failures count the same as any
        // reconnect cause; the "login" stage in the warn log carries the
        // distinction for operators.
        boost::system::error_code fake = boost::asio::error::operation_aborted;
        schedule_reconnect("login", fake);
    }

    void schedule_reconnect(const char* stage, boost::system::error_code ec) {
        if (stopping_.load(std::memory_order_acquire)) return;
        spdlog::warn("okx_private_disconnect stage={} err={}", stage, ec.message());

        // Cancel keepalive so a pending wait can't enqueue a ping onto a
        // dead socket. arm_ping_timer fires again on the next subscribe ack.
        boost::system::error_code cec;
        ping_timer_.cancel(cec);

        ++attempt_;
        if (attempt_ > cfg_.reconnect.max_attempts) {
            spdlog::critical("okx_private_reconnect_exhausted");
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
    asio::steady_timer ping_timer_;
    ssl::context* ssl_ctx_ref_{nullptr};
    beast::flat_buffer buffer_;
    simdjson::ondemand::parser parser_;
    std::string host_;
    std::string port_;
    std::string path_;
    std::string symbol_;
    const execution::Credentials& creds_;
    execution::OrderManager& om_;
    const infra::Config& cfg_;
    std::function<void()> on_fatal_;
    int attempt_{0};
    int backoff_ms_;
    bool logged_in_{false};
    std::atomic<bool> stopping_{false};
    // Strand-confined; only touched from on_read / on_ws_handshake / write
    // completion handlers, all of which run on strand_.
    std::deque<std::shared_ptr<std::string>> write_queue_;
    bool writing_{false};
};

OkxPrivateWsClient::OkxPrivateWsClient(asio::io_context& ioc,
                                       const infra::Config& cfg,
                                       const execution::Credentials& creds,
                                       execution::OrderManager& order_manager,
                                       FatalCallback on_fatal)
    : ioc_(ioc),
      ssl_ctx_(ssl::context::tls_client),
      cfg_(cfg),
      creds_(creds),
      order_manager_(order_manager),
      on_fatal_(std::move(on_fatal)) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
}

OkxPrivateWsClient::~OkxPrivateWsClient() { stop(); }

void OkxPrivateWsClient::start() {
    const std::string ws_url =
        cfg_.testnet.enabled && !cfg_.testnet.private_ws_base_url.empty()
            ? cfg_.testnet.private_ws_base_url
            : cfg_.exchange.private_ws_base_url;
    ParsedWsUrl parsed;
    if (!parse_ws_url(ws_url, parsed)) {
        spdlog::critical("okx_private_invalid_ws_base_url url={}", ws_url);
        if (on_fatal_) on_fatal_();
        return;
    }
    const std::string sym = cfg_.exchange.symbol.empty() ? cfg_.market_data.symbol
                                                         : cfg_.exchange.symbol;

    conn_ = std::make_shared<OkxPrivateWsConnection>(
        ioc_, ssl_ctx_, parsed.host, parsed.port, parsed.path,
        sym, creds_, order_manager_, cfg_, on_fatal_);
    conn_->run();
}

void OkxPrivateWsClient::stop() {
    if (stopped_.exchange(true)) return;
    if (conn_) conn_->stop();
}

}  // namespace spreadara::market_data::okx
