// rhi/vulkan/swap_resolve_target_plat.hpp

#pragma once

namespace cairns::rhi {

struct SwapChain;

struct SwapResolveTargetPlat {
    // Today the vk path is window-bound: the resolve target is the
    // SwapChain's framebuffer at swapchain_image_index. swap_chain == nullptr
    // is reserved for the upcoming render-to-texture vk path; Frames asserts.
    SwapChain* swap_chain = nullptr;
};

}  // namespace cairns::rhi
