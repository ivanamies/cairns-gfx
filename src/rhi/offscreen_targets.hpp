// rhi/offscreen_targets.hpp
//
// #222 Phase F.3: render-pass + framebuffer cache extracted out of
// Frames. vk path owns the OffscreenTargetCache (keyed on attachment
// formats / load-store ops); metal path is empty (no equivalent).
// Frames::Begin stamps the cache pointer onto the recorder so the
// per-pass BeginRenderPass can look up / create framebuffers.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL
#include "rhi/metal/offscreen_targets_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/offscreen_targets_plat.hpp"
#elif CAIRNS_WEBGPU
#include "rhi/webgpu/offscreen_targets_plat.hpp"
#endif

namespace cairns::rhi {

class Device;

class OffscreenTargets {
public:
    OffscreenTargets() = default;
    ~OffscreenTargets() = default;
    OffscreenTargets(const OffscreenTargets&) = delete;
    OffscreenTargets& operator=(const OffscreenTargets&) = delete;

    // vk: stash device handle (cache needs it to vkDestroy on Deinit).
    // metal: no-op.
    void Init(Device& device);
    // vk: destroy all cached VkFramebuffer + VkRenderPass. metal: no-op.
    void Deinit();
    // vk: drop only framebuffers (resize -- render passes stay valid
    // since they key on format, not dims). metal: no-op.
    void FlushFramebuffers();

    OffscreenTargetsPlat plat;
};

}  // namespace cairns::rhi
