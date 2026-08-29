// rhi/vulkan/resources_plat.hpp

#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

#include "rhi/resource_manager.hpp"  // Handle<>, Memory, Buffer

namespace cairns::rhi {

class Resources;
class Allocator;

struct ResourcesPlat {
    VkDevice device_ = VK_NULL_HANDLE;          // mirrored from Device
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    uint32_t frame_index_ = 0;                  // drives deferred-free + bump retire
    VkDescriptorSetLayout material_set_layout_ = VK_NULL_HANDLE;  // set 2 (lazy)
    VkDescriptorPool material_pool_ = VK_NULL_HANDLE;             // per-material sets
    Resources* resources_ = nullptr;            // owner back-pointer (set in Resources::Init)

    // Native-handle resolution. Definitions in rhi/vulkan/resources.cpp.
    VkBuffer GetVkBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t* out_offset);
    VkBuffer GetVkBumpMasterBuffer(Allocator& alloc, Memory mem);
    uint8_t* MappedPtr(Allocator& alloc, Handle<Buffer> h);
    // set 2: shared layout for per-material combined image+sampler bind groups
    // (created lazily on first CreateBindGroup). The unlit pipeline layout
    // references it.
    VkDescriptorSetLayout MaterialSetLayout();
};

}  // namespace cairns::rhi
