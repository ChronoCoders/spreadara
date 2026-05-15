#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "transport/spsc_ring_buffer.hpp"

namespace spreadara::strategy {

// WHY: 512-byte payload cap; observed Phase 1 MarketSnapshot FB sizes were 96/112/128 bytes
// in live runs, so 512 gives ~4x headroom while keeping the POD trivially copyable for SPSC.
struct SnapshotMsg {
    uint16_t size{0};
    std::array<uint8_t, 512> bytes{};
};

struct QuoteMsg {
    uint16_t size{0};
    std::array<uint8_t, 512> bytes{};
};

using SnapshotRing = transport::SpscRingBuffer<SnapshotMsg, 16384>;
using QuoteRing = transport::SpscRingBuffer<QuoteMsg, 16384>;

struct Signals {
    double mid{0.0};
    double sigma_sq{0.0};
    double depth_imbalance{0.0};
    double total_depth{0.0};
    double last_trade_price{0.0};
    double last_trade_qty{0.0};
    double realized_vol{0.0};
    double bid_depth_5{0.0};
    double ask_depth_5{0.0};
    bool valid{false};
};

class SignalAggregator {
public:
    SignalAggregator() = default;

    bool ingest(const SnapshotMsg& msg);

    const Signals& signals() const { return signals_; }

private:
    Signals signals_{};
};

}
