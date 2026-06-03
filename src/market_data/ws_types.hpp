// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <array>
#include <cstdint>

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

}
