// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "backtest/replay_engine.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#include <flatbuffers/flatbuffers.h>
#include <spdlog/spdlog.h>

#include "backtest/historical_data_loader.hpp"
#include "market_snapshot_generated.h"

namespace spreadara::backtest {

struct ReplayEngine::StepState {
    std::vector<std::string> archives;
    std::size_t archive_idx{0};
    std::ifstream cur;
    strategy::SnapshotRing* out_ring{nullptr};
};

ReplayEngine::ReplayEngine(const infra::Config& cfg) : cfg_(cfg) {}

ReplayEngine::~ReplayEngine() {
    stop();
    delete step_state_;
}

void ReplayEngine::emit_record(strategy::SnapshotRing* out_ring,
                               const uint8_t* fb_data, std::size_t fb_size) {
    flatbuffers::Verifier v(fb_data, fb_size);
    if (!schemas::VerifyMarketSnapshotBuffer(v)) return;
    const auto* snap = schemas::GetMarketSnapshot(fb_data);
    const uint64_t ts_ns = snap->timestamp_ns();
    if (on_record_) {
        on_record_(fb_data, fb_size, ts_ns,
                   snap->best_bid(), snap->best_ask(),
                   snap->bid_depth_5(), snap->ask_depth_5());
    }
    if (out_ring) {
        strategy::SnapshotMsg m;
        if (fb_size <= m.bytes.size()) {
            m.size = static_cast<uint16_t>(fb_size);
            std::memcpy(m.bytes.data(), fb_data, fb_size);
            // WHY: spin-with-yield if the ring is full so we don't drop replay
            // records — backtest correctness matters more than latency here.
            while (!out_ring->push(m)) {
                std::this_thread::yield();
                if (!running_.load(std::memory_order_acquire) && step_state_ == nullptr) return;
            }
        }
    }
    ++records_emitted_;
}

void ReplayEngine::run_loop(std::vector<std::string> archives,
                            strategy::SnapshotRing* out_ring,
                            double speed_multiplier,
                            std::atomic<bool>* pause_flag) {
    HistoricalDataLoader loader(cfg_);
    uint64_t prev_ts = 0;
    auto wall_start = std::chrono::steady_clock::now();
    uint64_t replay_start_ts = 0;
    bool started = false;
    for (auto& path : archives) {
        if (!running_.load(std::memory_order_acquire)) break;
        loader.stream_archive(path, [&](const uint8_t* data, std::size_t sz) {
            if (!running_.load(std::memory_order_acquire)) return;
            while (pause_flag && pause_flag->load(std::memory_order_acquire) &&
                   running_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            flatbuffers::Verifier v(data, sz);
            if (!schemas::VerifyMarketSnapshotBuffer(v)) return;
            const auto* snap = schemas::GetMarketSnapshot(data);
            const uint64_t ts = snap->timestamp_ns();
            if (speed_multiplier > 0.0 && prev_ts != 0 && ts > prev_ts) {
                if (!started) { started = true; replay_start_ts = prev_ts; wall_start = std::chrono::steady_clock::now(); }
                const uint64_t replay_dt = ts - replay_start_ts;
                const auto wall_target = wall_start +
                    std::chrono::nanoseconds(static_cast<int64_t>(
                        static_cast<double>(replay_dt) / speed_multiplier));
                const auto now = std::chrono::steady_clock::now();
                if (wall_target > now) std::this_thread::sleep_for(wall_target - now);
            }
            prev_ts = ts;
            emit_record(out_ring, data, sz);
        });
    }
    finished_.store(true, std::memory_order_release);
}

void ReplayEngine::start(const std::vector<std::string>& archive_paths,
                         strategy::SnapshotRing* out_ring,
                         double speed_multiplier,
                         std::atomic<bool>* pause_flag) {
    if (running_.exchange(true)) return;
    finished_.store(false, std::memory_order_release);
    thread_ = std::thread([this, archive_paths, out_ring, speed_multiplier, pause_flag]() mutable {
        run_loop(std::move(archive_paths), out_ring, speed_multiplier, pause_flag);
    });
}

void ReplayEngine::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

std::size_t ReplayEngine::run_sync(const std::vector<std::string>& archive_paths,
                                   strategy::SnapshotRing* out_ring) {
    HistoricalDataLoader loader(cfg_);
    running_.store(true, std::memory_order_release);
    for (auto& path : archive_paths) {
        loader.stream_archive(path, [&](const uint8_t* data, std::size_t sz) {
            emit_record(out_ring, data, sz);
        });
    }
    running_.store(false, std::memory_order_release);
    finished_.store(true, std::memory_order_release);
    return records_emitted_;
}

bool ReplayEngine::step_one() {
    // WHY: minimalist step mode for deterministic tests — drains a single
    // length-prefixed record from the current archive, advances on EOF.
    if (!step_state_) {
        spdlog::warn("step_one_no_archive — call start() in step mode is unsupported; use run_sync");
        return false;
    }
    auto& s = *step_state_;
    while (true) {
        if (!s.cur.is_open()) {
            if (s.archive_idx >= s.archives.size()) return false;
            s.cur.open(s.archives[s.archive_idx], std::ios::binary);
            if (!s.cur.is_open()) { ++s.archive_idx; continue; }
        }
        uint8_t lp[4];
        s.cur.read(reinterpret_cast<char*>(lp), 4);
        if (s.cur.gcount() != 4) {
            s.cur.close();
            ++s.archive_idx;
            continue;
        }
        const uint32_t sz = static_cast<uint32_t>(lp[0]) |
                            (static_cast<uint32_t>(lp[1]) << 8) |
                            (static_cast<uint32_t>(lp[2]) << 16) |
                            (static_cast<uint32_t>(lp[3]) << 24);
        std::vector<uint8_t> buf(sz);
        s.cur.read(reinterpret_cast<char*>(buf.data()), sz);
        if (static_cast<uint32_t>(s.cur.gcount()) != sz) return false;
        emit_record(s.out_ring, buf.data(), sz);
        return true;
    }
}

}
