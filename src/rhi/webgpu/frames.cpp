// rhi/webgpu/frames.cpp -- WebGPU backend (W2 stubs; real frame loop in W3/W4).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/frames.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/allocator.hpp"
#include "rhi/pipelines.hpp"
#include "rhi/gpu_profiler.hpp"
#include "rhi/offscreen_targets.hpp"
#include "rhi/frame_capture.hpp"

namespace cairns::rhi {

Frames::~Frames() {}

bool Frames::Init(Device& device, Pipelines& pipelines) {
    plat.device_ = device.plat.device;
    plat.queue_ = device.plat.queue;
    (void)pipelines;
    inited_ = true;
    return true;
}
void Frames::Deinit() { inited_ = false; }

bool Frames::InitTargets(Resources& resources, Allocator& alloc, uint32_t width, uint32_t height) {
    (void)resources; (void)alloc; (void)width; (void)height;
    return true;
}

FrameContext Frames::Begin(Resources& resources, Allocator& alloc, GpuProfiler& gpu_profiler,
                           OffscreenTargets& offscreen_targets, const SwapResolveTarget& target) {
    (void)resources; (void)alloc; (void)gpu_profiler; (void)offscreen_targets; (void)target;
    FrameContext fc{};
    fc.cmd.plat.device_ = plat.device_;
    fc.cmd.plat.queue_ = plat.queue_;
    return fc;
}

void Frames::EndSubmit(const SwapResolveTarget& target, FrameCapture& frame_capture, FrameContext& fc) {
    (void)target; (void)frame_capture; (void)fc;
}
void Frames::Present(const SwapResolveTarget& target, FrameCapture& frame_capture, FrameContext& fc) {
    (void)target; (void)frame_capture; (void)fc;
}
void Frames::WriteUnlitDescriptors(Resources& resources, Allocator& alloc) {
    (void)resources; (void)alloc;
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
