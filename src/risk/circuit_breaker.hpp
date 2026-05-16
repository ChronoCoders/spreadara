#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "infra/config.hpp"
#include "risk/position_tracker.hpp"
#include "risk/risk_manager.hpp"
#include "transport/spsc_ring_buffer.hpp"

namespace spreadara::db {
class PgReporter;
}

namespace spreadara::risk {

struct RiskEventMsg {
    uint16_t size{0};
    std::array<uint8_t, 512> bytes{};
};

using RiskEventRing = transport::SpscRingBuffer<RiskEventMsg, 1024>;

class CircuitBreaker {
public:
    CircuitBreaker(const infra::Config& cfg,
                   PositionTracker& pt,
                   RiskManager& rm,
                   RiskEventRing* out_ring);
    ~CircuitBreaker();

    CircuitBreaker(const CircuitBreaker&) = delete;
    CircuitBreaker& operator=(const CircuitBreaker&) = delete;

    void start();
    void stop();

    bool halted() const { return halted_.load(std::memory_order_acquire); }
    uint64_t halted_at_ns() const { return halted_at_ns_.load(std::memory_order_acquire); }

    void notify_ws_disconnect_exhausted();
    void notify_exception(const std::string& detail);

    // Test hook: run one monitor tick synchronously.
    void tick_for_test() { do_tick(std::chrono::steady_clock::now()); }

    // WHY: optional Phase-5 hook. nullptr keeps existing tests unchanged.
    void set_reporter(db::PgReporter* r) { reporter_ = r; }

private:
    struct PendingTrigger {
        std::string trig;
        std::string detail;
        uint64_t ts_ns{0};
    };

    void monitor_loop();
    void do_tick(std::chrono::steady_clock::time_point now);
    // WHY: ring push happens only here, called from the monitor thread, so the
    // SPSC ring sees exactly one producer.
    void publish_pending();
    // WHY: enqueue from arbitrary threads (notify_*, do_tick) for monitor-thread publish.
    void enqueue(const std::string& trig, const std::string& detail, uint64_t ts_ns);
    void set_halt_synchronously(const std::string& trig, const std::string& detail, uint64_t ts_ns);

    const infra::Config& cfg_;
    PositionTracker& pt_;
    RiskManager& rm_;
    RiskEventRing* out_ring_;

    std::atomic<bool> halted_{false};
    std::atomic<uint64_t> halted_at_ns_{0};

    std::atomic<bool> running_{false};
    std::thread monitor_thread_;

    db::PgReporter* reporter_{nullptr};

    // Internal state for drawdown / unhedged tracking — only touched by monitor thread.
    double peak_equity_{0.0};
    bool inv_started_{false};
    std::chrono::steady_clock::time_point inv_started_at_{};

    // WHY: one-per-state-transition latches. Set true on trigger; cleared when
    // the underlying condition clears. Prevents the 10 Hz event flood while a
    // condition persists. Touched only by monitor thread.
    bool dd_active_{false};
    bool unhedged_active_{false};
    bool rej_active_{false};

    // External-thread trigger queue, drained on each monitor tick.
    std::mutex pending_mu_;
    std::vector<PendingTrigger> pending_;
    static constexpr std::size_t kPendingCap = 64;
};

}
