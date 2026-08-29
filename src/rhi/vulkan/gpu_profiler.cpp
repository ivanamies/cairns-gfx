// rhi/vulkan/gpu_profiler.cpp
//
// GPU profiler -- vk Init/Deinit. Per-FIF readback + reset lives in
// Frames::Begin (calls into ts_pool_ directly).

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include "rhi/gpu_profiler.hpp"

#include "rhi/device.hpp"
#include "rhi/command_recorder.hpp"  // kMaxPasses + kFramesInFlight

namespace cairns::rhi {

bool GpuProfiler::Init(Device& device) {
    plat.device_ = device.plat.device_;
    plat.ts_period_ns_ = device.plat.timestamp_period_ns_;
    plat.host_query_reset_ = device.plat.host_query_reset_;
    plat.vk_reset_query_pool_ = device.plat.vk_reset_query_pool_;
    VkQueryPoolCreateInfo qpi{};
    qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = 2 * kMaxPasses * kFramesInFlight;
    if (vkCreateQueryPool(plat.device_, &qpi, nullptr, &plat.ts_pool_) !=
        VK_SUCCESS) {
        return false;
    }
    plat.pass_names_.assign(kFramesInFlight, {});
    plat.pass_count_.assign(kFramesInFlight, 0);
    plat.compute_pass_count_.assign(kFramesInFlight, 0);
    return true;
}

void GpuProfiler::Deinit() {
    if (plat.ts_pool_) {
        vkDestroyQueryPool(plat.device_, plat.ts_pool_, nullptr);
        plat.ts_pool_ = VK_NULL_HANDLE;
    }
    plat.pass_names_.clear();
    plat.pass_count_.clear();
    plat.compute_pass_count_.clear();
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
