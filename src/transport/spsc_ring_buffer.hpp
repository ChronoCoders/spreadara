// Copyright (c) 2026 ChronoCoders. All rights reserved.
// Proprietary and confidential. Unauthorized copying or distribution is prohibited.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace spreadara::transport {

template <typename T, std::size_t N>
class SpscRingBuffer {
    static_assert((N & (N - 1)) == 0, "Capacity must be a power of two");
    static_assert(N >= 2, "Capacity must be at least 2");

public:
    SpscRingBuffer() = default;
    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    bool push(const T& item) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool push(T&& item) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        buffer_[head] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = std::move(buffer_[tail]);
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() { return N - 1; }

private:
    static constexpr std::size_t kMask = N - 1;

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) std::array<T, N> buffer_{};
};

}
