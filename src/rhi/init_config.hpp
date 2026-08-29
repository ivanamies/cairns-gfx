// rhi/init_config.hpp
//
// Platform-handle init config for the RHI. The SDL shell (cairns_app) fills
// these in from its SDL window; the headless host (cairns_serve) sets
// `surfaceless = true` and leaves the rest zero. RHI no longer references SDL.
//
// Backend-specific handles live on plat, populated by the shell. Today:
//  vk: instance extensions list + surface-creation callback + window-size
//      callback (used by SwapChain during resize-recovery).
//  metal: pre-resolved CA::MetalLayer*.

#pragma once

#include "util/define.hpp"

#include <cstdint>

#if CAIRNS_METAL
#include "rhi/metal/init_config_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/init_config_plat.hpp"
#endif

namespace cairns::rhi {

struct InitConfig {
    // When true, RHI initializes without any platform surface and
    // SwapChain::Init is skipped. The engine allocates an offscreen
    // final_target_ that the swap pass writes into instead.
    bool surfaceless = false;

    // Initial framebuffer dimensions (used for surfaceless final_target_
    // sizing; ignored when a real surface dictates extent).
    uint32_t width = 1280;
    uint32_t height = 720;

    InitConfigPlat plat;
};

}  // namespace cairns::rhi
