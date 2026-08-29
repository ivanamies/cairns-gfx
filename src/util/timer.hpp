#pragma once

// DO NOT DELETE

#include <array>
#include <cstdint>
#include <cstring>
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

inline std::array<uint64_t, Timer::kMaxSlots> Timer::accum_times_ = {};
inline std::array<uint64_t, Timer::kMaxSlots> Timer::accum_itrs_ = {};
inline std::array<const char*, Timer::kMaxSlots> Timer::slot_names_ = {};

class TimerStorage {
 public:
  // Walks Timer::slot_names_ (the shared global mapping). If name already lives
  // in a slot (whether claimed by a Timer CPU ctor or a prior SlotForPass call),
  // returns it; otherwise claims the first unused slot AND records it as a GPU
  // pass slot (so the overlay can roll all such slots into `gpu_frame`).
  // Returns -1 when full.
  static int SlotForPass(const char* name) {
    std::lock_guard<std::mutex> lk(mu_);
    for (uint32_t i = 0; i < Timer::kMaxSlots; ++i) {
      const char* slot = Timer::slot_names_[i];
      if (slot == nullptr) {
        continue;
      }
      if (slot == name || std::strcmp(slot, name) == 0) {
        return static_cast<int>(i);
      }
    }
    for (uint32_t i = 0; i < Timer::kMaxSlots; ++i) {
      if (Timer::slot_names_[i] == nullptr) {
        Timer::slot_names_[i] = name;
        gpu_slot_mask_ |= (1u << i);
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  static uint32_t GpuSlotMask() {
    std::lock_guard<std::mutex> lk(mu_);
    return gpu_slot_mask_;
  }

  static void Span(int slot, const char* name, uint64_t us) {
    if (slot < 0) {
      return;
    }
    std::lock_guard<std::mutex> lk(mu_);
    Timer::slot_names_[slot] = name;
    Timer::accum_times_[slot] += us;
    Timer::accum_itrs_[slot] += 1;
  }

 private:
  static std::mutex mu_;
  static uint32_t gpu_slot_mask_;
};

inline std::mutex TimerStorage::mu_;
inline uint32_t TimerStorage::gpu_slot_mask_ = 0;

} // namespace cairns
