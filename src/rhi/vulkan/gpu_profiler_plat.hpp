// rhi/vulkan/gpu_profiler_plat.hpp
//
// Backend state for GpuProfiler. vk path owns a VkQueryPool +
// kFramesInFlight per-FIF arrays of pass names / counts. Per-pass
// PassTimerBegin writes the start TS, PassTimerEnd writes the end
// TS; Frames::Begin reads the prior frame's pool and resets.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "rhi/command_recorder.hpp"  // kMaxPasses
#include "util/alloc_tags.hpp"
#include "util/print_allocator.hpp"

namespace cairns::rhi {

struct GpuProfilerPlat {
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool ts_pool_ = VK_NULL_HANDLE;
    float ts_period_ns_ = 0.0f;
    bool host_query_reset_ = false;
    PFN_vkResetQueryPool vk_reset_query_pool_ = nullptr;
    std::vector<std::array<const char*, kMaxPasses>,
                cairns::print_allocator<std::array<const char*, kMaxPasses>,
                                        cairns::tags::VkProfilerPassNames>>
        pass_names_;
    std::vector<uint32_t,
                cairns::print_allocator<uint32_t,
                                        cairns::tags::VkProfilerPassCount>>
        pass_count_;
    std::vector<uint32_t,
                cairns::print_allocator<
                    uint32_t, cairns::tags::VkProfilerComputePassCount>>
        compute_pass_count_;
};

}  // namespace cairns::rhi
