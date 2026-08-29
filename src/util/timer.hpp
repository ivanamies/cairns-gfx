#pragma once

#include <cstdint>
#include <limits>

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
    static std::array<uint64_t, kMaxSlots> accum_times_;
    static std::array<uint64_t, kMaxSlots> accum_itrs_;
    
  explicit Timer(const std::string& task_name, uint32_t slot)
      : task_name_(task_name),
        slot_(slot),
        is_running_(true),
        start_time_(timestamp_ns()) {
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
      
      accum_times_[slot_] += elapsed_us;
      accum_itrs_[slot_]++;

    is_running_ = false;
  }
    
    static void PrintReport(bool average) {
        printf("==============\n");
        for ( uint32_t i = 0; i < kMaxSlots; ++i ) {
            if ( average ) {
                printf("slot %d average: %lld (us)\n",i,accum_times_[i]/accum_itrs_[i]);
            }
            else { // accum
                printf("slot %d accum: %lld (us)\n",i,accum_times_[i]);
            }
        }
    }

  // Prevent copying to ensure one timer per scope/task
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;

  // Allow moving if ownership needs to be transferred
  Timer(Timer&& other) noexcept
      : task_name_(std::move(other.task_name_)),
        is_running_(other.is_running_),
        start_time_(other.start_time_) {
    other.is_running_ = false;
  }

 private:
  std::string_view task_name_;
    uint32_t slot_;
  bool is_running_;
  uint64_t start_time_;
};

// todo @iamies move this out
std::array<uint64_t, Timer::kMaxSlots> Timer::accum_times_ = {};
std::array<uint64_t, Timer::kMaxSlots> Timer::accum_itrs_ = {};

} // namespace cairns
