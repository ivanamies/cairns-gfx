#pragma once

#include <cstdint>
#include <limits>

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
    
    uint32_t num_tex = 0;
    uint32_t num_attr = 0;
    uint32_t num_sampler = 0;
    
    // Handle id to GPU residency slot
    // the draw packet producer takes a handle to a eg texture, and it calls Handle<Texture>::get_id() to get an id to look up in the vector.
    // gpu resource id = tex_id[Handle<Texture>::get_id()]
    // IN ALL CASES, THE SCENE'S TEXTURE HANDLE MANAGER SHOULD BE 1-1 WITH THE PSO SCENE REGISTRY
    // std::vector<uint32_t> tex_id;
    std::vector<uint32_t> attr_id;
    std::vector<uint32_t> sampler_id;
};

} // namespace cairns::rhi
