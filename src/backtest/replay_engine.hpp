#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "infra/config.hpp"
#include "strategy/signal_aggregator.hpp"  // SnapshotRing

namespace spreadara::backtest {

// Reads one or more length-prefixed FB archives in chronological order, paces
// playback by snapshot timestamp_ns (scaled by speed_multiplier; 0 = max speed),
// and pushes SnapshotMsg POD onto the strategy SnapshotRing.
// Pauseable + steppable for deterministic tests.
class ReplayEngine {
public:
    explicit ReplayEngine(const infra::Config& cfg);
    ~ReplayEngine();

    ReplayEngine(const ReplayEngine&) = delete;
    ReplayEngine& operator=(const ReplayEngine&) = delete;

    // Optional per-record hook fired BEFORE pushing to the ring. Used by the
    // backtest runner to update SimulatedRestClient's market state.
    using OnRecord = std::function<void(const uint8_t* fb_data, std::size_t fb_size,
                                        uint64_t timestamp_ns,
                                        double best_bid, double best_ask,
                                        double bid_qty, double ask_qty)>;
    void set_on_record(OnRecord cb) { on_record_ = std::move(cb); }

    // Async start. speed_multiplier: 1, 10, 100, ... or 0 for max-speed.
    void start(const std::vector<std::string>& archive_paths,
               strategy::SnapshotRing* out_ring,
               double speed_multiplier,
               std::atomic<bool>* pause_flag);

    void stop();
    bool finished() const { return finished_.load(std::memory_order_acquire); }

    // Synchronous: read+emit exactly one record. Returns true if a record was
    // consumed; false if no records remain. Caller must NOT also call start().
    bool step_one();

    // Synchronous: drain everything from the configured archive_paths at max
    // speed (no sleeps, no thread). Returns total records emitted.
    std::size_t run_sync(const std::vector<std::string>& archive_paths,
                         strategy::SnapshotRing* out_ring);

    std::size_t records_emitted() const { return records_emitted_; }

private:
    void run_loop(std::vector<std::string> archives,
                  strategy::SnapshotRing* out_ring,
                  double speed_multiplier,
                  std::atomic<bool>* pause_flag);

    void emit_record(strategy::SnapshotRing* out_ring,
                     const uint8_t* fb_data, std::size_t fb_size);

    const infra::Config& cfg_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    OnRecord on_record_;
    std::size_t records_emitted_{0};

    // Step-mode state (lazy).
    struct StepState;
    StepState* step_state_{nullptr};
};

}
