// rhi/vulkan/internal/bindless_impl.hpp
//
// Internal: Bindless::Impl for Vulkan. Shared between vulkan/bindless.cpp and
// vulkan/resource_manager.cpp (the deferred CreateGraphicsPipeline reads
// bindless_layout via friendship). Not a public header.

#pragma once

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "rhi/bindless.hpp"

namespace cairns::rhi {

struct Bindless::Impl {
    VkDevice device = VK_NULL_HANDLE;  // mirrored from Device
    Resources* res = nullptr;          // borrowed

    VkDescriptorSetLayout bindless_layout = VK_NULL_HANDLE;
    VkDescriptorPool bindless_pool = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set = VK_NULL_HANDLE;
    Handle<BindGroup> bindless_handle;
    uint32_t bindless_tex_binding = 0;
    uint32_t bindless_attr_binding = 0;
    uint32_t bindless_samp_binding = 0;
    std::vector<VkDescriptorImageInfo> bindless_tex_infos;
    std::vector<VkDescriptorBufferInfo> bindless_attr_infos;
    std::vector<VkDescriptorImageInfo> bindless_sampler_infos;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
