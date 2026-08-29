// rhi/frames.hpp
//
// Frame lifecycle: owns per-frame sync + command buffers + the per-frame
// descriptor sets (Vulkan) / render-pass + MSAA/depth targets (Metal).
// Begin() returns a FrameContext whose CommandRecorder records into the frame;
// End() submits + presents. Depends on Device + Resources.

#pragma once

#include "util/define.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "rhi/command_recorder.hpp"  // FrameContext
#include "rhi/swap_resolve_target.hpp"
#if CAIRNS_METAL
#include "rhi/metal/frames_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/frames_plat.hpp"
#elif CAIRNS_WEBGPU
#include "rhi/webgpu/frames_plat.hpp"
#endif

namespace cairns::rhi {

class Device;
class Resources;
class Allocator;
class GpuProfiler;
class FrameCapture;
class OffscreenTargets;
class Pipelines;
struct SwapChain;

class Frames {
public:
    Frames() = default;
    ~Frames();
    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    // CALLER: ENGINE. Stateless wrt the sibling subsystems --
    // GpuProfiler / FrameCapture / OffscreenTargets are passed per-call
    // to Begin/EndSubmit. Pipelines is passed only because Frames needs
    // its descriptor set layouts to alloc per-FIF sets here; not stored.
    [[nodiscard]] bool Init(Device& device, Pipelines& pipelines);
    // CALLER: ENGINE.
    void Deinit();

    // Metal: create MSAA/depth render targets + render-pass descriptor (called
    // after scene textures load). Vulkan: no-op (targets created in SwapChain).
    // CALLER: ENGINE.
    [[nodiscard]] bool InitTargets(Resources& resources, Allocator& alloc,
                                    uint32_t width, uint32_t height);

    // CALLER: ENGINE (per-frame draw loop). Sibling subsystems (profiler,
    // offscreen targets) passed per-call so Frames doesn't stash pointers
    // between Init and Begin.
    FrameContext Begin(Resources& resources, Allocator& alloc,
                       GpuProfiler& gpu_profiler,
                       OffscreenTargets& offscreen_targets,
                       const SwapResolveTarget& target);

    // Two-phase frame end. EndSubmit runs on the render thread: it commits
    // command buffers and submits work to the GPU queue. Present runs on the
    // MAIN thread: it calls the platform present primitive
    // (vkQueuePresentKHR / [CAMetalDrawable present]). Driver vendors don't
    // test render-thread present -- on macOS MoltenVK's vkQueuePresentKHR
    // reaches into CALayer which is main-thread-only and aborts under
    // CA_ASSERT_MAIN_THREAD_TRANSACTIONS. Same contract on every backend so
    // future backends (WebGPU) don't have to relearn it.
    //
    // Metal: presentDrawable is enqueued into a command buffer and is
    // thread-safe per Apple's command-buffer threading rules. EndSubmit does
    // the work; Present is a no-op kept for contract symmetry.
    //
    // Vulkan: vkQueueSubmit is thread-safe; vkQueuePresentKHR on MoltenVK
    // touches CALayer and MUST run on main.
    // frame_capture passed per-call so the dump path doesn't live as
    // engine-side back-pointer state. vk fires the dump inside Present
    // (post vkQueuePresentKHR); metal inside EndSubmit (after the blit +
    // waitUntilCompleted). The unused side ignores it.
    void EndSubmit(const SwapResolveTarget& target,
                    FrameCapture& frame_capture, FrameContext& fc);
    void Present(const SwapResolveTarget& target,
                  FrameCapture& frame_capture, FrameContext& fc);

    // Write the per-frame globals_sets_ + drawtmp_sets_ ONCE
    // at engine init. Both bindings are UNIFORM_BUFFER_DYNAMIC pointing
    // at the master kDynamic buffer with a fixed sizeof(struct) range;
    // the dynamic offset selects the per-pass / per-draw window at bind
    // time. Avoids per-pass vkUpdateDescriptorSets racing pending cmd
    // buffers (VUID-vkUpdateDescriptorSets-None-03047). Metal: no-op.
    void WriteUnlitDescriptors(Resources& resources, Allocator& alloc);

    // Backend state. Resources allocates descriptor sets from
    // plat.descriptor_pool_ (vk).
    FramesPlat plat;

private:
    bool inited_ = false;
};

}  // namespace cairns::rhi
