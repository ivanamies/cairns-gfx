#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

// metal-cpp implementation symbols live in gfx_impl.cpp (exactly one TU);
// here we only pull in the type declarations. Defining the *_PRIVATE_
// IMPLEMENTATION macros here produced duplicate symbols once engine.hpp
// started being included from more than one TU (e.g. cairns_control's
// render_ops via engine_headless.hpp).
#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#endif // CAIRNS_METAL

#if CAIRNS_VULKAN
#include <SDL3/SDL_vulkan.h>
// Include Vulkan C++ wrapper on Android
#include <vulkan/vulkan.h>
#endif // CAIRNS_VULKAN


namespace cairns {

enum class GfxApi {
    kHeadless = 0,
    kMetal = 1,
    kVulkan = 2,
    kD3D12 = 3
};

#if CAIRNS_METAL
inline static constexpr GfxApi kGfxApi = GfxApi::kMetal;
#elif CAIRNS_VULKAN
inline static constexpr GfxApi kGfxApi = GfxApi::kVulkan;
#else
inline static constexpr GfxApi kGfxApi = GfxApi::kHeadless;
#endif

constexpr bool is_headless() {
    return kGfxApi == GfxApi::kHeadless;
}

constexpr bool is_vulkan() {
    return kGfxApi == GfxApi::kVulkan;
}

constexpr bool is_metal() {
    return kGfxApi == GfxApi::kMetal;
}

constexpr bool is_d3d12() {
    return kGfxApi == GfxApi::kD3D12;
}

} // namespace cairns
