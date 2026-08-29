#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION

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

inline static constexpr GfxApi kGfxApi = GfxApi::kVulkan;

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
