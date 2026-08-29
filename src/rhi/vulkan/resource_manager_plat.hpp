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
    // Cache the last-written (font, sampler) pair so DrawImGui
    // only re-runs vkUpdateDescriptorSets when they actually change.
    // Stable across frames since the font atlas + sampler are immutable
    // post-Engine init -- avoids VUID-vkUpdateDescriptorSets-None-03047.
    uint32_t vk_imgui_last_font_packed = 0xFFFFFFFFu;
    uint32_t vk_imgui_last_sampler_packed = 0xFFFFFFFFu;
};

struct KernelHotPlat {
    VkPipeline vk_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout vk_layout = VK_NULL_HANDLE;
};

// vk-side per-FIF descriptor sets for DynamicBuffers: one layout, one
// set per frame-in-flight. All sets bound against the same backing buffer
// at offset 0 with the binding's max_range; per-draw dynamic offsets shift
// the access window. kMaxFrames=4 (covers FIF=2 and FIF=3 with headroom);
// slots past frames_in_flight stay Null.
struct DynamicBuffersHotPlat {
    VkDescriptorSetLayout vk_layout = VK_NULL_HANDLE;
    static constexpr uint32_t kMaxFrames = 4;
    VkDescriptorSet vk_sets[kMaxFrames] = {VK_NULL_HANDLE, VK_NULL_HANDLE,
                                           VK_NULL_HANDLE, VK_NULL_HANDLE};
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
