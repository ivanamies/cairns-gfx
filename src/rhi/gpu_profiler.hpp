// rhi/gpu_profiler.hpp
//
// #222 Phase F.1: GPU-side per-pass timing. Extracted out of Frames
// so FramesPlat is no longer a god struct.
//
// vk path: VkQueryPool + per-FIF arrays of pass names/counts. The
// recorder reaches into plat.profiler_ to write start/end timestamps;
// Frames::Begin asks GpuProfiler to read back the prior frame's pool
// and reset it (host-side or via vkCmdResetQueryPool).
//
// metal path: no analog. MTLCommandBuffer GPUStartTime/EndTime drives
// timing via the completion handler set in
// CommandRecorder::PassTimerEnd. GpuProfiler is empty on metal.

#pragma once

#include <cstdint>

#include "util/define.hpp"

#if CAIRNS_METAL
#include "rhi/metal/gpu_profiler_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/gpu_profiler_plat.hpp"
#elif CAIRNS_WEBGPU
#include "rhi/webgpu/gpu_profiler_plat.hpp"
#endif

namespace cairns::rhi {

class Device;

class GpuProfiler {
public:
    GpuProfiler() = default;
    ~GpuProfiler() = default;
    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    // vk: create VkQueryPool + per-FIF tracker vectors.
    // metal: no-op.
    [[nodiscard]] bool Init(Device& device);

    // vk: destroy VkQueryPool. metal: no-op.
    void Deinit();

    GpuProfilerPlat plat;
};

}  // namespace cairns::rhi
