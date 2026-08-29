// rhi/webgpu/gpu_profiler.cpp -- WebGPU backend (no query pool; timing in W3+).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/gpu_profiler.hpp"
#include "rhi/device.hpp"

namespace cairns::rhi {

bool GpuProfiler::Init(Device& device) { (void)device; return true; }
void GpuProfiler::Deinit() {}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
