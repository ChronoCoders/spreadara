// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "backtest/historical_data_loader.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include "market_data/volatility.hpp"
#include "market_snapshot_generated.h"

namespace spreadara::backtest {

namespace {

bool parse_csv_row(const std::string& line, std::vector<std::string>& cols) {
    cols.clear();
    std::string cur;
    cur.reserve(32);
    for (char c : line) {
        if (c == ',') { cols.push_back(std::move(cur)); cur.clear(); }
        else if (c != '\r') { cur.push_back(c); }
    }
    cols.push_back(std::move(cur));
    return !cols.empty();
}

double parse_d(const std::string& s) {
    try { return std::stod(s); } catch (...) { return 0.0; }
}
uint64_t parse_u64(const std::string& s) {
    try { return static_cast<uint64_t>(std::stoull(s)); } catch (...) { return 0ULL; }
}

void write_record_le(std::ofstream& out, const uint8_t* data, uint32_t size) {
    uint8_t lp[4];
    lp[0] = static_cast<uint8_t>(size & 0xFF);
    lp[1] = static_cast<uint8_t>((size >> 8) & 0xFF);
    lp[2] = static_cast<uint8_t>((size >> 16) & 0xFF);
    lp[3] = static_cast<uint8_t>((size >> 24) & 0xFF);
    out.write(reinterpret_cast<const char*>(lp), 4);
    out.write(reinterpret_cast<const char*>(data), size);
}

}  // namespace

HistoricalDataLoader::HistoricalDataLoader(const infra::Config& cfg) : cfg_(cfg) {}

bool HistoricalDataLoader::load_csv(const std::string& csv_path, const std::string& bin_out) {
    std::ifstream in(csv_path);
    if (!in.is_open()) {
        spdlog::error("hist_csv_open_failed path={}", csv_path);
        return false;
    }
    std::ofstream out(bin_out, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        spdlog::error("hist_bin_open_failed path={}", bin_out);
        return false;
    }

    std::string line;
    if (!std::getline(in, line)) {
        spdlog::error("hist_csv_empty path={}", csv_path);
        return false;
    }
    // Header consumed; nothing to validate strictly — we positionally index.

    market_data::TickVolatility vol(static_cast<std::size_t>(
        cfg_.market_data.volatility_window > 0 ? cfg_.market_data.volatility_window : 100));

    std::vector<std::string> cols;
    cols.reserve(8);
    std::size_t rows = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (!parse_csv_row(line, cols)) continue;
        if (cols.size() < 7) continue;

        const double bid = parse_d(cols[1]);
        const double bid_qty = parse_d(cols[2]);
        const double ask = parse_d(cols[3]);
        const double ask_qty = parse_d(cols[4]);
        const uint64_t tx_ms = parse_u64(cols[5]);
        const uint64_t ev_ms = parse_u64(cols[6]);

        const double mid = (bid > 0.0 && ask > 0.0) ? (bid + ask) * 0.5 : 0.0;
        const double spread_bps = (mid > 0.0) ? ((ask - bid) / mid) * 1e4 : 0.0;
        if (mid > 0.0) vol.push_mid(mid);

        flatbuffers::FlatBufferBuilder fbb(256);
        auto snap = schemas::CreateMarketSnapshot(
            fbb,
            /*timestamp_ns*/ tx_ms * 1'000'000ULL,
            /*exchange_ts_ms*/ ev_ms,
            /*best_bid*/ bid,
            /*best_ask*/ ask,
            /*mid_price*/ mid,
            /*spread_bps*/ spread_bps,
            /*bid_depth_5*/ bid_qty,
            /*ask_depth_5*/ ask_qty,
            /*last_trade_price*/ 0.0,
            /*last_trade_qty*/ 0.0,
            /*volatility*/ vol.stdev_log_returns());
        fbb.Finish(snap);
        write_record_le(out, fbb.GetBufferPointer(), fbb.GetSize());
        ++rows;
    }
    spdlog::info("hist_csv_loaded rows={} bin_out={}", rows, bin_out);
    return rows > 0;
}

bool HistoricalDataLoader::stream_archive(const std::string& bin_path, const OnRecord& on_record) {
    std::ifstream in(bin_path, std::ios::binary);
    if (!in.is_open()) {
        spdlog::error("hist_archive_open_failed path={}", bin_path);
        return false;
    }
    std::vector<uint8_t> buf;
    buf.reserve(1024);
    while (true) {
        uint8_t lp[4];
        in.read(reinterpret_cast<char*>(lp), 4);
        if (in.gcount() == 0) break;
        if (in.gcount() != 4) return false;
        const uint32_t sz = static_cast<uint32_t>(lp[0]) |
                            (static_cast<uint32_t>(lp[1]) << 8) |
                            (static_cast<uint32_t>(lp[2]) << 16) |
                            (static_cast<uint32_t>(lp[3]) << 24);
        if (sz == 0 || sz > (1u << 20)) return false;
        buf.resize(sz);
        in.read(reinterpret_cast<char*>(buf.data()), sz);
        if (static_cast<uint32_t>(in.gcount()) != sz) return false;
        on_record(buf.data(), sz);
    }
    return true;
}

}
