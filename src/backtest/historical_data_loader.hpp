#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "infra/config.hpp"

namespace spreadara::backtest {

// Reads Binance bookTicker daily CSV → emits MarketSnapshot FlatBuffers into a
// binary archive (length-prefixed records: 4-byte LE size + bytes). Streaming
// reader inverts the operation.
class HistoricalDataLoader {
public:
    explicit HistoricalDataLoader(const infra::Config& cfg);

    // CSV columns (header row required):
    //   update_id, best_bid_price, best_bid_qty, best_ask_price,
    //   best_ask_qty, transaction_time, event_time
    // timestamp_ns = transaction_time(ms) * 1e6.
    bool load_csv(const std::string& csv_path, const std::string& bin_out);

    using OnRecord = std::function<void(const uint8_t* fb_data, std::size_t fb_size)>;
    bool stream_archive(const std::string& bin_path, const OnRecord& on_record);

private:
    const infra::Config& cfg_;
};

}
