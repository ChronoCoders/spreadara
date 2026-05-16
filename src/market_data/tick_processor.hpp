#pragma once

#include <atomic>
#include <thread>

#include "infra/config.hpp"
#include "market_data/binance_ws_client.hpp"
#include "market_data/orderbook.hpp"
#include "market_data/volatility.hpp"
#include "strategy/signal_aggregator.hpp"

namespace spreadara::market_data {

class TickProcessor {
public:
    TickProcessor(const infra::Config& cfg, EventRing& ring,
                  strategy::SnapshotRing* snap_ring = nullptr,
                  strategy::SnapshotRing* record_ring = nullptr);
    ~TickProcessor();

    TickProcessor(const TickProcessor&) = delete;
    TickProcessor& operator=(const TickProcessor&) = delete;

    void start();
    void stop();

    // Exposed for tests/bench.
    void process_event(const MarketEvent& ev);

    const OrderBook& book() const { return book_; }

private:
    void run_loop();
    void emit_snapshot(uint64_t exchange_ts_ms);

    const infra::Config& cfg_;
    EventRing& ring_;
    strategy::SnapshotRing* snap_ring_;
    strategy::SnapshotRing* record_ring_{nullptr};
    OrderBook book_;
    TickVolatility vol_;
    double last_trade_price_{0.0};
    double last_trade_qty_{0.0};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}
