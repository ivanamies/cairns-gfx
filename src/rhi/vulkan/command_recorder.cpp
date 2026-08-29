// rhi/vulkan/command_recorder.cpp
//
// Vulkan backend bodies for CommandRecorder. Moved out of resource_manager.cpp
// EndFrame in resource_manager.cpp can construct/destroy it.

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <array>
#include <vector>

#include <vulkan/vulkan.h>

#include "rhi/command_recorder.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/resources.hpp"
#include "rhi/swap_chain.hpp"
#include "util/draw.hpp"
#include "util/material_gpu.hpp"
#include "util/render_pass_globals.hpp"

namespace cairns::rhi {

void CommandRecorder::Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d) {
    Kernel::Hot* k = res.GetHot(d.kernel);
    VkDescriptorSet set = compute_set_;

    const size_t n = d.buffers.size();
    std::vector<VkDescriptorBufferInfo> infos(n);
    std::vector<VkWriteDescriptorSet> writes(n);
    for (size_t i = 0; i < n; ++i) {
        const BoundBuffer& b = d.buffers[i];
        uint32_t off = 0;
        VkBuffer buf = res.GetVkBuffer(alloc,b.buffer, &off);
        const bool is_ubo = (b.slot == 0);
        infos[i].buffer = buf;
        infos[i].offset = off + b.offset;
        infos[i].range = is_ubo ? static_cast<VkDeviceSize>(sizeof(float)) : VK_WHOLE_SIZE;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = b.slot;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = is_ubo ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &infos[i];
    }
    if (n > 0) {
        vkUpdateDescriptorSets(device_, static_cast<uint32_t>(n), writes.data(), 0,
                               nullptr);
    }

    vkCmdBindPipeline(comp_, VK_PIPELINE_BIND_POINT_COMPUTE, k->vk_pipeline);
    vkCmdBindDescriptorSets(comp_, VK_PIPELINE_BIND_POINT_COMPUTE, k->vk_layout,
                            0, 1, &set, 0, nullptr);
    vkCmdDispatch(comp_, d.groups_x, d.groups_y, d.groups_z);
}

void CommandRecorder::BeginRenderPass(SwapChain& sc, const RenderPassDesc& desc) {
    VkRenderPassBeginInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = sc.renderPass;
    rpi.framebuffer = sc.swapChainFramebuffers[image_index_];
    rpi.renderArea.offset = {0, 0};
    rpi.renderArea.extent = sc.swapChainExtent;
    VkClearValue clears[2]{};
    if (!desc.color.empty()) {
        clears[0].color = {{desc.color[0].clear[0], desc.color[0].clear[1],
                            desc.color[0].clear[2], desc.color[0].clear[3]}};
    }
    clears[1].depthStencil = {desc.depth.clear_depth, 0};
    rpi.clearValueCount = 2;
    rpi.pClearValues = clears;
    vkCmdBeginRenderPass(gfx_, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    // Negative-height viewport flips NDC Y so the shared (Metal-convention)
    // projection renders upright on Vulkan, instead of an in-shader proj[1][1]*=-1.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(sc.swapChainExtent.height);
    viewport.width = static_cast<float>(sc.swapChainExtent.width);
    viewport.height = -static_cast<float>(sc.swapChainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(gfx_, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = sc.swapChainExtent;
    vkCmdSetScissor(gfx_, 0, 1, &scissor);
}

void CommandRecorder::DrawMeshes(Resources& res, Allocator& alloc, const MeshDrawList& list) {
    VkCommandBuffer cb = gfx_;
    VkDescriptorSet dyn_set = dyn_ubo_set_;

    VkBuffer bump_buf = res.GetVkBumpMasterBuffer(alloc, Memory::kDynamic);
    std::array<VkWriteDescriptorSet, 3> writes{};
    std::array<VkDescriptorBufferInfo, 3> buf_infos{};
    const uint32_t ranges[3] = {static_cast<uint32_t>(sizeof(RenderPassGlobals)),
                                static_cast<uint32_t>(sizeof(MaterialGpu)),
                                static_cast<uint32_t>(sizeof(DrawTmp))};
    for (uint32_t i = 0; i < 3; ++i) {
        buf_infos[i].buffer = bump_buf;
        buf_infos[i].offset = 0;
        buf_infos[i].range = ranges[i];
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = dyn_set;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &buf_infos[i];
    }
    vkUpdateDescriptorSets(device_, 3, writes.data(), 0, nullptr);

    Shader::Hot* unlit = res.GetHot(list.pipeline);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->vk_pipeline);

    // Pack-meshes (Aaltonen slide 26): bind each stream at its mesh-region base
    // and select the primitive via baseVertex/baseIndex in the draw call, so the
    // VB/IB binds are emitted only when the mesh buffer actually changes.
    uint32_t last_mat_bg = 0xFFFFFFFFu;
    VkBuffer last_pos_buf = VK_NULL_HANDLE;
    uint32_t last_pos_off = 0xFFFFFFFFu;
    VkBuffer last_attr_buf = VK_NULL_HANDLE;
    uint32_t last_attr_off = 0xFFFFFFFFu;
    VkBuffer last_idx_buf = VK_NULL_HANDLE;
    uint32_t last_idx_off = 0xFFFFFFFFu;
    for (size_t i = 0; i < list.sorted_draws.size(); ++i) {
        const cairns::Draw& draw = list.draws[list.sorted_draws[i].second];
        // set 2: per-material bind group, bound only when the material changes
        // (DrawKey sorts by material, so equal-material draws are adjacent).
        const uint32_t mat_bg = draw.bind_groups[1].index;
        if (mat_bg != last_mat_bg) {
            last_mat_bg = mat_bg;
            VkDescriptorSet ms = static_cast<VkDescriptorSet>(
                res.GetHot(draw.bind_groups[1])->api_descriptor_set);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    unlit->vk_layout, 1, 1, &ms, 0, nullptr);
        }
        uint32_t pos_off = 0;
        VkBuffer pos_buf =
            res.GetVkBuffer(alloc,draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot], &pos_off);
        if (pos_buf != last_pos_buf || pos_off != last_pos_off) {
            last_pos_buf = pos_buf;
            last_pos_off = pos_off;
            VkDeviceSize off = pos_off;
            vkCmdBindVertexBuffers(cb, 0, 1, &pos_buf, &off);
        }
        uint32_t attr_off = 0;
        VkBuffer attr_buf =
            res.GetVkBuffer(alloc,draw.vertex_buffers[cairns::Draw::kVertexBufferAttrSlot], &attr_off);
        if (attr_buf != last_attr_buf || attr_off != last_attr_off) {
            last_attr_buf = attr_buf;
            last_attr_off = attr_off;
            VkDeviceSize off = attr_off;
            vkCmdBindVertexBuffers(cb, cairns::kMeshAttrVertexBindSlot, 1, &attr_buf, &off);
        }
        uint32_t idx_base = 0;
        VkBuffer idx_buf = res.GetVkBuffer(alloc,draw.index_buffer, &idx_base);
        if (idx_buf != last_idx_buf || idx_base != last_idx_off) {
            last_idx_buf = idx_buf;
            last_idx_off = idx_base;
            vkCmdBindIndexBuffer(cb, idx_buf, idx_base, VK_INDEX_TYPE_UINT32);
        }
        const uint32_t first_index = (draw.index_offset - idx_base) / sizeof(uint32_t);
        std::array<uint32_t, 3> dyn_offsets = {list.globals_offset,
                                               draw.dynamic_buffer_offsets[0],
                                               draw.dynamic_buffer_offsets[1]};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->vk_layout, 0, 1,
                                &dyn_set, 3, dyn_offsets.data());
        vkCmdDrawIndexed(cb, draw.triangle_count * 3, draw.instance_count, first_index,
                         static_cast<int32_t>(draw.vertex_offset), draw.instance_offset);
    }
}

void CommandRecorder::DrawPoints(Resources& res, Allocator& alloc, const PointDraw& pd) {
    VkCommandBuffer cb = gfx_;
    Shader::Hot* p = res.GetHot(pd.pipeline);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->vk_pipeline);
    uint32_t ssbo_off = 0;
    VkBuffer ssbo = res.GetVkBuffer(alloc,pd.vertex_buffer, &ssbo_off);
    VkDeviceSize off = ssbo_off;
    vkCmdBindVertexBuffers(cb, 0, 1, &ssbo, &off);
    VkDescriptorSet point_set = point_set_;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->vk_layout, 0, 1,
                            &point_set, 0, nullptr);
    vkCmdDraw(cb, pd.vertex_count, 1, 0, 0);
}

void CommandRecorder::EndRenderPass() {
    vkCmdEndRenderPass(gfx_);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
