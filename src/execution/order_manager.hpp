#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "execution/rest_client.hpp"
#include "infra/config.hpp"
#include "risk/position_tracker.hpp"
#include "strategy/signal_aggregator.hpp"  // QuoteRing
#include "transport/spsc_ring_buffer.hpp"

namespace spreadara::risk {
class CircuitBreaker;
class RiskManager;
}

namespace spreadara::execution {

enum class OrderState : uint8_t {
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
    OrderState state{OrderState::PENDING};
    uint64_t submit_ts_ns{0};
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
                 RestClient& rest,
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

    // Called by the fill-applier thread peer (Phase 6 user-data stream injects here).
    // Phase 4: public so tests / mocks can inject synthetic fills.
    bool inject_fill(const risk::FillInput& f);

    // Reconciliation entry point.
    void reconcile_now();

    // Test seam: run the reconciliation comparison against synthetic snapshots
    // instead of issuing live REST calls. Mirrors what reconcile_now() does
    // after the rest_ calls return.
    void for_testing_reconcile(const PositionsSnapshot& pos,
                               const OpenOrdersSnapshot& oo);

    // Test seam: drive a state transition manually. Returns true iff valid.
    bool for_testing_state_transition(int slot_idx, OrderState to);

    // Test seam: synchronously process one quote (bid, ask) without threads.
    void for_testing_on_quote(double bid, double ask, double qty);

    // Accessors for tests.
    const OrderSlot& slot(int idx) const { return slots_[idx]; }

private:
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
    bool decode_quote(const strategy::QuoteMsg& msg, double& bid, double& ask,
                      double& qty) const;
    std::string make_cid();

    const infra::Config& cfg_;
    RestClient& rest_;
    risk::PositionTracker& pt_;
    risk::RiskManager& rm_;
    risk::CircuitBreaker& cb_;
    strategy::QuoteRing* quote_ring_;

    OrderSlot slots_[2]{};

    FillEventRing fill_ring_{};

    std::atomic<bool> running_{false};
    std::atomic<bool> halt_handled_{false};
    std::thread quote_thread_;
    std::thread fill_thread_;
    std::thread halt_thread_;
    std::thread reconcile_thread_;

    std::atomic<uint64_t> cid_counter_{0};
    uint64_t start_ms_{0};

    // WHY: serializes slot bookkeeping AND rest_ calls. Quote, halt, and
    // reconcile threads all touch slots_ and rest_; libcurl handles are not
    // thread-safe and slot fields are non-atomic. One coarse mutex held across
    // a REST call (~50–100 ms wire latency) is acceptable here because the
    // contending paths (halt, reconcile) are infrequent. Phase 5 may split
    // this into rest_mu_ + slots_mu_ if contention becomes measurable.
    mutable std::mutex mu_;
};

}
