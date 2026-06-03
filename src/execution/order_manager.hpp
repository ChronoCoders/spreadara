// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "execution/i_rest_client.hpp"
#include "infra/config.hpp"
#include "risk/position_tracker.hpp"
#include "strategy/signal_aggregator.hpp"  // QuoteRing
#include "transport/spsc_ring_buffer.hpp"

namespace spreadara::risk {
class CircuitBreaker;
class RiskManager;
}

namespace spreadara::db {
class PgReporter;
}

namespace spreadara::execution {

class OrderManagerTestPeer;
class OrderManagerBacktestAccess;

enum class OrderState : uint8_t {
    // WHY: NEW is the default-constructed sentinel — slot exists but no order
    // attempt yet. NEW -> PENDING is the only valid first transition; this
    // routes the initial-state change through transition() so the audit log
    // captures every placement.
    NEW,
    PENDING,
    SUBMITTED,
    ACKNOWLEDGED,
    PARTIALLY_FILLED,
    FILLED,
    CANCELED,
    REJECTED,
};

const char* to_str(OrderState s);

// Returns true if the transition is permitted.
bool is_valid_transition(OrderState from, OrderState to);

struct OrderSlot {
    bool active{false};
    std::string client_order_id;
    int64_t exchange_order_id{0};
    int8_t side{0};  // +1 buy / -1 sell
    double price{0.0};
    double qty{0.0};
    double executed_qty{0.0};
    OrderState state{OrderState::NEW};
    uint64_t submit_ts_ns{0};
    // WHY: RDTSC capture at SUBMITTED so a future async-ACK path (FIX,
    // user-data stream) can still compute round-trip latency across
    // method boundaries. Today's synchronous place_order/ACK pair uses it
    // within place_new only.
    uint64_t submit_cycles{0};
};

// FillEventRing carries FlatBuffer-encoded FillEvent bytes from the
// OrderManager quote-thread (single producer) to the fill-applier thread
// (single consumer) that calls PositionTracker::apply_fill.
struct FillMsg {
    uint16_t size{0};
    std::array<uint8_t, 512> bytes{};
};
using FillEventRing = transport::SpscRingBuffer<FillMsg, 1024>;

class OrderManager {
public:
    OrderManager(const infra::Config& cfg,
                 IRestClient& rest,
                 risk::PositionTracker& pt,
                 risk::RiskManager& rm,
                 risk::CircuitBreaker& cb,
                 strategy::QuoteRing* quote_ring);
    ~OrderManager();

    OrderManager(const OrderManager&) = delete;
    OrderManager& operator=(const OrderManager&) = delete;

    void start();
    void stop();

    // Called when CB transitions to halted=true. Cancels outstanding orders and
    // flattens inventory via IOC market order if |inv| > flatten_threshold.
    void on_halt();

    // WHY: clean Ctrl-C path. Cancels active orders and (if |inv| >
    // flatten_threshold) flattens inventory. Safe to call before start() —
    // slots are inert.
    void shutdown_cancel_all();

    // Public so tests / mocks can inject synthetic fills.
    bool inject_fill(const risk::FillInput& f);

    // External producers (OkxPrivateWsClient) call this to push a
    // parsed FillInput onto the same ring the local quote-thread feeds. Goes
    // through the same FlatBuffer-encode + slot-bookkeeping path as
    // inject_fill so the consumer thread is unchanged.
    bool push_external_fill(const risk::FillInput& f) { return inject_fill(f); }

    // Reconciliation entry point.
    void reconcile_now();

    // WHY: optional reporter hook. nullptr by default keeps existing tests intact.
    void set_reporter(db::PgReporter* r) { reporter_ = r; }

    // Rolling-window ACK-latency percentiles in microseconds. Returns
    // 0.0 if the window is empty. Cheap enough to call at 1 Hz from snap_thread.
    void latency_percentiles(double& p50_us, double& p95_us, double& p99_us) const;
    double latency_p50_us() const;
    double latency_p95_us() const;
    double latency_p99_us() const;

private:
    friend class OrderManagerTestPeer;
    // WHY: the BacktestRunner drives quotes inline (no threads) and
    // needs the same private seam test peers use. Defined in
    // backtest_runner.cpp.
    friend class OrderManagerBacktestAccess;

    // WHY: kept private — only OrderManagerTestPeer (defined in the test TU)
    // can reach in. Production code cannot drive state machines from outside.
    void for_testing_reconcile(const PositionsSnapshot& pos,
                               const OpenOrdersSnapshot& oo);
    bool for_testing_state_transition(int slot_idx, OrderState to);
    void for_testing_on_quote(double bid, double ask, double qty);
    const OrderSlot& peer_slot(int idx) const { return slots_[idx]; }

    enum SlotIdx : int { BID = 0, ASK = 1 };

    void quote_loop();
    void fill_apply_loop();
    void halt_watcher_loop();
    void reconcile_loop();

    void on_quote(double bid, double ask, double qty);
    void maybe_requote_side(int slot_idx, double new_price, double qty);
    bool place_new(int slot_idx, double price, double qty);
    bool cancel_slot(int slot_idx);

    void transition(int slot_idx, OrderState to);
    void sync_open_orders();
    // Shared cancel+flatten helper for on_halt() and shutdown_cancel_all().
    // Caller MUST hold mu_. Returns counts via out params.
    void cancel_and_flatten_locked(int& cancelled_out, bool& flattened_out);
    bool decode_quote(const strategy::QuoteMsg& msg, double& bid, double& ask,
                      double& qty) const;
    std::string make_cid();

    const infra::Config& cfg_;
    IRestClient& rest_;
    risk::PositionTracker& pt_;
    risk::RiskManager& rm_;
    risk::CircuitBreaker& cb_;
    strategy::QuoteRing* quote_ring_;

    OrderSlot slots_[2]{};

    db::PgReporter* reporter_{nullptr};

    FillEventRing fill_ring_{};

    std::atomic<bool> running_{false};
    std::atomic<bool> halt_handled_{false};
    std::thread quote_thread_;
    std::thread fill_thread_;
    std::thread halt_thread_;
    std::thread reconcile_thread_;

    std::atomic<uint64_t> cid_counter_{0};
    uint64_t start_ms_{0};

    // ACK-latency telemetry (RDTSC cycles, rolling window). Mutex
    // contention is negligible — record fires once per ACK (~10/s), query
    // fires at 1 Hz from snap_thread.
    static constexpr std::size_t kLatencyWindow = 1000;
    std::array<uint64_t, kLatencyWindow> latency_cycles_{};
    std::size_t latency_idx_{0};
    std::size_t latency_count_{0};
    mutable std::mutex latency_mu_;

    void record_latency_cycles(uint64_t cycles);

    // WHY: serializes slot bookkeeping AND rest_ calls. Quote, halt, and
    // reconcile threads all touch slots_ and rest_; libcurl handles are not
    // thread-safe and slot fields are non-atomic. One coarse mutex held across
    // a REST call (~50–100 ms wire latency) is acceptable here because the
    // contending paths (halt, reconcile) are infrequent. This may later split
    // into rest_mu_ + slots_mu_ if contention becomes measurable.
    mutable std::mutex mu_;
};

}
