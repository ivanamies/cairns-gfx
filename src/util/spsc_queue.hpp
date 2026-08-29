#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <semaphore>

namespace cairns {

// Fixed-capacity single-producer / single-consumer ring with blocking Push/Pop.
// Push blocks the producer while the ring is full; Pop blocks the consumer
// while the ring is empty. Capacity is a compile-time constant so the
// semaphores are template-instantiated to the right depth.
//
// Intended use: game thread -> render thread FramePacket handoff with
// capacity = kFramesInFlight (= 2). The producer can be at most kCapacity
// frames ahead of the consumer.
template <typename T, size_t kCapacity>
class SpscQueue {
public:
    static_assert(kCapacity >= 1, "SpscQueue capacity must be >= 1");

    SpscQueue() = default;
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // PRODUCER. Blocks if the ring is full.
    void Push(const T& v) {
        free_slots_.acquire();
        const size_t w = write_.load(std::memory_order_relaxed);
        buffer_[w % kCapacity] = v;
        write_.store(w + 1, std::memory_order_release);
        filled_slots_.release();
    }

    // CONSUMER. Blocks if the ring is empty.
    T Pop() {
        filled_slots_.acquire();
        const size_t r = read_.load(std::memory_order_relaxed);
        T v = buffer_[r % kCapacity];
        read_.store(r + 1, std::memory_order_release);
        free_slots_.release();
        return v;
    }

    // CONSUMER. Non-blocking; returns false if empty.
    bool TryPop(T& out) {
        if (!filled_slots_.try_acquire()) {
            return false;
        }
        const size_t r = read_.load(std::memory_order_relaxed);
        out = buffer_[r % kCapacity];
        read_.store(r + 1, std::memory_order_release);
        free_slots_.release();
        return true;
    }

private:
    std::array<T, kCapacity> buffer_{};
    std::atomic<size_t> write_{0};
    std::atomic<size_t> read_{0};
    std::counting_semaphore<kCapacity> free_slots_{kCapacity};
    std::counting_semaphore<kCapacity> filled_slots_{0};
};

}  // namespace cairns
