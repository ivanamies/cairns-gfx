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
#include "rhi/offscreen_targets.hpp"
#include "rhi/frames.hpp"
#include "rhi/pipelines.hpp"

namespace cairns::rhi {

struct Rhi {
    Device device;
    Allocator alloc;
    Resources resources;
    // GPU per-pass timing. Must init after device and before frames:
    // Frames::Begin stamps the query pool onto the recorder.
    GpuProfiler gpu_profiler;
    // One-shot swap-image dump request. Engine writes it; Frames reads
    // it via per-call param.
    FrameCapture frame_capture;
    // Offscreen render-pass + framebuffer cache (vk owns it; other
    // backends stub). Frames::Begin stamps it onto the recorder.
    OffscreenTargets offscreen_targets;
    Frames frames;
    Pipelines pipelines;
};

}  // namespace cairns::rhi
