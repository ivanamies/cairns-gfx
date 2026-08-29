// rhi/metal/gpu_profiler_plat.hpp
//
// Metal has no analog to vk's VkQueryPool. Per-pass GPU timing is
// driven by MTLCommandBuffer GPUStartTime/EndTime in the completion
// handler (see CommandRecorder::PassTimerEnd). GpuProfiler keeps an
// empty backend struct on metal to match the vk shape.

#pragma once

namespace cairns::rhi {

struct GpuProfilerPlat {};

}  // namespace cairns::rhi
