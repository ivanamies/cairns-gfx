// rhi/frames.hpp
//
// Fork C frame lifecycle: owns per-frame sync + command buffers + the per-frame
// descriptor sets (Vulkan) / render-pass + MSAA/depth targets (Metal) + the
// one-shot swapchain dump. Begin() returns a FrameContext whose CommandRecorder
// records into the frame; End() submits + presents. Depends on Device + Resources.

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
#endif

namespace cairns::rhi {

class Device;
class Resources;
class Allocator;
class GpuProfiler;
struct SwapChain;

class Frames {
public:
    Frames() = default;
    ~Frames();
    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    // CALLER: ENGINE. #222 Phase F.1: profiler owned by Rhi; Frames
    // stashes a pointer so Begin can stamp it onto the recorder + run
    // the per-FIF readback against ts_pool_.
    [[nodiscard]] bool Init(Device& device, GpuProfiler& gpu_profiler);
    // CALLER: ENGINE.
    void Deinit();

    // Metal: create MSAA/depth render targets + render-pass descriptor (called
    // after scene textures load). Vulkan: no-op (targets created in SwapChain).
    // CALLER: ENGINE.
    [[nodiscard]] bool InitTargets(Resources& resources, Allocator& alloc,
                                    uint32_t width, uint32_t height);

    // CALLER: ENGINE (per-frame draw loop).
    FrameContext Begin(Resources& resources, Allocator& alloc,
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
    void EndSubmit(const SwapResolveTarget& target, FrameContext& fc);
    void Present(const SwapResolveTarget& target, FrameContext& fc);

    // Request a one-shot swapchain dump on the next End(). CALLER: ENGINE.
    void SetDumpPath(const std::filesystem::path& path);

    // #222 Phase D.3 cleanup: WriteSkinGroupBDescriptors +
    // WriteAnimEvalDescriptors retired -- skin Group B + anim_eval set 0
    // now flow through dyn_skin_group_b_ / dyn_anim_eval_ DynamicBuffers
    // created in engine.

    // #237 fix: write the per-frame globals_sets_ + drawtmp_sets_ ONCE
    // at engine init. Both bindings are UNIFORM_BUFFER_DYNAMIC pointing
    // at the master kDynamic buffer with a fixed sizeof(struct) range;
    // the dynamic offset selects the per-pass / per-draw window at bind
    // time. Avoids per-pass vkUpdateDescriptorSets racing pending cmd
    // buffers (VUID-vkUpdateDescriptorSets-None-03047). Metal: no-op.
    void WriteUnlitDescriptors(Resources& resources, Allocator& alloc);

    // Called by the engine after a window-resize is applied. Metal no-op
    // (drawable resize is handled implicitly per-frame). Vk wipes the
    // offscreen-target-cache framebuffers (sized at create-time against
    // prior dims; would never re-match the new size).
    void OnSurfaceResize();

    // Backend state. Pipelines reads plat.*_set_layout_ (vk pipeline layouts).
    FramesPlat plat;
    // #222 Phase F.2: FrameCapture nest -- the swap-image dump path. Frames
    // checks capture_.dump_path on EndSubmit and runs the stb_image_write
    // path when set. Grouped so the capture surface (currently one field;
    // future: format, ROI) reads as a unit.
    struct FrameCapture {
        std::filesystem::path dump_path;
    } capture_;

private:
    bool inited_ = false;
};

}  // namespace cairns::rhi
