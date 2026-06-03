#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "db/pg_reporter.hpp"
#include "infra/config.hpp"

using namespace spreadara;

static infra::Config make_test_cfg(int batch_size = 100, int flush_ms = 100) {
    infra::Config c{};
    c.reporter.core = -1;
    c.reporter.batch_size = batch_size;
    c.reporter.flush_interval_ms = flush_ms;
    c.reporter.pg_pool_min = 0;
    c.transport.db_ring_capacity = 4096;
    return c;
}

static db::DbEvent make_trade_event(int i) {
    db::DbEvent e{};
    e.kind = db::DbEventKind::Trade;
    std::snprintf(e.trade.order_id, sizeof(e.trade.order_id), "tst-%d", i);
    e.trade.side = (i % 2) ? 1 : -1;
    e.trade.price = 30000.0 + i;
    e.trade.qty = 0.001;
    e.trade.fee = 0.01;
    std::snprintf(e.trade.fee_asset, sizeof(e.trade.fee_asset), "USDT");
    e.trade.ts_ns = static_cast<uint64_t>(i) * 1'000'000ULL;
    return e;
}

TEST(PgReporter, DryModeBatchFlushBySize) {
    auto cfg = make_test_cfg(/*batch_size=*/10, /*flush_ms=*/10000);
    db::DbEventRing ring;
    db::PgReporter r(cfg, ring, /*dsn=*/"");
    r.start();

    // Push batch_size + 1 = 11 events.
    for (int i = 0; i < 11; ++i) {
        ASSERT_TRUE(r.push(make_trade_event(i)));
    }
    // Wait until a size-triggered flush happens.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (r.flushed_count() < 10 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GE(r.flushed_count(), 10u);
    // The 11th event remains pending (not size-triggered yet, no time-trigger).
    EXPECT_LE(r.pending_count(), 1u);
    r.stop();
    // After stop, drain flushes everything.
    EXPECT_GE(r.flushed_count(), 11u);
}

TEST(PgReporter, DryModeBatchFlushByTime) {
    auto cfg = make_test_cfg(/*batch_size=*/1000, /*flush_ms=*/50);
    db::DbEventRing ring;
    db::PgReporter r(cfg, ring, /*dsn=*/"");
    r.start();

    ASSERT_TRUE(r.push(make_trade_event(1)));
    // Wait > flush_interval_ms.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (r.flushed_count() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(r.flushed_count(), 1u);
    r.stop();
}

static bool can_connect_local_pg(std::string& out_dsn) {
    // WHY: live schema test requires a real DSN. Prefer SPREADARA_PG_DSN
    // (set by ctest); if unset or unreachable, skip with explanation.
    const char* env = std::getenv("SPREADARA_PG_DSN");
    if (!env || !*env) return false;
    out_dsn = env;
    try {
        pqxx::connection c(out_dsn);
        return c.is_open();
    } catch (...) {
        return false;
    }
}

TEST(PgReporter, SchemaCreationIdempotent) {
    std::string dsn;
    if (!can_connect_local_pg(dsn)) {
        GTEST_SKIP() << "SPREADARA_PG_DSN unset or postgres unreachable; "
                        "see README 'Postgres setup for tests'";
    }
    // Create a per-test schema and run schema creation inside it.
    const std::string schema_name = "spreadara_test_" + std::to_string(::getpid());
    try {
        pqxx::connection c(dsn);
        pqxx::work w(c);
        w.exec("DROP SCHEMA IF EXISTS " + w.quote_name(schema_name) + " CASCADE");
        w.exec("CREATE SCHEMA " + w.quote_name(schema_name));
        w.commit();
    } catch (const std::exception& ex) {
        GTEST_SKIP() << "could not prepare test schema: " << ex.what();
    }

    const std::string scoped_dsn = dsn + " options=-csearch_path=" + schema_name;
    auto cfg = make_test_cfg();
    db::DbEventRing ring;
    db::PgReporter r(cfg, ring, scoped_dsn);

    EXPECT_TRUE(r.create_schema_for_test());
    EXPECT_TRUE(r.create_schema_for_test());

    try {
        pqxx::connection c(dsn);
        pqxx::work w(c);
        w.exec("DROP SCHEMA IF EXISTS " + w.quote_name(schema_name) + " CASCADE");
        w.commit();
    } catch (...) {}
}

// WHY: dry-mode test — verifies that same-date DailyPnl events accumulate
// in-memory and only a date rollover causes a flush. We can't directly
// inspect the in-memory current_day_ but we can observe flushed_count.
TEST(PgReporter, DailyPnlOnlyFlushesOnRollover) {
    auto cfg = make_test_cfg(/*batch_size=*/100, /*flush_ms=*/30);
    db::DbEventRing ring;
    db::PgReporter r(cfg, ring, /*dsn=*/"");
    r.start();

    db::DbEvent ev1{};
    ev1.kind = db::DbEventKind::DailyPnl;
    ev1.daily.date = 20260515;
    ev1.daily.realized = 1.0;
    ev1.daily.total = 1.0;
    ASSERT_TRUE(r.push(ev1));

    db::DbEvent ev2{};
    ev2.kind = db::DbEventKind::DailyPnl;
    ev2.daily.date = 20260515;
    ev2.daily.realized = 2.0;
    ev2.daily.total = 2.0;
    ASSERT_TRUE(r.push(ev2));

    // Give the consumer a chance — same-date events must NOT trigger a flush.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(r.flushed_count(), 0u);

    // Push a different date — should produce a single rollover flush.
    db::DbEvent ev3{};
    ev3.kind = db::DbEventKind::DailyPnl;
    ev3.daily.date = 20260516;
    ev3.daily.realized = 3.0;
    ev3.daily.total = 3.0;
    ASSERT_TRUE(r.push(ev3));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (r.flushed_count() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(r.flushed_count(), 1u);
    r.stop();
    EXPECT_GE(r.flushed_count(), 2u);

    // WHY: verify CONTENT not just count. The rollover flush must carry the
    // OLD day's final totals (ev2's values) under the OLD date — not the new
    // day's first snapshot stamped under the old date.
    auto flushed = r.dry_flushed_snapshot_for_test();
    ASSERT_EQ(flushed.size(), 2u);
    EXPECT_EQ(flushed[0].kind, db::DbEventKind::DailyPnl);
    EXPECT_EQ(flushed[0].daily.date, 20260515);
    EXPECT_DOUBLE_EQ(flushed[0].daily.realized, 2.0);
    EXPECT_DOUBLE_EQ(flushed[0].daily.total, 2.0);
    EXPECT_EQ(flushed[1].kind, db::DbEventKind::DailyPnl);
    EXPECT_EQ(flushed[1].daily.date, 20260516);
    EXPECT_DOUBLE_EQ(flushed[1].daily.realized, 3.0);
    EXPECT_DOUBLE_EQ(flushed[1].daily.total, 3.0);
}

// WHY: poison-pill — flush_batch must NOT lose the entire batch when one
// event is bad. The retry path commits good events one-by-one and skips
// the bad one. Exercises the per-event single-row fallback path.
TEST(PgReporter, PoisonPillRetainsGoodEvents) {
    std::string dsn;
    if (!can_connect_local_pg(dsn)) {
        GTEST_SKIP() << "SPREADARA_PG_DSN unset or postgres unreachable";
    }
    const std::string schema_name = "spreadara_poison_" + std::to_string(::getpid());
    try {
        pqxx::connection c(dsn);
        pqxx::work w(c);
        w.exec("DROP SCHEMA IF EXISTS " + w.quote_name(schema_name) + " CASCADE");
        w.exec("CREATE SCHEMA " + w.quote_name(schema_name));
        w.commit();
    } catch (const std::exception& ex) {
        GTEST_SKIP() << "could not prepare test schema: " << ex.what();
    }
    const std::string scoped_dsn = dsn + " options=-csearch_path=" + schema_name;
    auto cfg = make_test_cfg(/*batch_size=*/3, /*flush_ms=*/50);
    cfg.reporter.pg_pool_min = 1;
    db::DbEventRing ring;
    db::PgReporter r(cfg, ring, scoped_dsn);
    ASSERT_TRUE(r.create_schema_for_test());
    r.start();

    r.inject_poison_for_test();
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(r.push(make_trade_event(i)));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (r.flushed_count() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // All three good trade events should land via the per-event fallback.
    EXPECT_GE(r.flushed_count(), 3u);
    r.stop();

    try {
        pqxx::connection c(dsn);
        pqxx::work w(c);
        w.exec("DROP SCHEMA IF EXISTS " + w.quote_name(schema_name) + " CASCADE");
        w.commit();
    } catch (...) {}
}
