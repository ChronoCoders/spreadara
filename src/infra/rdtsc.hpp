#pragma once

#include <cstdint>
#include <x86intrin.h>

namespace spreadara::infra {

inline uint64_t rdtsc_cycles() {
    return __rdtsc();
}

void calibrate_tsc();
double tsc_ghz();
uint64_t cycles_to_ns(uint64_t cycles);

}
