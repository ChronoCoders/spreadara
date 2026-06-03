// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

// WHY: Spec says "separate process" but the user chose the pinned-thread
// interpretation for IPC simplicity. PgReporter runs as a dedicated consumer
// thread inside the spreadara binary, pinned to cfg.reporter.core if >= 0.
// All libpqxx calls are wrapped in try/catch and translated to bool — no
// libpqxx exception is ever allowed to escape into the trading hot path or
// the SPSC consumer loop.

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "infra/config.hpp"
#include "transport/spsc_ring_buffer.hpp"

namespace spreadara::db {

enum class DbEventKind : uint8_t {
    Trade,
    PositionSnapshot,
    SystemEvent,
    DailyPnl,
};

struct DbTrade {
    char order_id[40];
    int8_t side;
    double price;
    double qty;
    double fee;
    char fee_asset[8];
    uint64_t ts_ns;
    bool is_maker;   // postOnly fills are always maker
};

struct DbPositionSnapshot {
    uint64_t ts_ns;
    double inventory;
    double avg_entry;
    double realized;
    double unrealized;
    double fees;
    double mid;
    double best_bid;
    double best_ask;
    double spread_bps;
    double bid_qty;
    double ask_qty;
    double volatility;
    double gamma;
    double k;
    double T;
    double lat_p50_us;
    double lat_p95_us;
    double lat_p99_us;
    int32_t open_orders;
};

struct DbSystemEvent {
    uint64_t ts_ns;
    char severity[16];
    char source[32];
    char code[32];
    char msg[256];
};

struct DbDailyPnl {
    int32_t date;
    double realized;
    double unrealized;
    double fees;
    double total;
};

struct DbEvent {
    DbEventKind kind;
    union {
        DbTrade trade;
        DbPositionSnapshot snap;
        DbSystemEvent evt;
        DbDailyPnl daily;
    };
};

static_assert(std::is_trivially_copyable<DbEvent>::value, "DbEvent must be trivially copyable");

// WHY: SPSC capacity must be constexpr. Hard-coded
// template arg matches cfg.transport.db_ring_capacity default (4096).
using DbEventRing = transport::SpscRingBuffer<DbEvent, 4096>;

// Forward-declared pimpl — defined in pg_reporter.cpp.
struct PgConnImpl;

class PgReporter {
public:
    // WHY: dsn passed in (never read SPREADARA_PG_DSN here) so tests can
    // inject "" for dry mode or a fake DSN without touching the environment.
    PgReporter(const infra::Config& cfg, DbEventRing& ring, const std::string& dsn);
    ~PgReporter();

    PgReporter(const PgReporter&) = delete;
    PgReporter& operator=(const PgReporter&) = delete;

    void start();
    void stop();

    // Returns false on full ring — caller MUST drop the event and log; never block.
    bool push(const DbEvent& e);

    // Observable counters for tests.
    uint64_t flushed_count() const { return flushed_count_.load(std::memory_order_acquire); }
    uint64_t pending_count() const { return pending_count_.load(std::memory_order_acquire); }
    uint64_t dropped_count() const { return dropped_count_.load(std::memory_order_acquire); }

    // Test seam: synchronously create the schema (idempotent). Returns true on success
    // or in dry mode (dsn empty).
    bool create_schema_for_test();

    // Test seam: inject one bad event into the next flush (consumer will fail
    // the batch txn, then per-event retry must skip ONLY this one).
    void inject_poison_for_test() { poison_next_.store(true, std::memory_order_release); }

    // Test seam: snapshot of events flushed in dry mode, in order.
    // WHY: tests need to verify CONTENT, not just count, to catch bugs like
    // the rollover writing new-day totals stamped under old-day's date.
    std::vector<DbEvent> dry_flushed_snapshot_for_test() const {
        std::lock_guard<std::mutex> lk(dry_flushed_mu_);
        return dry_flushed_;
    }

private:
    void consumer_loop();
    bool flush_batch(std::vector<DbEvent>& batch);
    bool create_schema_internal();
    bool ensure_conn(size_t idx);
    bool exec_one_event(PgConnImpl& impl, const DbEvent& ev);
    void update_daily_current(const DbDailyPnl& d);

    const infra::Config& cfg_;
    DbEventRing& ring_;
    // Serializes concurrent push() callers so the SPSC ring keeps a single
    // producer (see PgReporter::push).
    std::mutex push_mu_;
    std::string dsn_;
    bool dry_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::atomic<uint64_t> flushed_count_{0};
    std::atomic<uint64_t> pending_count_{0};
    std::atomic<uint64_t> dropped_count_{0};

    // Failed-flush retry state — only touched by the consumer thread.
    int consecutive_failures_{0};

    // RAII connection pool. nullptr entries are reconnected lazily.
    std::vector<std::unique_ptr<PgConnImpl>> pool_;
    size_t next_conn_{0};

    // Daily P&L rollover state — only touched by the consumer thread.
    int32_t last_pnl_date_int_{0};
    bool has_current_day_{false};
    DbDailyPnl current_day_{};

    // Test-only poison injection: when set, flush_batch prepends an invalid
    // event before tx so the batch fails and per-event retry must skip it.
    std::atomic<bool> poison_next_{false};

    // Test-only: dry-mode flushed events captured for content assertions.
    mutable std::mutex dry_flushed_mu_;
    std::vector<DbEvent> dry_flushed_;
};

}
