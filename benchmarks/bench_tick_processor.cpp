// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

// Target: <1µs median per tick (apply update + snapshot build).
#include <array>
#include <memory>

#include <benchmark/benchmark.h>

#include "infra/config.hpp"
#include "infra/logger.hpp"
#include "market_data/tick_processor.hpp"

using namespace spreadara;

namespace {

infra::Config make_cfg() {
    infra::Config c{};
    c.market_data.symbol = "BTCUSDT";
    c.market_data.depth_levels = 20;
    c.market_data.volatility_window = 100;
    c.transport.ring_buffer_capacity = 65536;
    c.runtime.ws_cpu_core = -1;
    c.runtime.processor_cpu_core = -1;
    c.logging.level = "off";
    c.logging.file = "";
    return c;
}

market_data::MarketEvent make_depth_event(uint64_t prev_u, uint64_t u) {
    market_data::MarketEvent ev;
    ev.type = market_data::EventType::Depth;
    ev.depth.exchange_ts_ms = 1234567890;
    ev.depth.first_update_id = prev_u + 1;
    ev.depth.final_update_id = u;
    ev.depth.prev_final_update_id = prev_u;
    ev.depth.bid_count = 20;
    ev.depth.ask_count = 20;
    for (int i = 0; i < 20; ++i) {
        ev.depth.bids[i] = {100.0 - i * 0.1, 1.0 + i * 0.05};
        ev.depth.asks[i] = {101.0 + i * 0.1, 1.0 + i * 0.05};
    }
    return ev;
}

}

static void BM_TickProcessorDepth(benchmark::State& state) {
    auto cfg = make_cfg();
    infra::init_logger("off", "");
    auto ring = std::make_unique<market_data::EventRing>();
    market_data::TickProcessor proc(cfg, *ring);

    auto seed = make_depth_event(0, 1);
    proc.process_event(seed);

    uint64_t u = 1;
    for (auto _ : state) {
        auto ev = make_depth_event(u, u + 1);
        ++u;
        proc.process_event(ev);
        benchmark::DoNotOptimize(proc.book().mid());
    }
}
BENCHMARK(BM_TickProcessorDepth);

BENCHMARK_MAIN();
