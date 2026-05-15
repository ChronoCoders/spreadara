#pragma once

#include <array>
#include <cstdint>

namespace spreadara::market_data {

enum class EventType : uint8_t {
    None = 0,
    BookTicker,
    Depth,
    Trade,
};

struct PriceLevel {
    double price;
    double qty;
};

struct BookTickerEvent {
    uint64_t exchange_ts_ms;
    double best_bid_price;
    double best_bid_qty;
    double best_ask_price;
    double best_ask_qty;
};

struct DepthEvent {
    uint64_t exchange_ts_ms;
    uint64_t first_update_id;  // U
    uint64_t final_update_id;  // u
    uint64_t prev_final_update_id;  // pu
    uint8_t bid_count;
    uint8_t ask_count;
    std::array<PriceLevel, 20> bids;
    std::array<PriceLevel, 20> asks;
};

struct TradeEvent {
    uint64_t exchange_ts_ms;
    double price;
    double qty;
};

struct MarketEvent {
    EventType type{EventType::None};
    uint64_t ingress_ts_ns{0};
    BookTickerEvent book_ticker{};
    DepthEvent depth{};
    TradeEvent trade{};
};

}
