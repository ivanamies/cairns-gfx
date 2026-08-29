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

class Timer {
 public:

    static constexpr uint32_t kMaxSlots = 16;
    // Reserved slot for GPU frame time written by Metal/Vk command-buffer
    // completion callbacks (different thread than Timer::End). Note this
    // measures cmd-buffer-create -> cmd-buffer-complete, which on late-acquire
    // backends includes the nextDrawable / vkAcquireNextImageKHR wait
    // inside the encoding window. Use kDrawableAcquireSlot to split that out.
    static constexpr uint32_t kGpuSlot = 8;
    static constexpr uint32_t kDrawableAcquireSlot = 9;
    // dispatch_semaphore_wait / vkWaitForFences at top of Frames::Begin --
    // the kFramesInFlight gate blocking on previous GPU completions.
    static constexpr uint32_t kFramesBeginWaitSlot = 10;
    static std::array<uint64_t, kMaxSlots> accum_times_;
    static std::array<uint64_t, kMaxSlots> accum_itrs_;
    static std::array<const char*, kMaxSlots> slot_names_;
    // Guards accum_times_/accum_itrs_ when the completion handler races
    // PrintReport/Reset/Timer::End on the main/game thread.
    static std::mutex mu_;

  explicit Timer(const char* task_name, uint32_t slot)
      : slot_(slot),
        is_running_(true),
        start_time_(timestamp_ns()) {
    slot_names_[slot] = task_name;
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

    uint64_t end_time = timestamp_ns();
    uint64_t elapsed_us = (end_time - start_time_)/1000;

    {
      std::lock_guard<std::mutex> lk(mu_);
      accum_times_[slot_] += elapsed_us;
      accum_itrs_[slot_]++;
    }

    is_running_ = false;
  }

    // Accumulator setter for callers that don't have a scoped Timer (e.g. the
    // Metal/Vk command-buffer completion callback writing GPU frame time
    // from a queue thread).
    static void Accum(uint32_t slot, const char* task_name, uint64_t elapsed_us) {
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

  // Prevent copying to ensure one timer per scope/task
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

  // Allow moving if ownership needs to be transferred
  Timer(Timer&& other) noexcept
      : slot_(other.slot_),
        is_running_(other.is_running_),
        start_time_(other.start_time_) {
    other.is_running_ = false;
  }

 private:
    uint32_t slot_;
  bool is_running_;
  uint64_t start_time_;
};


// todo @iamies move this out
inline std::array<uint64_t, Timer::kMaxSlots> Timer::accum_times_ = {};
inline std::array<uint64_t, Timer::kMaxSlots> Timer::accum_itrs_ = {};
inline std::array<const char*, Timer::kMaxSlots> Timer::slot_names_ = {};
inline std::mutex Timer::mu_;

} // namespace cairns
