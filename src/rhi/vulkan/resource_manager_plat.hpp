// rhi/vulkan/resource_manager_plat.hpp
//
// Vulkan side of the resource_manager seam. ApiTextureHandle etc. are
// type-erased to void* on vk (the actual handles ride on VkImage/etc.
// fields wherever needed); per-resource Plat sub-structs hold the
// vk-specific image-layout cache + pipeline/layout/desc-set handles.

#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace cairns::rhi {

using ApiTextureHandle = void*;
using ApiSamplerHandle = void*;
using ApiPsoHandle = void*;
using ApiArgBufferHandle = void*;
using ApiKernelHandle = void*;

struct TextureColdPlat {
    // Current image layout, updated by CommandRecorder::BeginRenderPass when
    // attachments + input_textures transition between passes.
    VkImageLayout vk_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct ShaderHotPlat {
    VkPipeline vk_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout vk_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout vk_imgui_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool vk_imgui_pool = VK_NULL_HANDLE;
    VkDescriptorSet vk_imgui_set = VK_NULL_HANDLE;
};

struct KernelHotPlat {
    VkPipeline vk_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout vk_layout = VK_NULL_HANDLE;
};

struct BackendInitParams {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family_index = 0;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    bool enable_bda = false;
};

}  // namespace cairns::rhi
