// rhi/vulkan/pipelines_plat.hpp
//
// Vulkan side of Pipelines::plat. See rhi/pipelines_plat.hpp.

#pragma once

#include <vulkan/vulkan.h>

namespace cairns::rhi {

struct PipelinesPlat {
    VkDevice device_ = VK_NULL_HANDLE;  // mirrored from Device
    // #222 Phase F.4: descriptor set layouts moved out of Frames.
    // Created at Pipelines::Init; read by CreateGraphicsPipeline +
    // CreateComputePipeline for VkPipelineLayout; read by
    // Frames::Init to alloc per-FIF sets + by Resources::CreateSkinGroupA
    // to alloc per-mesh bind groups.
    VkDescriptorSetLayout globals_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout drawtmp_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout point_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout composite_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout skin_group_a_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout skin_group_b_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout anim_eval_layout_ = VK_NULL_HANDLE;
};

}  // namespace cairns::rhi
