// rhi/metal/gpu_profiler.cpp
//
// GPU profiler -- metal no-ops. Metal drives per-pass
// timing via MTLCommandBuffer GPUStartTime/EndTime in the completion
// handler installed by CommandRecorder::PassTimerEnd; no shared state
// lives on the profiler.

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi/gpu_profiler.hpp"

namespace cairns::rhi {

bool GpuProfiler::Init(Device& /*device*/) { return true; }
void GpuProfiler::Deinit() {}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
