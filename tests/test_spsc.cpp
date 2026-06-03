// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#include <atomic>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

#include "transport/spsc_ring_buffer.hpp"

using spreadara::transport::SpscRingBuffer;

TEST(Spsc, SingleThreadPushPop) {
    SpscRingBuffer<int, 8> ring;
    int v;
    EXPECT_FALSE(ring.pop(v));
    EXPECT_TRUE(ring.push(42));
    EXPECT_TRUE(ring.pop(v));
    EXPECT_EQ(v, 42);
    EXPECT_FALSE(ring.pop(v));
}

TEST(Spsc, FullAndEmptyEdges) {
    SpscRingBuffer<int, 4> ring;  // capacity 3 usable
    EXPECT_TRUE(ring.push(1));
    EXPECT_TRUE(ring.push(2));
    EXPECT_TRUE(ring.push(3));
    EXPECT_FALSE(ring.push(4));

    int v;
    EXPECT_TRUE(ring.pop(v)); EXPECT_EQ(v, 1);
    EXPECT_TRUE(ring.pop(v)); EXPECT_EQ(v, 2);
    EXPECT_TRUE(ring.pop(v)); EXPECT_EQ(v, 3);
    EXPECT_FALSE(ring.pop(v));
}

TEST(Spsc, ProducerConsumerStress) {
    static constexpr std::size_t kN = 2'000'000;
    SpscRingBuffer<uint64_t, 1024> ring;

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (uint64_t i = 0; i < kN;) {
            if (ring.push(i)) {
                ++i;
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    uint64_t expected = 0;
    while (expected < kN) {
        uint64_t v;
        if (ring.pop(v)) {
            ASSERT_EQ(v, expected);
            ++expected;
        }
    }

    producer.join();
    EXPECT_TRUE(producer_done.load());
    EXPECT_EQ(expected, kN);
}
