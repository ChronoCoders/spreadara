#pragma once

// Phase 7: OKX WebSocket adapter. Mirrors the Binance client's shape — pushes
// into the same EventRing using the same BookTickerEvent/DepthEvent/TradeEvent
// PODs. Downstream pipeline (TickProcessor, OrderBook, strategy) is untouched.
//
// OKX-specific concerns:
//   - control frames: OKX sends {"event":"ping"} text every ~30s, expects
//     {"event":"pong"} text reply (NOT a WS control ping/pong frame).
//   - books5 levels arrive as 4-element string arrays [px, sz, liq, ord_cnt];
//     we extract px/sz only.
//   - depth sequence: prevSeqId / seqId are mapped onto
//     DepthEvent.prev_final_update_id / final_update_id so the existing
//     OrderBook gap-detection path applies unchanged.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include "infra/config.hpp"
#include "market_data/ws_types.hpp"  // EventRing, PriceLevel

namespace spreadara::market_data::okx {

class OkxWsConnection;

class OkxWsClient {
public:
    using FatalCallback = std::function<void()>;

    OkxWsClient(boost::asio::io_context& ioc,
                const infra::Config& cfg,
                EventRing& ring,
                FatalCallback on_fatal);
    ~OkxWsClient();

    OkxWsClient(const OkxWsClient&) = delete;
    OkxWsClient& operator=(const OkxWsClient&) = delete;

    void start();
    void stop();

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context ssl_ctx_;
    const infra::Config& cfg_;
    EventRing& ring_;
    FatalCallback on_fatal_;
    std::vector<std::shared_ptr<OkxWsConnection>> connections_;
    std::atomic<bool> stopped_{false};
};

}
