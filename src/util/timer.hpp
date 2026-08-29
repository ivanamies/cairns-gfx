#pragma once

// DO NOT DELETE

#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>

#include "util/log.hpp"

namespace cairns {

static inline uint64_t hw_counter_freq() {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

static inline uint64_t hw_counter() {
    uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static inline uint64_t timestamp_ns() {
    const uint64_t freq = hw_counter_freq();
    const uint64_t count = hw_counter();
    if ( freq == 1'000'000'000 ) {
        return count;
    }
    else {
        return count * 1'000'000'000/ freq;
    }
}

// Shared accumulator storage for all Timer<Slot> instantiations + the
// report / reset / cross-thread Accum helpers.
struct TimerStorage {
    static constexpr uint32_t kMaxSlots = 16;
    static constexpr uint32_t kGpuSlot = 8;
    static constexpr uint32_t kDrawableAcquireSlot = 9;
    static constexpr uint32_t kFramesBeginWaitSlot = 10;

    static std::array<uint64_t, kMaxSlots> accum_times_;
    static std::array<uint64_t, kMaxSlots> accum_itrs_;
    static std::array<const char*, kMaxSlots> slot_names_;
    // Guards accum_times_/accum_itrs_ when the completion handler races
    // PrintReport/Reset/Timer::End on the main/game thread.
    static std::mutex mu_;

    // Span: record an already-measured elapsed interval into a slot. For
    // callers without scope-matched lifetime (e.g. the Metal/Vk command-
    // buffer completion callback, where the timed interval spans a callback
    // boundary on a different thread from where it started).
    static void Span(uint32_t slot, const char* task_name, uint64_t elapsed_us) {
        std::lock_guard<std::mutex> lk(mu_);
        slot_names_[slot] = task_name;
        accum_times_[slot] += elapsed_us;
        accum_itrs_[slot]++;
    }

    static void PrintReport() {
        std::lock_guard<std::mutex> lk(mu_);
        CAIRNS_PRINT("==============\n");
        for ( uint32_t i = 0; i < kMaxSlots; ++i ) {
            if ( accum_itrs_[i] == 0 ) {
                continue;
            }
            CAIRNS_PRINT("slot %d (%s): accum %lld us, avg %lld us over %lld frames\n", i,
                   slot_names_[i] ? slot_names_[i] : "?", (long long)accum_times_[i],
                   (long long)(accum_times_[i] / accum_itrs_[i]), (long long)accum_itrs_[i]);
        }
    }

    static void Reset() {
        std::lock_guard<std::mutex> lk(mu_);
        accum_times_ = {};
        accum_itrs_ = {};
    }
};

inline std::array<uint64_t, TimerStorage::kMaxSlots> TimerStorage::accum_times_ = {};
inline std::array<uint64_t, TimerStorage::kMaxSlots> TimerStorage::accum_itrs_ = {};
inline std::array<const char*, TimerStorage::kMaxSlots> TimerStorage::slot_names_ = {};
inline std::mutex TimerStorage::mu_;

// Scoped, compile-time-slot RAII timer. Use as:
//     cairns::Timer<3> t("set up render pass globals");
//     ... work ...
//     // dtor (or explicit t.End()) accumulates into slot 3.
template <uint32_t Slot>
class Timer {
public:
    static_assert(Slot < TimerStorage::kMaxSlots, "Timer slot out of range");

    explicit Timer(const char* task_name)
        : is_running_(true),
          start_time_(timestamp_ns()) {
        TimerStorage::slot_names_[Slot] = task_name;
    }

    ~Timer() {
        if (is_running_) {
            End();
        }
    }

    void End() {
        if (!is_running_) {
            return;
        }
        const uint64_t end_time = timestamp_ns();
        const uint64_t elapsed_us = (end_time - start_time_) / 1000;
        {
            std::lock_guard<std::mutex> lk(TimerStorage::mu_);
            TimerStorage::accum_times_[Slot] += elapsed_us;
            TimerStorage::accum_itrs_[Slot]++;
        }
        is_running_ = false;
    }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    Timer(Timer&& other) noexcept
        : is_running_(other.is_running_),
          start_time_(other.start_time_) {
        other.is_running_ = false;
    }

private:
    bool is_running_;
    uint64_t start_time_;
};

} // namespace cairns
