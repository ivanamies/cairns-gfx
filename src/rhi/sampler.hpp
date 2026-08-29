#pragma once

#include "rhi/gfx_api.hpp"

#include <cstdint>

namespace cairns::rhi {

enum class SamplerFilter : uint8_t {
    Nearest = 0,
    Linear = 1
};

enum class SamplerMipFilter : uint8_t {
    None = 0,    // No mipmapping
    Nearest = 1, // Picking the nearest level
    Linear = 2   // Blending between levels (Trilinear)
};

enum class SamplerAddressMode : uint8_t {
    Repeat = 0,
    MirroredRepeat = 1,
    ClampToEdge = 2,
    ClampToBorder = 3
};

namespace metal {
inline MTL::SamplerMinMagFilter MapMinMag(SamplerFilter filter) {
    return (filter == SamplerFilter::Nearest) ?
    MTL::SamplerMinMagFilterNearest :
    MTL::SamplerMinMagFilterLinear;
}

inline MTL::SamplerAddressMode MapAddressMode(SamplerAddressMode mode) {
    switch (mode) {
        case SamplerAddressMode::Repeat:         return MTL::SamplerAddressModeRepeat;
        case SamplerAddressMode::MirroredRepeat: return MTL::SamplerAddressModeMirrorRepeat;
        case SamplerAddressMode::ClampToEdge:    return MTL::SamplerAddressModeClampToEdge;
        case SamplerAddressMode::ClampToBorder:  return MTL::SamplerAddressModeClampToZero;
        default:                                 return MTL::SamplerAddressModeRepeat;
    }
}
} // namespace metal

namespace vulkan {
inline VkFilter MapFilter(SamplerFilter filter) {
    return (filter == SamplerFilter::Nearest) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

// Helper to map your AddressMode to VkSamplerAddressMode
inline VkSamplerAddressMode MapAddressMode(SamplerAddressMode mode) {
    switch (mode) {
        case SamplerAddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case SamplerAddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case SamplerAddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case SamplerAddressMode::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:                                 return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// Helper to map your MipFilter to VkSamplerMipmapMode
inline VkSamplerMipmapMode MapMipmapMode(SamplerMipFilter mode) {
    // Note: 'None' is handled by setting maxLod to 0 in the main function.
    // Vulkan only supports Nearest or Linear for the mode itself.
    if (mode == SamplerMipFilter::Nearest) return VK_SAMPLER_MIPMAP_MODE_NEAREST;
    return VK_SAMPLER_MIPMAP_MODE_LINEAR;
}
} // namespace vulkan


struct SamplerInfo {
    SamplerFilter minFilter = SamplerFilter::Linear;
    SamplerFilter magFilter = SamplerFilter::Linear;
    SamplerMipFilter mipFilter = SamplerMipFilter::None;
    SamplerAddressMode addressModeU = SamplerAddressMode::Repeat;
    SamplerAddressMode addressModeV = SamplerAddressMode::Repeat;
};

} // namespace cairns::rhi
