// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <string>
#include <thread>

#include "strategy/signal_aggregator.hpp"  // SnapshotRing

namespace spreadara::backtest {

// WHY: live capture of MarketSnapshot FBs to a length-prefixed archive on
// disk. Reads from its OWN SnapshotRing (the producer at TickProcessor pushes
// to two rings when --record is set) so the strategy SPSC consumer is untouched.
class Recorder {
public:
    Recorder(const std::string& out_path, strategy::SnapshotRing* ring);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void start();
    void stop();

    std::size_t records_written() const { return records_written_.load(std::memory_order_acquire); }

private:
    void run_loop();

    std::string out_path_;
    strategy::SnapshotRing* ring_;
    std::ofstream out_;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> records_written_{0};
    std::thread thread_;
};

}
