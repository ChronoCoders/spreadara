#include "backtest/recorder.hpp"

#include <chrono>
#include <thread>

#include <spdlog/spdlog.h>

namespace spreadara::backtest {

Recorder::Recorder(const std::string& out_path, strategy::SnapshotRing* ring)
    : out_path_(out_path), ring_(ring) {}

Recorder::~Recorder() { stop(); }

void Recorder::start() {
    if (running_.exchange(true)) return;
    out_.open(out_path_, std::ios::binary | std::ios::trunc);
    if (!out_.is_open()) {
        spdlog::error("recorder_open_failed path={}", out_path_);
        running_.store(false);
        return;
    }
    thread_ = std::thread(&Recorder::run_loop, this);
}

void Recorder::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (out_.is_open()) out_.close();
}

void Recorder::run_loop() {
    strategy::SnapshotMsg msg;
    while (running_.load(std::memory_order_acquire)) {
        if (ring_ && ring_->pop(msg)) {
            const uint32_t sz = msg.size;
            uint8_t lp[4] = {
                static_cast<uint8_t>(sz & 0xFF),
                static_cast<uint8_t>((sz >> 8) & 0xFF),
                static_cast<uint8_t>((sz >> 16) & 0xFF),
                static_cast<uint8_t>((sz >> 24) & 0xFF),
            };
            out_.write(reinterpret_cast<const char*>(lp), 4);
            out_.write(reinterpret_cast<const char*>(msg.bytes.data()), sz);
            records_written_.fetch_add(1, std::memory_order_relaxed);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // Drain.
    while (ring_ && ring_->pop(msg)) {
        const uint32_t sz = msg.size;
        uint8_t lp[4] = {
            static_cast<uint8_t>(sz & 0xFF),
            static_cast<uint8_t>((sz >> 8) & 0xFF),
            static_cast<uint8_t>((sz >> 16) & 0xFF),
            static_cast<uint8_t>((sz >> 24) & 0xFF),
        };
        out_.write(reinterpret_cast<const char*>(lp), 4);
        out_.write(reinterpret_cast<const char*>(msg.bytes.data()), sz);
        records_written_.fetch_add(1, std::memory_order_relaxed);
    }
    out_.flush();
}

}
