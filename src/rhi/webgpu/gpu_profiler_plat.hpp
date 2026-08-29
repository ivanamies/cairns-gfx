// rhi/webgpu/gpu_profiler_plat.hpp
// Empty GpuProfilerPlat so the cross-backend header resolves. GPU timestamp
// queries are not wired on webgpu -- only CPU-side timer slots report here
// (see PERFORMANCE.md).
#pragma once

#include <webgpu/webgpu.h>

namespace cairns::rhi {

struct GpuProfilerPlat {};

}  // namespace cairns::rhi
