// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

// Unit tests for backtest building blocks. Hits the historical
// loader, simulated rest client, and reporter; integration coverage lives in
// test_integration.cpp.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <flatbuffers/flatbuffers.h>
#include <gtest/gtest.h>

#include "backtest/backtest_reporter.hpp"
#include "backtest/historical_data_loader.hpp"
#include "execution/simulated_rest_client.hpp"
#include "infra/config.hpp"
#include "market_snapshot_generated.h"

using namespace spreadara;

namespace {

infra::Config mk_cfg() {
    infra::Config c{};
    c.market_data.symbol = "BTCUSDT";
    c.market_data.volatility_window = 100;
    c.backtest.initial_capital = 10000.0;
    c.backtest.risk_free_rate = 0.05;
    c.reporter.flush_interval_ms = 1000;
    return c;
}

}  // namespace

TEST(HistoricalLoader, CsvToFbRoundtrip) {
    auto cfg = mk_cfg();
    backtest::HistoricalDataLoader loader(cfg);

    std::string csv = "/tmp/sp6_test_a.csv";
    std::string bin = "/tmp/sp6_test_a.bin";
    {
        std::FILE* f = std::fopen(csv.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fprintf(f, "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time\n");
        std::fprintf(f, "1,100.0,0.5,101.0,0.5,1700000000000,1700000000001\n");
        std::fprintf(f, "2,100.5,0.5,101.5,0.5,1700000001000,1700000001001\n");
        std::fclose(f);
    }
    ASSERT_TRUE(loader.load_csv(csv, bin));

    std::vector<double> mids;
    std::size_t count = 0;
    ASSERT_TRUE(loader.stream_archive(bin, [&](const uint8_t* d, std::size_t sz) {
        flatbuffers::Verifier v(d, sz);
        ASSERT_TRUE(schemas::VerifyMarketSnapshotBuffer(v));
        auto s = schemas::GetMarketSnapshot(d);
        mids.push_back(s->mid_price());
        ++count;
    }));
    EXPECT_EQ(count, 2u);
    ASSERT_EQ(mids.size(), 2u);
    EXPECT_NEAR(mids[0], 100.5, 1e-9);
    EXPECT_NEAR(mids[1], 101.0, 1e-9);
    std::remove(csv.c_str());
    std::remove(bin.c_str());
}

TEST(HistoricalLoader, ArchiveRoundTrip100) {
    // 100 rows, byte-equal on stream.
    auto cfg = mk_cfg();
    backtest::HistoricalDataLoader loader(cfg);
    std::string csv = "/tmp/sp6_test_b.csv";
    std::string bin = "/tmp/sp6_test_b.bin";
    {
        std::FILE* f = std::fopen(csv.c_str(), "w");
        ASSERT_NE(f, nullptr);
        std::fprintf(f, "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time\n");
        for (int i = 0; i < 100; ++i) {
            std::fprintf(f, "%d,%.2f,0.5,%.2f,0.5,%lu,%lu\n",
                         i + 1, 100.0 + 0.01 * i, 101.0 + 0.01 * i,
                         1700000000000UL + i * 1000UL, 1700000000001UL + i * 1000UL);
        }
        std::fclose(f);
    }
    ASSERT_TRUE(loader.load_csv(csv, bin));
    std::size_t n = 0;
    ASSERT_TRUE(loader.stream_archive(bin, [&](const uint8_t*, std::size_t) { ++n; }));
    EXPECT_EQ(n, 100u);
    std::remove(csv.c_str());
    std::remove(bin.c_str());
}

TEST(SimulatedRestClient, FillsOnCrossAndCancel) {
    auto cfg = mk_cfg();
    execution::SimulatedRestClient sim(cfg, "BTCUSDT");

    std::vector<risk::FillInput> fills;
    sim.set_on_fill([&](const risk::FillInput& f) { fills.push_back(f); });

    // Initial book: bid=100, ask=101 with 0.5 on each side.
    sim.update_market(100.0, 101.0, 0.5, 0.5, 1ULL);

    // Resting BUY @ 99.5 (below ask): should NOT fill.
    auto a1 = sim.place_order("BUY", 0.3, 99.5, true, "cid-1");
    EXPECT_TRUE(a1.ok);
    EXPECT_EQ(fills.size(), 0u);

    // Market drops so ask=99.0 — BUY @ 99.5 now crosses. partial fill capped
    // by ask_qty=0.2.
    sim.update_market(98.0, 99.0, 0.5, 0.2, 2ULL);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].side, +1);
    EXPECT_NEAR(fills[0].qty, 0.2, 1e-9);
    EXPECT_NEAR(fills[0].price, 99.5, 1e-9);

    // Remainder fills when more depth arrives.
    sim.update_market(98.0, 99.0, 0.5, 0.5, 3ULL);
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_NEAR(fills[1].qty, 0.1, 1e-9);

    // Cancel a no-op cid is idempotent ok.
    auto c = sim.cancel_order("cid-1", 0);
    EXPECT_TRUE(c.ok);

    // Place + cancel before crossing.
    sim.place_order("SELL", 0.4, 200.0, true, "cid-2");
    auto c2 = sim.cancel_order("cid-2", 0);
    EXPECT_TRUE(c2.ok);
    EXPECT_EQ(sim.pending_count(), 0u);
}

TEST(BacktestReporter, SharpeOnKnownSeries) {
    // Returns 1% per sample, zero variance => infinite Sharpe;
    // use a non-zero-variance series instead.
    backtest::BacktestReporter r(/*capital*/1000.0, /*rf*/0.0, /*flush_ms*/1000);
    r.on_equity_sample(1000.0);
    r.on_equity_sample(1010.0);
    r.on_equity_sample(1005.0);
    r.on_equity_sample(1015.0);
    auto s = r.finalize();
    EXPECT_GT(s.final_equity, 0.0);
    EXPECT_NEAR(s.total_pnl, 15.0, 1e-9);
    EXPECT_GT(s.sharpe_ratio, 0.0);
    EXPECT_GE(s.max_drawdown_pct, 0.0);
}
