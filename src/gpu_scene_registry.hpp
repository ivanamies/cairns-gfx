#pragma once

#include <cstdint>
#include <limits>

#include "util/define.hpp"

namespace cairns::rhi {

// for bindless rendering resources (but not position)
// attr_id is only used for metal
// this is bound into kBindSlot on both Vulkan and Metal
struct GpuSceneRegistry {
    // OpenGL and Vulkan guarantee only 4 slots.
    // However Metal must always dedicate one slot to the argument table binding.
    // Metal always has 4+ slots, so let's put this in the 5th slot.
    static constexpr uint32_t kBindSlot = 5;
    static constexpr uint32_t kMaxTextures = 1024;
    static constexpr uint32_t kMaxMeshes = 1024;
    static constexpr uint32_t kMaxSamplers = 128;
    static constexpr uint32_t kTexturesSlotOffset = 0;
    static constexpr uint32_t kMeshesSlotOffset = kTexturesSlotOffset + kMaxTextures;
    static constexpr uint32_t kSamplersSlotOffset = kMeshesSlotOffset + kMaxMeshes;

    // Where the bindless registry binds textures / attr-buffers / samplers.
    // Metal: id offsets within a single argument buffer. Vulkan: descriptor
    // set-0 binding numbers (the shaders declare set=0 binding=0/1/2).
#if CAIRNS_VULKAN
    static constexpr uint32_t kTextureRegistrySlot = 0;
    static constexpr uint32_t kAttrBufferRegistrySlot = 1;
    static constexpr uint32_t kSamplerRegistrySlot = 2;
#else
    static constexpr uint32_t kTextureRegistrySlot = kTexturesSlotOffset;
    static constexpr uint32_t kAttrBufferRegistrySlot = kMeshesSlotOffset;
    static constexpr uint32_t kSamplerRegistrySlot = kSamplersSlotOffset;
#endif
};

} // namespace cairns::rhi
