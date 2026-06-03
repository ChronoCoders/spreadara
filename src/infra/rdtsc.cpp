// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include "infra/rdtsc.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace spreadara::infra {

namespace {
std::atomic<double> g_tsc_ghz{0.0};
}

void calibrate_tsc() {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const uint64_t c0 = rdtsc_cycles();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto t1 = clock::now();
    const uint64_t c1 = rdtsc_cycles();

    const double ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    const double cycles = static_cast<double>(c1 - c0);
    g_tsc_ghz.store(cycles / ns, std::memory_order_release);
}

double tsc_ghz() {
    return g_tsc_ghz.load(std::memory_order_acquire);
}

uint64_t cycles_to_ns(uint64_t cycles) {
    const double ghz = tsc_ghz();
    if (ghz <= 0.0) {
        return 0;
    }
    return static_cast<uint64_t>(static_cast<double>(cycles) / ghz);
}

}
