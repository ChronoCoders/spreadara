// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "db/pg_reporter.hpp"

#include <chrono>
#include <cstring>

#include <pqxx/pqxx>
#include <spdlog/spdlog.h>

#include "infra/cpu_affinity.hpp"

namespace spreadara::db {

struct PgConnImpl {
    pqxx::connection c;
    explicit PgConnImpl(const std::string& dsn) : c(dsn) {}
};

namespace {

// WHY: ts_ns BIGINT columns hold raw nanoseconds-since-epoch; the dashboard
// converts to ISO8601 client-side. Avoids the to_timestamp() round-trip and
// keeps trade timestamps lossless across the wire.
constexpr const char* kSchemaSql =
    "CREATE TABLE IF NOT EXISTS trades ("
    "  id BIGSERIAL PRIMARY KEY,"
    "  ts_ns BIGINT NOT NULL,"
    "  order_id TEXT NOT NULL,"
    "  side SMALLINT NOT NULL,"
    "  price DOUBLE PRECISION NOT NULL,"
    "  qty DOUBLE PRECISION NOT NULL,"
    "  fee DOUBLE PRECISION NOT NULL,"
    "  fee_asset TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS trades_ts_ns_desc ON trades(ts_ns DESC);"
    "CREATE TABLE IF NOT EXISTS position_snapshots ("
    "  id BIGSERIAL PRIMARY KEY,"
    "  ts_ns BIGINT NOT NULL,"
    "  inventory DOUBLE PRECISION NOT NULL,"
    "  avg_entry DOUBLE PRECISION NOT NULL,"
    "  realized_pnl DOUBLE PRECISION NOT NULL,"
    "  unrealized_pnl DOUBLE PRECISION NOT NULL,"
    "  total_fees DOUBLE PRECISION NOT NULL,"
    "  mid_price DOUBLE PRECISION NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS position_snapshots_ts_ns_desc ON position_snapshots(ts_ns DESC);"
    "CREATE TABLE IF NOT EXISTS system_events ("
    "  id BIGSERIAL PRIMARY KEY,"
    "  ts_ns BIGINT NOT NULL,"
    "  severity TEXT NOT NULL,"
    "  source TEXT NOT NULL,"
    "  code TEXT NOT NULL,"
    "  msg TEXT NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS system_events_ts_ns_desc ON system_events(ts_ns DESC);"
    "CREATE TABLE IF NOT EXISTS daily_pnl ("
    "  date DATE PRIMARY KEY,"
    "  realized DOUBLE PRECISION NOT NULL,"
    "  unrealized DOUBLE PRECISION NOT NULL,"
    "  fees DOUBLE PRECISION NOT NULL,"
    "  total DOUBLE PRECISION NOT NULL"
    ");"
    // Idempotent telemetry columns. DOUBLE PRECISION (not NUMERIC) to
    // match the rest of the schema and avoid pqxx <-> numeric conversion friction.
    // T_param (not T) avoids reserved-feeling identifier conflicts in some clients.
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS best_bid    DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS best_ask    DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS spread_bps  DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS bid_qty     DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS ask_qty     DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS volatility  DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS gamma       DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS k           DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS T_param     DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS lat_p50_us  DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS lat_p95_us  DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS lat_p99_us  DOUBLE PRECISION;"
    "ALTER TABLE position_snapshots ADD COLUMN IF NOT EXISTS open_orders INTEGER;"
    "ALTER TABLE trades             ADD COLUMN IF NOT EXISTS is_maker    BOOLEAN DEFAULT TRUE;";

std::string date_int_to_iso(int32_t d) {
    // d is yyyymmdd
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  d / 10000, (d / 100) % 100, d % 100);
    return std::string(buf);
}

}  // namespace

PgReporter::PgReporter(const infra::Config& cfg, DbEventRing& ring, const std::string& dsn)
    : cfg_(cfg), ring_(ring), dsn_(dsn), dry_(dsn.empty()) {}

PgReporter::~PgReporter() {
    stop();
    pool_.clear();  // RAII
}

bool PgReporter::push(const DbEvent& e) {
    return ring_.push(e);
}

bool PgReporter::ensure_conn(size_t idx) {
    if (dry_) return false;
    if (idx >= pool_.size()) return false;
    if (pool_[idx]) return true;
    try {
        pool_[idx] = std::make_unique<PgConnImpl>(dsn_);
        return true;
    } catch (const std::exception& ex) {
        spdlog::warn("pg_reporter_connect_failed pool_idx={} err={}", idx, ex.what());
        return false;
    }
}

bool PgReporter::create_schema_internal() {
    if (dry_) return true;
    if (pool_.empty()) pool_.resize(1);
    if (!ensure_conn(0)) return false;
    try {
        pqxx::work tx(pool_[0]->c);
        tx.exec(kSchemaSql);
        tx.commit();
        return true;
    } catch (const std::exception& ex) {
        spdlog::warn("pg_reporter_schema_failed err={}", ex.what());
        return false;
    }
}

bool PgReporter::create_schema_for_test() {
    return create_schema_internal();
}

void PgReporter::start() {
    if (running_.exchange(true)) return;
    if (!dry_) {
        // WHY: open the configured pool now so first-flush latency excludes
        // connection establishment. Schema creation only runs on connection 0.
        const int n = std::max(1, cfg_.reporter.pg_pool_min);
        pool_.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            if (!ensure_conn(static_cast<size_t>(i))) {
                spdlog::warn("pg_reporter_pool_partial idx={}", i);
            }
        }
        if (create_schema_internal()) {
            spdlog::info("pg_reporter_schema_ok pool_size={}", n);
        } else {
            spdlog::warn("pg_reporter_schema_failed_at_startup retry_on_first_flush");
        }
    } else {
        spdlog::info("pg_reporter_dry_mode_no_dsn");
    }
    thread_ = std::thread(&PgReporter::consumer_loop, this);
}

void PgReporter::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void PgReporter::update_daily_current(const DbDailyPnl& d) {
    if (!has_current_day_) {
        current_day_ = d;
        has_current_day_ = true;
        if (last_pnl_date_int_ == 0) last_pnl_date_int_ = d.date;
        return;
    }
    // Same-day update: replace running totals with the latest snapshot.
    if (d.date == current_day_.date) {
        current_day_ = d;
    } else {
        // Day changed — caller (consumer loop) handles the rollover flush.
        current_day_ = d;
    }
}

void PgReporter::consumer_loop() {
    if (cfg_.reporter.core >= 0) {
        spreadara::infra::pin_current_thread_to_core(cfg_.reporter.core);
    }
    std::vector<DbEvent> batch;
    batch.reserve(static_cast<size_t>(cfg_.reporter.batch_size) * 2);
    auto last_flush = std::chrono::steady_clock::now();
    const auto flush_interval = std::chrono::milliseconds(cfg_.reporter.flush_interval_ms);

    while (running_.load(std::memory_order_acquire)) {
        DbEvent e;
        bool got_any = false;
        while (batch.size() < static_cast<size_t>(cfg_.reporter.batch_size) * 2 && ring_.pop(e)) {
            // WHY: DailyPnl events accumulate in current_day_ and only flush
            // when the UTC date changes. Per-tick events would inflate write
            // volume — we want one row per day, written at rollover.
            if (e.kind == DbEventKind::DailyPnl) {
                // WHY: flush the OLD day BEFORE updating current_day_. Otherwise
                // update_daily_current overwrites the closed-out day's final
                // totals with the new day's first snapshot, and the flushed row
                // ends up with new-day numbers stamped under the old date.
                if (has_current_day_ && current_day_.date != 0 &&
                    e.daily.date != current_day_.date) {
                    DbEvent flush_ev{};
                    flush_ev.kind = DbEventKind::DailyPnl;
                    flush_ev.daily = current_day_;
                    batch.push_back(flush_ev);
                    last_pnl_date_int_ = e.daily.date;
                }
                update_daily_current(e.daily);
                got_any = true;
                continue;
            }
            batch.push_back(e);
            got_any = true;
        }
        pending_count_.store(static_cast<uint64_t>(batch.size()), std::memory_order_release);

        const auto now = std::chrono::steady_clock::now();
        const bool size_ready = batch.size() >= static_cast<size_t>(cfg_.reporter.batch_size);
        const bool time_ready = !batch.empty() && (now - last_flush) >= flush_interval;
        if (size_ready || time_ready) {
            if (flush_batch(batch)) {
                last_flush = now;
            } else {
                if (consecutive_failures_ >= 2) {
                    spdlog::critical("pg_reporter_drop_batch size={} after_failures={}",
                                     batch.size(), consecutive_failures_);
                    dropped_count_.fetch_add(batch.size(), std::memory_order_acq_rel);
                    batch.clear();
                    consecutive_failures_ = 0;
                    last_flush = now;
                }
            }
            pending_count_.store(static_cast<uint64_t>(batch.size()), std::memory_order_release);
        }
        if (!got_any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // Drain on shutdown — flush current_day_ as final daily_pnl row.
    if (has_current_day_) {
        DbEvent flush_ev{};
        flush_ev.kind = DbEventKind::DailyPnl;
        flush_ev.daily = current_day_;
        batch.push_back(flush_ev);
        has_current_day_ = false;
    }
    DbEvent e;
    while (ring_.pop(e)) batch.push_back(e);
    if (!batch.empty()) {
        flush_batch(batch);
    }
}

bool PgReporter::exec_one_event(PgConnImpl& impl, const DbEvent& ev) {
    pqxx::work tx(impl.c);
    switch (ev.kind) {
        case DbEventKind::Trade: {
            const auto& t = ev.trade;
            tx.exec_params(
                "INSERT INTO trades(ts_ns, order_id, side, price, qty, fee, fee_asset, is_maker) "
                "VALUES($1, $2, $3, $4, $5, $6, $7, $8)",
                static_cast<int64_t>(t.ts_ns), std::string(t.order_id),
                static_cast<int>(t.side), t.price, t.qty, t.fee,
                std::string(t.fee_asset), t.is_maker);
            break;
        }
        case DbEventKind::PositionSnapshot: {
            const auto& s = ev.snap;
            tx.exec_params(
                "INSERT INTO position_snapshots(ts_ns, inventory, avg_entry, realized_pnl, "
                "unrealized_pnl, total_fees, mid_price, best_bid, best_ask, spread_bps, "
                "bid_qty, ask_qty, volatility, gamma, k, T_param, lat_p50_us, lat_p95_us, "
                "lat_p99_us, open_orders) "
                "VALUES($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, "
                "$16, $17, $18, $19, $20)",
                static_cast<int64_t>(s.ts_ns), s.inventory, s.avg_entry, s.realized,
                s.unrealized, s.fees, s.mid,
                s.best_bid, s.best_ask, s.spread_bps, s.bid_qty, s.ask_qty,
                s.volatility, s.gamma, s.k, s.T,
                s.lat_p50_us, s.lat_p95_us, s.lat_p99_us,
                static_cast<int>(s.open_orders));
            break;
        }
        case DbEventKind::SystemEvent: {
            const auto& e2 = ev.evt;
            tx.exec_params(
                "INSERT INTO system_events(ts_ns, severity, source, code, msg) "
                "VALUES($1, $2, $3, $4, $5)",
                static_cast<int64_t>(e2.ts_ns), std::string(e2.severity),
                std::string(e2.source), std::string(e2.code),
                std::string(e2.msg));
            break;
        }
        case DbEventKind::DailyPnl: {
            const auto& d = ev.daily;
            tx.exec_params(
                "INSERT INTO daily_pnl(date, realized, unrealized, fees, total) "
                "VALUES($1::date, $2, $3, $4, $5) "
                "ON CONFLICT (date) DO UPDATE SET "
                "realized=EXCLUDED.realized, unrealized=EXCLUDED.unrealized, "
                "fees=EXCLUDED.fees, total=EXCLUDED.total",
                date_int_to_iso(d.date), d.realized, d.unrealized, d.fees, d.total);
            break;
        }
    }
    tx.commit();
    return true;
}

bool PgReporter::flush_batch(std::vector<DbEvent>& batch) {
    if (batch.empty()) return true;
    const size_t n = batch.size();
    if (dry_) {
        {
            std::lock_guard<std::mutex> lk(dry_flushed_mu_);
            dry_flushed_.insert(dry_flushed_.end(), batch.begin(), batch.end());
        }
        flushed_count_.fetch_add(n, std::memory_order_acq_rel);
        batch.clear();
        consecutive_failures_ = 0;
        return true;
    }
    // WHY: round-robin across the pool; a failed flush invalidates only that
    // connection. Lazily reconnect on next pick.
    if (pool_.empty()) pool_.resize(1);
    const size_t idx = next_conn_ % pool_.size();
    next_conn_ = (next_conn_ + 1) % pool_.size();
    if (!ensure_conn(idx)) {
        ++consecutive_failures_;
        return false;
    }
    if (!pool_[0] && idx != 0) {
        // Schema may have been deferred — attempt once on connection 0.
        if (!create_schema_internal()) {
            ++consecutive_failures_;
            return false;
        }
    }

    auto try_whole_batch = [&]() -> bool {
        try {
            pqxx::work tx(pool_[idx]->c);
            // Test seam: poison injection causes an intentional SQL error in
            // the SAME transaction so the whole-batch txn fails.
            if (poison_next_.exchange(false, std::memory_order_acq_rel)) {
                tx.exec("INSERT INTO trades(no_such_column) VALUES (1)");
            }
            for (const auto& ev : batch) {
                switch (ev.kind) {
                    case DbEventKind::Trade: {
                        const auto& t = ev.trade;
                        tx.exec_params(
                            "INSERT INTO trades(ts_ns, order_id, side, price, qty, fee, fee_asset, is_maker) "
                            "VALUES($1, $2, $3, $4, $5, $6, $7, $8)",
                            static_cast<int64_t>(t.ts_ns), std::string(t.order_id),
                            static_cast<int>(t.side), t.price, t.qty, t.fee,
                            std::string(t.fee_asset), t.is_maker);
                        break;
                    }
                    case DbEventKind::PositionSnapshot: {
                        const auto& s = ev.snap;
                        tx.exec_params(
                            "INSERT INTO position_snapshots(ts_ns, inventory, avg_entry, realized_pnl, "
                            "unrealized_pnl, total_fees, mid_price, best_bid, best_ask, spread_bps, "
                            "bid_qty, ask_qty, volatility, gamma, k, T_param, lat_p50_us, lat_p95_us, "
                            "lat_p99_us, open_orders) "
                            "VALUES($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, "
                            "$16, $17, $18, $19, $20)",
                            static_cast<int64_t>(s.ts_ns), s.inventory, s.avg_entry, s.realized,
                            s.unrealized, s.fees, s.mid,
                            s.best_bid, s.best_ask, s.spread_bps, s.bid_qty, s.ask_qty,
                            s.volatility, s.gamma, s.k, s.T,
                            s.lat_p50_us, s.lat_p95_us, s.lat_p99_us,
                            static_cast<int>(s.open_orders));
                        break;
                    }
                    case DbEventKind::SystemEvent: {
                        const auto& e2 = ev.evt;
                        tx.exec_params(
                            "INSERT INTO system_events(ts_ns, severity, source, code, msg) "
                            "VALUES($1, $2, $3, $4, $5)",
                            static_cast<int64_t>(e2.ts_ns), std::string(e2.severity),
                            std::string(e2.source), std::string(e2.code),
                            std::string(e2.msg));
                        break;
                    }
                    case DbEventKind::DailyPnl: {
                        const auto& d = ev.daily;
                        tx.exec_params(
                            "INSERT INTO daily_pnl(date, realized, unrealized, fees, total) "
                            "VALUES($1::date, $2, $3, $4, $5) "
                            "ON CONFLICT (date) DO UPDATE SET "
                            "realized=EXCLUDED.realized, unrealized=EXCLUDED.unrealized, "
                            "fees=EXCLUDED.fees, total=EXCLUDED.total",
                            date_int_to_iso(d.date), d.realized, d.unrealized, d.fees, d.total);
                        break;
                    }
                }
            }
            tx.commit();
            return true;
        } catch (const std::exception& ex) {
            spdlog::warn("pg_reporter_flush_failed n={} err={}", batch.size(), ex.what());
            return false;
        }
    };

    if (try_whole_batch()) {
        flushed_count_.fetch_add(n, std::memory_order_acq_rel);
        batch.clear();
        consecutive_failures_ = 0;
        return true;
    }
    // First failure — connection state may be aborted; drop and reconnect.
    pool_[idx].reset();
    if (!ensure_conn(idx)) { ++consecutive_failures_; return false; }
    if (try_whole_batch()) {
        flushed_count_.fetch_add(n, std::memory_order_acq_rel);
        batch.clear();
        consecutive_failures_ = 0;
        return true;
    }
    // Second failure — fall back to per-event single-row txns. Skip the bad ones.
    pool_[idx].reset();
    if (!ensure_conn(idx)) { ++consecutive_failures_; return false; }
    size_t ok_count = 0;
    size_t skipped = 0;
    for (const auto& ev : batch) {
        try {
            if (!exec_one_event(*pool_[idx], ev)) {
                ++skipped;
                continue;
            }
            ++ok_count;
        } catch (const std::exception& ex) {
            spdlog::warn("pg_event_skipped reason={}", ex.what());
            ++skipped;
            // pqxx leaves the connection in an aborted state after an exception;
            // reset and reconnect so the next event can run.
            pool_[idx].reset();
            if (!ensure_conn(idx)) break;
        }
    }
    flushed_count_.fetch_add(ok_count, std::memory_order_acq_rel);
    dropped_count_.fetch_add(skipped, std::memory_order_acq_rel);
    batch.clear();
    consecutive_failures_ = 0;
    return true;
}

}
