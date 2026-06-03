#pragma once

// OKX private user-data WebSocket adapter.
//
// Subscribes to the `orders` and `positions` channels on
// /ws/v5/private (or the testnet equivalent). Each parsed "filled" /
// "partially_filled" order update is converted to a risk::FillInput and pushed
// through OrderManager::push_external_fill, which feeds the existing
// FillEventRing consumer thread — so PositionTracker accounting is identical
// to the locally-injected path.
//
// Mirrors OkxWsClient: shared_ptr connection on its own strand, Beast async
// pattern, identical exponential reconnect backoff. Differences:
//   - performs an HMAC-signed `login` op before subscribing
//   - the timestamp encoding is unix-seconds-as-string (NOT ISO 8601)
//   - on login failure, escalates via the same fatal callback the public
//     client uses

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include "execution/i_rest_client.hpp"  // Credentials
#include "infra/config.hpp"
#include "risk/position_tracker.hpp"     // FillInput (for the detail helper)

namespace spreadara::execution { class OrderManager; }

namespace spreadara::market_data::okx {

class OkxPrivateWsConnection;

class OkxPrivateWsClient {
public:
    using FatalCallback = std::function<void()>;

    OkxPrivateWsClient(boost::asio::io_context& ioc,
                       const infra::Config& cfg,
                       const execution::Credentials& creds,
                       execution::OrderManager& order_manager,
                       FatalCallback on_fatal);
    ~OkxPrivateWsClient();

    OkxPrivateWsClient(const OkxPrivateWsClient&) = delete;
    OkxPrivateWsClient& operator=(const OkxPrivateWsClient&) = delete;

    void start();
    void stop();

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context ssl_ctx_;
    const infra::Config& cfg_;
    const execution::Credentials& creds_;
    execution::OrderManager& order_manager_;
    FatalCallback on_fatal_;
    std::shared_ptr<OkxPrivateWsConnection> conn_;
    std::atomic<bool> stopped_{false};
};

// Detail helpers exposed for unit testing. parse_orders_fill walks
// data[0] (test fixture uses a single-entry array) and produces a FillInput;
// returns false on non-fill states or malformed input. build_subscribe_message
// returns the literal frame the connection sends post-login.
namespace detail {

bool parse_orders_fill(const std::string& json,
                       const infra::Config& cfg,
                       risk::FillInput& out);

std::string build_subscribe_message(const std::string& symbol);

}  // namespace detail

}  // namespace spreadara::market_data::okx
