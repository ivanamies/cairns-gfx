// rhi/vulkan/command_recorder.cpp
//
// Vulkan backend bodies for CommandRecorder. Moved out of resource_manager.cpp
// EndFrame in resource_manager.cpp can construct/destroy it.

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <array>
#include <cstring>
#include <vector>

#include <vulkan/vulkan.h>

#include "imgui.h"
#include "rhi/allocator.hpp"
#include "rhi/command_recorder.hpp"
#include "rhi/frames.hpp"
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

namespace {

VkFormat to_vk_format(Format f) {
    switch (f) {
        case Format::kR8Unorm: return VK_FORMAT_R8_UNORM;
        case Format::kRg8Unorm: return VK_FORMAT_R8G8_UNORM;
        case Format::kRgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::kRgba8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::kBgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::kBgra8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::kR16F: return VK_FORMAT_R16_SFLOAT;
        case Format::kRgba16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::kR32F: return VK_FORMAT_R32_SFLOAT;
        case Format::kRg32F: return VK_FORMAT_R32G32_SFLOAT;
        case Format::kRgba32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::kD32F: return VK_FORMAT_D32_SFLOAT;
        case Format::kD24S8: return VK_FORMAT_D24_UNORM_S8_UINT;
        default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

bool is_depth_format(Format f) {
    return f == Format::kD32F || f == Format::kD24S8;
}

VkAttachmentLoadOp to_vk_load(LoadOp op) {
    switch (op) {
        case LoadOp::kClear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::kLoad: return VK_ATTACHMENT_LOAD_OP_LOAD;
        default: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
}

// Broad barrier: track the image's layout in its Cold record + transition. The
// graph guarantees ordering; we over-synchronize (ALL_COMMANDS) for simplicity.
void transition(VkCommandBuffer cb, Resources& res, Handle<Texture> h,
                VkImageLayout new_layout) {
    if (h.IsNull()) {
        return;
    }
    Texture::Cold* c = res.textures.GetCold(h);
    if (!c || !c->api_image || c->vk_layout == new_layout) {
        return;
    }
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = c->vk_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = reinterpret_cast<VkImage>(c->api_image);
    b.subresourceRange.aspectMask =
        is_depth_format(c->format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    c->vk_layout = new_layout;
}

VkRenderPass get_offscreen_rp(OffscreenTargetCache* cache,
                              const OffscreenTargetCache::RpKey& key) {
    for (const OffscreenTargetCache::RpEntry& e : cache->rps) {
        if (e.key.color == key.color && e.key.depth == key.depth &&
            e.key.color_load == key.color_load && e.key.depth_load == key.depth_load &&
            e.key.has_color == key.has_color && e.key.has_depth == key.has_depth) {
            return e.rp;
        }
    }
    VkAttachmentDescription atts[2]{};
    VkAttachmentReference color_ref{};
    VkAttachmentReference depth_ref{};
    uint32_t n = 0;
    int color_idx = -1;
    int depth_idx = -1;
    if (key.has_color) {
        color_idx = static_cast<int>(n);
        atts[n].format = key.color;
        atts[n].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[n].loadOp = key.color_load;
        atts[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ++n;
    }
    if (key.has_depth) {
        depth_idx = static_cast<int>(n);
        atts[n].format = key.depth;
        atts[n].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[n].loadOp = key.depth_load;
        atts[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ++n;
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (color_idx >= 0) {
        color_ref.attachment = static_cast<uint32_t>(color_idx);
        color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &color_ref;
    }
    if (depth_idx >= 0) {
        depth_ref.attachment = static_cast<uint32_t>(depth_idx);
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sub.pDepthStencilAttachment = &depth_ref;
    }
    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = n;
    ci.pAttachments = atts;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    vkCreateRenderPass(cache->device, &ci, nullptr, &rp);
    cache->rps.push_back({key, rp});
    return rp;
}

VkFramebuffer get_offscreen_fb(OffscreenTargetCache* cache, VkRenderPass rp,
                               VkImageView v0, VkImageView v1, uint32_t w, uint32_t h) {
    for (const OffscreenTargetCache::FbEntry& e : cache->fbs) {
        if (e.rp == rp && e.v0 == v0 && e.v1 == v1 && e.w == w && e.h == h) {
            return e.fb;
        }
    }
    VkImageView views[2]{};
    uint32_t n = 0;
    if (v0) {
        views[n++] = v0;
    }
    if (v1) {
        views[n++] = v1;
    }
    VkFramebufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass = rp;
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    vkCreateFramebuffer(cache->device, &ci, nullptr, &fb);
    cache->fbs.push_back({rp, v0, v1, w, h, fb});
    return fb;
}

}  // namespace

void OffscreenTargetCache::Deinit() {
    for (FbEntry& e : fbs) {
        if (e.fb) {
            vkDestroyFramebuffer(device, e.fb, nullptr);
        }
    }
    for (RpEntry& e : rps) {
        if (e.rp) {
            vkDestroyRenderPass(device, e.rp, nullptr);
        }
    }
    fbs.clear();
    rps.clear();
}

void CommandRecorder::BeginRenderPass(Resources& res, SwapChain& sc,
                                      const RenderPassDesc& desc) {
    // Sampled inputs (textures produced by a prior pass) -> shader-read, for
    // BOTH swapchain and offscreen passes (e.g. the composite samples offscreen
    // color+depth while rendering to the swapchain).
    for (const Handle<Texture>& in : desc.input_textures) {
        transition(gfx_, res, in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    const bool is_swapchain = !desc.color.empty() && desc.color[0].target.IsNull();
    VkExtent2D extent{desc.width, desc.height};

    if (is_swapchain) {
        if (frames_ && !frames_->IsSwapchainAcquired()) {
            frames_->AcquireSwapchain(*res_, *alloc_, sc, *this);
        }
        VkRenderPassBeginInfo rpi{};
        rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpi.renderPass = sc.renderPass;
        rpi.framebuffer = sc.swapChainFramebuffers[image_index_];
        rpi.renderArea.offset = {0, 0};
        rpi.renderArea.extent = sc.swapChainExtent;
        extent = sc.swapChainExtent;
        VkClearValue clears[2]{};
        if (!desc.color.empty()) {
            clears[0].color = {{desc.color[0].clear[0], desc.color[0].clear[1],
                                desc.color[0].clear[2], desc.color[0].clear[3]}};
        }
        clears[1].depthStencil = {desc.depth.clear_depth, 0};
        rpi.clearValueCount = 2;
        rpi.pClearValues = clears;
        vkCmdBeginRenderPass(gfx_, &rpi, VK_SUBPASS_CONTENTS_INLINE);
    } else {
        const bool has_color = !desc.color.empty();
        const bool has_depth = !desc.depth.depth.IsNull();
        VkImageView color_view = VK_NULL_HANDLE;
        VkImageView depth_view = VK_NULL_HANDLE;
        OffscreenTargetCache::RpKey key;
        key.has_color = has_color;
        key.has_depth = has_depth;
        if (has_color) {
            transition(gfx_, res, desc.color[0].target,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            Texture::Cold* c = res.textures.GetCold(desc.color[0].target);
            color_view = reinterpret_cast<VkImageView>(res.GetHot(desc.color[0].target)->api_view);
            key.color = to_vk_format(c->format);
            key.color_load = to_vk_load(desc.color[0].load);
        }
        if (has_depth) {
            transition(gfx_, res, desc.depth.depth,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            Texture::Cold* c = res.textures.GetCold(desc.depth.depth);
            depth_view = reinterpret_cast<VkImageView>(res.GetHot(desc.depth.depth)->api_view);
            key.depth = to_vk_format(c->format);
            key.depth_load = to_vk_load(desc.depth.load);
        }
        VkRenderPass rp = get_offscreen_rp(offscreen_, key);
        // attachment order matches the render pass: color (if any) then depth.
        const VkImageView v0 = has_color ? color_view : depth_view;
        const VkImageView v1 = has_color ? depth_view : VK_NULL_HANDLE;
        VkFramebuffer fb = get_offscreen_fb(offscreen_, rp, v0, v1, extent.width, extent.height);

        VkClearValue clears[2]{};
        uint32_t clear_n = 0;
        if (has_color) {
            clears[clear_n++].color = {{desc.color[0].clear[0], desc.color[0].clear[1],
                                        desc.color[0].clear[2], desc.color[0].clear[3]}};
        }
        if (has_depth) {
            clears[clear_n++].depthStencil = {desc.depth.clear_depth, 0};
        }
        VkRenderPassBeginInfo rpi{};
        rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpi.renderPass = rp;
        rpi.framebuffer = fb;
        rpi.renderArea.offset = {0, 0};
        rpi.renderArea.extent = extent;
        rpi.clearValueCount = clear_n;
        rpi.pClearValues = clears;
        vkCmdBeginRenderPass(gfx_, &rpi, VK_SUBPASS_CONTENTS_INLINE);
    }

    // Negative-height viewport flips NDC Y so the shared (Metal-convention)
    // projection renders upright on Vulkan, instead of an in-shader proj[1][1]*=-1.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(extent.height);
    viewport.width = static_cast<float>(extent.width);
    viewport.height = -static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(gfx_, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(gfx_, 0, 1, &scissor);
}

void CommandRecorder::DrawMeshes(Resources& res, Allocator& alloc, const MeshDrawList& list) {
    VkCommandBuffer cb = gfx_;

    // Aaltonen frequency split: globals = set 0 (one dynamic UBO, bound once per
    // frame), drawtmp = set 2 (one dynamic UBO, one offset per draw). Each is its own
    // descriptor set so the per-draw bind carries a single dynamic offset.
    VkBuffer bump_buf = res.GetVkBumpMasterBuffer(alloc, Memory::kDynamic);
    std::array<VkWriteDescriptorSet, 2> writes{};
    std::array<VkDescriptorBufferInfo, 2> buf_infos{};
    const VkDescriptorSet sets[2] = {globals_set_, drawtmp_set_};
    const uint32_t ranges[2] = {static_cast<uint32_t>(sizeof(RenderPassGlobals)),
                                static_cast<uint32_t>(sizeof(DrawTmp))};
    for (uint32_t i = 0; i < 2; ++i) {
        buf_infos[i].buffer = bump_buf;
        buf_infos[i].offset = 0;
        buf_infos[i].range = ranges[i];
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = sets[i];
        writes[i].dstBinding = 0;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].descriptorCount = 1;
        writes[i].pBufferInfo = &buf_infos[i];
    }
    vkUpdateDescriptorSets(device_, 2, writes.data(), 0, nullptr);

    Shader::Hot* unlit = res.GetHot(list.pipeline);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->vk_pipeline);

    // set 0 globals: bind once for the whole pass.
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->vk_layout, 0, 1,
                            &globals_set_, 1, &list.globals_offset);

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
        // set 2 drawtmp: the only per-draw dynamic offset.
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->vk_layout, 2, 1,
                                &drawtmp_set_, 1, &draw.dynamic_buffer_offsets[1]);
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

void CommandRecorder::DrawFullscreen(Resources& res, Handle<Shader> pipeline,
                                     const Handle<Texture>* textures, uint32_t tex_count,
                                     Handle<Sampler> sampler) {
    Shader::Hot* sh = res.GetHot(pipeline);
    VkSampler samp = reinterpret_cast<VkSampler>(res.GetHot(sampler)->api_sampler);
    // Take the next set from the per-frame ring so multiple fullscreen passes in
    // one frame don't clobber each other's bindings.
    VkDescriptorSet set = composite_set_ring_[composite_set_cursor_];
    composite_set_cursor_ = (composite_set_cursor_ + 1) % kCompositeRing;
    std::vector<VkDescriptorImageInfo> infos(tex_count);
    std::vector<VkWriteDescriptorSet> writes(tex_count);
    for (uint32_t i = 0; i < tex_count; ++i) {
        infos[i].sampler = samp;
        infos[i].imageView =
            reinterpret_cast<VkImageView>(res.GetHot(textures[i])->api_view);
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, tex_count, writes.data(), 0, nullptr);
    vkCmdBindPipeline(gfx_, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->vk_pipeline);
    vkCmdBindDescriptorSets(gfx_, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->vk_layout, 0, 1,
                            &set, 0, nullptr);
    vkCmdDraw(gfx_, 3, 1, 0, 0);
}

void CommandRecorder::DrawImGui(Resources& res, Allocator& alloc, Handle<Shader> pipeline,
                                Handle<Texture> font, Handle<Sampler> sampler,
                                const ImDrawData* dd) {
    if (!dd || dd->CmdListsCount == 0 || dd->DisplaySize.x <= 0.0f) {
        return;
    }
    Shader::Hot* sh = res.GetHot(pipeline);

    VkDescriptorImageInfo ii{};
    ii.sampler = reinterpret_cast<VkSampler>(res.GetHot(sampler)->api_sampler);
    ii.imageView = reinterpret_cast<VkImageView>(res.GetHot(font)->api_view);
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = imgui_set_;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

    vkCmdBindPipeline(gfx_, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->vk_pipeline);
    vkCmdBindDescriptorSets(gfx_, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->vk_layout, 0, 1,
                            &imgui_set_, 0, nullptr);

    const float fsx = dd->FramebufferScale.x;
    const float fsy = dd->FramebufferScale.y;
    const float disp_w = dd->DisplaySize.x;
    const float disp_h = dd->DisplaySize.y;
    const float fb_w = disp_w * fsx;
    const float fb_h = disp_h * fsy;
    float pc[4];
    pc[0] = 2.0f / disp_w;
    pc[1] = 2.0f / disp_h;
    pc[2] = -1.0f - dd->DisplayPos.x * pc[0];
    pc[3] = -1.0f - dd->DisplayPos.y * pc[1];
    vkCmdPushConstants(gfx_, sh->vk_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);

    // Positive-height viewport (overrides the pass's Metal-convention flip) so the
    // standard imgui ortho maps points -> framebuffer upright on Vulkan.
    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = fb_w;
    vp.height = fb_h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(gfx_, 0, 1, &vp);

    VkBuffer master = res.GetVkBumpMasterBuffer(alloc, Memory::kDynamic);
    const ImVec2 clip_off = dd->DisplayPos;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        const size_t vbytes = static_cast<size_t>(cl->VtxBuffer.Size) * sizeof(ImDrawVert);
        const size_t ibytes = static_cast<size_t>(cl->IdxBuffer.Size) * sizeof(ImDrawIdx);
        uint32_t voff = 0;
        uint32_t ioff = 0;
        void* vptr = alloc.BumpAllocate(static_cast<uint32_t>(vbytes), 16,
                                        Memory::kDynamic, &voff);
        void* iptr = alloc.BumpAllocate(static_cast<uint32_t>(ibytes), 4,
                                        Memory::kDynamic, &ioff);
        std::memcpy(vptr, cl->VtxBuffer.Data, vbytes);
        std::memcpy(iptr, cl->IdxBuffer.Data, ibytes);
        VkDeviceSize vbo = voff;
        vkCmdBindVertexBuffers(gfx_, 0, 1, &master, &vbo);
        vkCmdBindIndexBuffer(gfx_, master, ioff,
                             sizeof(ImDrawIdx) == 2 ? VK_INDEX_TYPE_UINT16
                                                    : VK_INDEX_TYPE_UINT32);
        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImDrawCmd* cmd = &cl->CmdBuffer[c];
            float cx = (cmd->ClipRect.x - clip_off.x) * fsx;
            float cy = (cmd->ClipRect.y - clip_off.y) * fsy;
            float cz = (cmd->ClipRect.z - clip_off.x) * fsx;
            float cw = (cmd->ClipRect.w - clip_off.y) * fsy;
            cx = cx < 0.0f ? 0.0f : cx;
            cy = cy < 0.0f ? 0.0f : cy;
            cz = cz > fb_w ? fb_w : cz;
            cw = cw > fb_h ? fb_h : cw;
            if (cz <= cx || cw <= cy) {
                continue;
            }
            VkRect2D scis{};
            scis.offset = {static_cast<int32_t>(cx), static_cast<int32_t>(cy)};
            scis.extent = {static_cast<uint32_t>(cz - cx), static_cast<uint32_t>(cw - cy)};
            vkCmdSetScissor(gfx_, 0, 1, &scis);
            vkCmdDrawIndexed(gfx_, cmd->ElemCount, 1, cmd->IdxOffset,
                             static_cast<int32_t>(cmd->VtxOffset), 0);
        }
    }
}

void CommandRecorder::SetViewport(float x, float y, float w, float h) {
    // Negative height to keep the Metal-convention Y-flip (see BeginRenderPass).
    VkViewport vp{};
    vp.x = x;
    vp.y = y + h;
    vp.width = w;
    vp.height = -h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(gfx_, 0, 1, &vp);
}

void CommandRecorder::SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    VkRect2D s{};
    s.offset = {x, y};
    s.extent = {w, h};
    vkCmdSetScissor(gfx_, 0, 1, &s);
}

void CommandRecorder::EndRenderPass() {
    vkCmdEndRenderPass(gfx_);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
