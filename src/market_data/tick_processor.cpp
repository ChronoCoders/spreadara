// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "market_data/tick_processor.hpp"

#include <chrono>
#include <cstring>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include "infra/cpu_affinity.hpp"
#include "market_snapshot_generated.h"

namespace spreadara::market_data {

namespace {
uint64_t mono_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

TickProcessor::TickProcessor(const infra::Config& cfg, EventRing& ring,
                             strategy::SnapshotRing* snap_ring,
                             strategy::SnapshotRing* record_ring)
    : cfg_(cfg), ring_(ring), snap_ring_(snap_ring), record_ring_(record_ring),
      vol_(static_cast<std::size_t>(cfg.market_data.volatility_window)) {}

TickProcessor::~TickProcessor() {
    stop();
}

void TickProcessor::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread([this] {
        if (cfg_.runtime.processor_cpu_core >= 0) {
            infra::pin_current_thread_to_core(cfg_.runtime.processor_cpu_core);
        }
        run_loop();
    });
}

void TickProcessor::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void TickProcessor::run_loop() {
    MarketEvent ev;
    while (running_.load(std::memory_order_acquire)) {
        if (ring_.pop(ev)) {
            process_event(ev);
        } else {
            std::this_thread::yield();
        }
    }
    while (ring_.pop(ev)) process_event(ev);
}

void TickProcessor::process_event(const MarketEvent& ev) {
    switch (ev.type) {
        case EventType::BookTicker: {
            std::array<PriceLevel, OrderBook::kMaxLevels> bids{};
            std::array<PriceLevel, OrderBook::kMaxLevels> asks{};
            bids[0] = {ev.book_ticker.best_bid_price, ev.book_ticker.best_bid_qty};
            asks[0] = {ev.book_ticker.best_ask_price, ev.book_ticker.best_ask_qty};
            // WHY: bookTicker carries no update_id; only use it when depth book has no data yet.
            if (!book_.has_data()) {
                book_.apply_snapshot(bids, 1, asks, 1, 0);
                book_.mark_resync();
            }
            emit_snapshot(ev.book_ticker.exchange_ts_ms);
            break;
        }
        case EventType::Depth: {
            if (ev.depth.is_snapshot) {
                // WHY: snapshot channels (OKX books5, etc.) ship a complete
                // book in every message. Apply directly. No gap detection,
                // no REST resync — pu/last_u comparisons are meaningless
                // when there is no delta semantics.
                std::array<PriceLevel, OrderBook::kMaxLevels> b = ev.depth.bids;
                std::array<PriceLevel, OrderBook::kMaxLevels> a = ev.depth.asks;
                book_.apply_snapshot(b, ev.depth.bid_count, a, ev.depth.ask_count,
                                     ev.depth.final_update_id);
            } else if (!book_.has_data() || book_.needs_resync()) {
                DepthSnapshot snap;
                if (fetch_depth_snapshot(cfg_, snap)) {
                    book_.apply_snapshot(snap.bids, snap.bid_count,
                                         snap.asks, snap.ask_count, snap.last_update_id);
                } else {
                    // WHY: REST failed (see rest_snapshot_fail log above for detail; in
                    // geo-blocked regions this is HTTP 451). @depth20 sends a full top-20
                    // snapshot every 100ms, so the incoming event is itself a valid
                    // recovery point.
                    spdlog::info("resync_fallback path=ws stream=depth20@100ms");
                    std::array<PriceLevel, OrderBook::kMaxLevels> b = ev.depth.bids;
                    std::array<PriceLevel, OrderBook::kMaxLevels> a = ev.depth.asks;
                    book_.apply_snapshot(b, ev.depth.bid_count, a, ev.depth.ask_count,
                                         ev.depth.final_update_id);
                }
            } else {
                if (!book_.apply_partial_update(ev.depth)) {
                    spdlog::warn("orderbook_gap pu={} last_u={}",
                                 ev.depth.prev_final_update_id, book_.last_update_id());
                }
            }
            const double mid = book_.mid();
            if (mid > 0.0) vol_.push_mid(mid);
            emit_snapshot(ev.depth.exchange_ts_ms);
            break;
        }
        case EventType::Trade: {
            last_trade_price_ = ev.trade.price;
            last_trade_qty_ = ev.trade.qty;
            emit_snapshot(ev.trade.exchange_ts_ms);
            break;
        }
        case EventType::None:
            break;
    }
}

void TickProcessor::emit_snapshot(uint64_t exchange_ts_ms) {
    if (!book_.has_data()) return;
    flatbuffers::FlatBufferBuilder fbb(256);
    auto snap = schemas::CreateMarketSnapshot(
        fbb,
        mono_ns(),
        exchange_ts_ms,
        book_.best_bid(),
        book_.best_ask(),
        book_.mid(),
        book_.spread_bps(),
        book_.depth_sum(true, 5),
        book_.depth_sum(false, 5),
        last_trade_price_,
        last_trade_qty_,
        vol_.stdev_log_returns());
    fbb.Finish(snap);
    spdlog::info("snap mid={:.4f} spread_bps={:.4f} vol={:.6f} bytes={}",
                 book_.mid(), book_.spread_bps(), vol_.stdev_log_returns(), fbb.GetSize());

    if (snap_ring_) {
        strategy::SnapshotMsg msg;
        const auto sz = fbb.GetSize();
        if (sz <= msg.bytes.size()) {
            msg.size = static_cast<uint16_t>(sz);
            std::memcpy(msg.bytes.data(), fbb.GetBufferPointer(), sz);
            if (!snap_ring_->push(msg)) {
                spdlog::warn("snapshot_ring_full bytes={}", sz);
            }
            // WHY: when --record is enabled, fan out the same bytes to a
            // dedicated recorder ring. Distinct rings preserve the SPSC
            // contract for each consumer (strategy and recorder).
            if (record_ring_) {
                if (!record_ring_->push(msg)) {
                    spdlog::warn("record_ring_full bytes={}", sz);
                }
            }
        } else {
            spdlog::warn("snapshot_oversize bytes={}", sz);
        }
    }
}

}
