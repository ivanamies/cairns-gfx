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
#include "rhi/webgpu/memory_allocator.hpp"

#include <cstdio>

#include <webgpu/webgpu.h>

#include "rhi/webgpu/native_compat.hpp"  // DrainGpu (wgpuDevicePoll wrapper)

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
    (void)gpu_profiler; (void)offscreen_targets; (void)target;
    plat.bump_alloc_ = &alloc.plat.memory_;
    resources.AdvanceFrame(alloc);
    resources.DrainDeferredFrees(alloc, resources.FrameIndex());
    FrameContext fc{};
    fc.cmd.plat.device_ = plat.device_;
    fc.cmd.plat.queue_ = plat.queue_;
    fc.cmd.plat.cmd_ = wgpuDeviceCreateCommandEncoder(plat.device_, nullptr);
    return fc;
}

void Frames::EndSubmit(const SwapResolveTarget& target, FrameCapture& frame_capture, FrameContext& fc) {
    (void)target; (void)frame_capture;
    CommandRecorder& ri = fc.cmd;
    if (ri.plat.enc_) { wgpuRenderPassEncoderEnd(ri.plat.enc_); ri.plat.enc_ = nullptr; }
    // Upload the frame's dynamic UBO/staging writes to the GPU before the draws
    // that read them execute (queueWriteBuffer is ordered before queueSubmit).
    if (plat.bump_alloc_) { plat.bump_alloc_->FlushBumpRing(plat.queue_); }
    if (ri.plat.cmd_) {
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(ri.plat.cmd_, nullptr);
        wgpuQueueSubmit(plat.queue_, 1, &cmd);
        wgpuCommandBufferRelease(cmd);
        wgpuCommandEncoderRelease(ri.plat.cmd_);
        ri.plat.cmd_ = nullptr;
    }
    webgpu::DrainGpu(plat.device_);
    // GPU is idle now (desktop) -- safe to drop the per-draw bind groups the
    // recorder held. Browser: bind groups stay alive in the impl until the
    // submitted command buffer executes, so releasing the handles now is safe.
    for (WGPUBindGroup bg : ri.plat.transient_bind_groups_) {
        wgpuBindGroupRelease(bg);
    }
    ri.plat.transient_bind_groups_.clear();
}
void Frames::Present(const SwapResolveTarget& target, FrameCapture& frame_capture, FrameContext& fc) {
    (void)target; (void)frame_capture; (void)fc;
}
void Frames::WriteUnlitDescriptors(Resources& resources, Allocator& alloc) {
    (void)resources; (void)alloc;
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
