#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl/context.hpp>

#include "infra/config.hpp"
#include "market_data/market_event.hpp"
#include "transport/spsc_ring_buffer.hpp"

namespace spreadara::market_data {

using EventRing = transport::SpscRingBuffer<MarketEvent, 65536>;

struct DepthSnapshot {
    uint64_t last_update_id{0};
    std::array<PriceLevel, 20> bids{};
    std::array<PriceLevel, 20> asks{};
    std::size_t bid_count{0};
    std::size_t ask_count{0};
};

// Synchronous REST fetch + parse. Blocks the calling thread; intended for the
// rare resync path, not the hot loop.
bool fetch_depth_snapshot(const infra::Config& cfg, DepthSnapshot& out);

enum class StreamKind {
    BookTicker,
    Depth,
    Trade,
};

class WsConnection;

class BinanceWsClient {
public:
    using FatalCallback = std::function<void()>;

    BinanceWsClient(boost::asio::io_context& ioc,
                    const infra::Config& cfg,
                    EventRing& ring,
                    FatalCallback on_fatal);
    ~BinanceWsClient();

    BinanceWsClient(const BinanceWsClient&) = delete;
    BinanceWsClient& operator=(const BinanceWsClient&) = delete;

    void start();
    void stop();

private:
    boost::asio::io_context& ioc_;
    boost::asio::ssl::context ssl_ctx_;
    const infra::Config& cfg_;
    EventRing& ring_;
    FatalCallback on_fatal_;
    std::vector<std::shared_ptr<WsConnection>> connections_;
    std::atomic<bool> stopped_{false};
};

}
