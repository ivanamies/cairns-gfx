// rhi/rhi.hpp
//
// The single owner of every RHI subsystem. The engine holds one Rhi by value.
// Field order = construction order = reverse teardown order. No methods: the
// subsystems keep their own; nothing stores a pointer to a sibling.

#pragma once

#include "rhi/device.hpp"
#include "rhi/allocator.hpp"
#include "rhi/resources.hpp"
#include "rhi/gpu_profiler.hpp"
#include "rhi/frame_capture.hpp"
#include "rhi/frames.hpp"
#include "rhi/pipelines.hpp"

namespace cairns::rhi {

struct Rhi {
    Device device;
    Allocator alloc;
    Resources resources;
    // #222 Phase F.1: GPU timing extracted out of Frames. Init runs
    // after device + before frames (frames stamps the query pool onto
    // CommandRecorderPlat::profiler_ during Begin).
    GpuProfiler gpu_profiler;
    // #222 Phase F.2: one-shot swap-image dump request. Frames::EndSubmit
    // reads dump_path; engine writes via Rhi::frame_capture.SetDumpPath.
    FrameCapture frame_capture;
    Frames frames;
    Pipelines pipelines;
};

}  // namespace cairns::rhi
