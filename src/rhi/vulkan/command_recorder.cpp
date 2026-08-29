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
#include "rhi/allocator.hpp"
#include "rhi/swap_chain.hpp"
#include "rhi/swap_resolve_target.hpp"
#include "util/draw.hpp"
#include "util/material_gpu.hpp"
#include "util/render_pass_globals.hpp"
#include "util/timer.hpp"

#include "imgui.h"

namespace cairns::rhi {

// --- OffscreenTargetCache helpers (owned by Frames) --------------------------

void OffscreenTargetCache::Deinit() {
    for (FbEntry& f : fbs) {
        if (f.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, f.fb, nullptr);
        }
    }
    fbs.clear();
    for (RpEntry& r : rps) {
        if (r.rp != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, r.rp, nullptr);
        }
    }
    rps.clear();
}

void OffscreenTargetCache::FlushFramebuffers() {
    for (FbEntry& f : fbs) {
        if (f.fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, f.fb, nullptr);
        }
    }
    fbs.clear();
}

static VkFormat to_vk_format(Format f) {
    switch (f) {
        case Format::kRgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::kBgra8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::kR32Uint:    return VK_FORMAT_R32_UINT;
        case Format::kD32F: return VK_FORMAT_D32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

static VkAttachmentLoadOp to_vk_load(LoadOp op) {
    switch (op) {
        case LoadOp::kClear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::kLoad: return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::kDontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

static VkAttachmentStoreOp to_vk_store(StoreOp op) {  // #222 Phase A.2
    switch (op) {
        case StoreOp::kStore: return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreOp::kDontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_STORE_OP_STORE;
}

static bool key_eq(const OffscreenTargetCache::RpKey& a,
                   const OffscreenTargetCache::RpKey& b) {
    if (a.color_count != b.color_count ||
        a.depth != b.depth ||
        a.color_load != b.color_load ||
        a.depth_load != b.depth_load ||
        a.depth_store != b.depth_store ||  // #222 Phase A.2
        a.has_depth != b.has_depth) {
        return false;
    }
    for (uint32_t i = 0; i < a.color_count; ++i) {
        if (a.colors[i] != b.colors[i]) return false;
        if (a.color_store[i] != b.color_store[i]) return false;  // #222 Phase A.2
    }
    return true;
}

static VkRenderPass get_offscreen_rp(OffscreenTargetCache* cache,
                                     const OffscreenTargetCache::RpKey& key) {
    for (const auto& e : cache->rps) {
        if (key_eq(e.key, key)) {
            return e.rp;
        }
    }
    constexpr uint32_t kMax = OffscreenTargetCache::kMaxColors + 1;
    VkAttachmentDescription atts[kMax]{};
    VkAttachmentReference color_refs[OffscreenTargetCache::kMaxColors]{};
    VkAttachmentReference depth_ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    uint32_t att_count = 0;
    for (uint32_t i = 0; i < key.color_count; ++i) {
        atts[att_count].format = key.colors[i];
        atts[att_count].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[att_count].loadOp = key.color_load;
        atts[att_count].storeOp = key.color_store[i];  // #222 Phase A.2
        atts[att_count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[att_count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // #207: when load_op=CLEAR the previous contents are discarded, so
        // initialLayout=UNDEFINED is the correct (and safer) value. Lets the
        // pool reuse a transient target that the validation tracker still
        // sees in SHADER_READ_ONLY from a prior pass without us needing to
        // emit a redundant transition barrier. With load_op=LOAD the caller
        // must arrive in COLOR_ATTACHMENT_OPTIMAL (we emit the transition
        // ourselves in BeginRenderPass).
        atts[att_count].initialLayout =
            (key.color_load == VK_ATTACHMENT_LOAD_OP_CLEAR)
                ? VK_IMAGE_LAYOUT_UNDEFINED
                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[att_count].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_refs[i].attachment = att_count;
        color_refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ++att_count;
    }
    if (key.has_depth) {
        atts[att_count].format = key.depth;
        atts[att_count].samples = VK_SAMPLE_COUNT_1_BIT;
        atts[att_count].loadOp = key.depth_load;
        atts[att_count].storeOp = key.depth_store;  // #222 Phase A.2
        atts[att_count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[att_count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[att_count].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[att_count].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref.attachment = att_count;
        ++att_count;
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = key.color_count;
    sub.pColorAttachments = key.color_count > 0 ? color_refs : nullptr;
    if (key.has_depth) {
        sub.pDepthStencilAttachment = &depth_ref;
    }
    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = att_count;
    ci.pAttachments = atts;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    vkCreateRenderPass(cache->device, &ci, nullptr, &rp);
    cache->rps.push_back({key, rp});
    return rp;
}

static VkFramebuffer get_offscreen_fb(OffscreenTargetCache* cache, VkRenderPass rp,
                                      const VkImageView* views, uint32_t view_count,
                                      uint32_t w, uint32_t h) {
    for (const auto& e : cache->fbs) {
        if (e.rp != rp || e.view_count != view_count || e.w != w || e.h != h) {
            continue;
        }
        bool match = true;
        for (uint32_t i = 0; i < view_count; ++i) {
            if (e.views[i] != views[i]) { match = false; break; }
        }
        if (match) return e.fb;
    }
    VkFramebufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass = rp;
    ci.attachmentCount = view_count;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    vkCreateFramebuffer(cache->device, &ci, nullptr, &fb);
    OffscreenTargetCache::FbEntry entry{};
    entry.rp = rp;
    for (uint32_t i = 0; i < view_count; ++i) entry.views[i] = views[i];
    entry.view_count = view_count;
    entry.w = w;
    entry.h = h;
    entry.fb = fb;
    cache->fbs.push_back(entry);
    return fb;
}

// Image-layout transition. Brute-force ALL_COMMANDS source/dst stages -- the
// per-pass count is small and we don't have a finer producer/consumer stage map.
static void transition(VkCommandBuffer cb, Resources& res, Handle<Texture> h,
                       VkImageLayout new_layout) {
    Texture::Hot* hot = res.GetHot(h);
    Texture::Cold* cold = res.textures.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    if (cold->plat.vk_layout == new_layout) {
        return;
    }
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = cold->plat.vk_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = reinterpret_cast<VkImage>(cold->api_image);
    if ((cold->usage & kTexUsageDepthTarget) != 0) {
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else {
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    cold->plat.vk_layout = new_layout;
}

void CommandRecorder::DispatchSkinBatches(
    Resources& res, Allocator& alloc, Handle<Kernel> kernel,
    Handle<Buffer> output_pool_buffer,
    std::span<const SkinDispatchBatch> batches) {
    if (batches.empty() || kernel.IsNull() ||
        output_pool_buffer.IsNull() ||
        plat.skin_group_b_set_ == VK_NULL_HANDLE) {
        return;
    }
    // Route onto plat.comp_ (free vertex-fetch sync via the existing
    // compute -> graphics semaphore @ VERTEX_INPUT in EndSubmit).
    if (plat.pending_pass_idx_ != UINT32_MAX &&
        plat.pass_cb_ == VK_NULL_HANDLE) {
        plat.pass_cb_ = plat.comp_;
        vkCmdWriteTimestamp(plat.pass_cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            plat.ts_pool_,
                            2 * kMaxPasses * plat.frame_ +
                                2 * plat.pending_pass_idx_);
    }

    Kernel::Hot* k = res.GetHot(kernel);
    if (!k) {
        return;
    }
    // Group B descriptors are written ONCE at engine init
    // (Frames::WriteSkinGroupBDescriptors). Per-dispatch we just provide
    // 3 dynamic byte offsets at vkCmdBindDescriptorSets time -- avoids
    // VUID-vkUpdateDescriptorSets-None-03047 (set in use by pending cmd
    // buffer) that fires when re-writing a sets-in-flight set every frame.
    (void)alloc;
    (void)output_pool_buffer;

    vkCmdBindPipeline(plat.comp_, VK_PIPELINE_BIND_POINT_COMPUTE,
                       k->plat.vk_pipeline);

    for (const SkinDispatchBatch& b : batches) {
        if (b.workgroups == 0 || b.mesh_set.IsNull()) {
            continue;
        }
        BindGroup::Hot* bg = res.bind_groups.GetHot(b.mesh_set);
        if (!bg || bg->api_descriptor_set == nullptr) {
            continue;
        }
        VkDescriptorSet sets[2] = {
            plat.skin_group_b_set_,
            static_cast<VkDescriptorSet>(bg->api_descriptor_set),
        };
        const uint32_t dyn_offsets[3] = {
            b.params_byte_offset,
            b.palettes_byte_offset,
            b.instance_meta_byte_offset,
        };
        vkCmdBindDescriptorSets(plat.comp_, VK_PIPELINE_BIND_POINT_COMPUTE,
                                 k->plat.vk_layout, 0, 2, sets,
                                 3, dyn_offsets);
        vkCmdDispatch(plat.comp_, b.workgroups, b.instance_count, 1);
    }
}

void CommandRecorder::DispatchAnimEval(
    Resources& res, Allocator& /*alloc*/, Handle<Kernel> kernel,
    const AnimEvalArgs& args) {
    const uint32_t actor_count = args.actor_count;
    const uint32_t records_byte_offset = args.records_byte_offset;
    if (kernel.IsNull() || actor_count == 0 ||
        plat.anim_eval_set_ == VK_NULL_HANDLE) {
        return;
    }
    if (plat.pending_pass_idx_ != UINT32_MAX &&
        plat.pass_cb_ == VK_NULL_HANDLE) {
        plat.pass_cb_ = plat.comp_;
        vkCmdWriteTimestamp(plat.pass_cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            plat.ts_pool_,
                            2 * kMaxPasses * plat.frame_ +
                                2 * plat.pending_pass_idx_);
    }
    Kernel::Hot* k = res.GetHot(kernel);
    if (!k) {
        return;
    }
    vkCmdBindPipeline(plat.comp_, VK_PIPELINE_BIND_POINT_COMPUTE,
                       k->plat.vk_pipeline);
    VkDescriptorSet set = plat.anim_eval_set_;
    const uint32_t dyn = records_byte_offset;
    vkCmdBindDescriptorSets(plat.comp_, VK_PIPELINE_BIND_POINT_COMPUTE,
                             k->plat.vk_layout, 0, 1, &set, 1, &dyn);
    vkCmdDispatch(plat.comp_, actor_count, 1, 1);
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(plat.comp_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                         0, nullptr, 0, nullptr);
}

void CommandRecorder::Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d) {
    if (plat.pending_pass_idx_ != UINT32_MAX && plat.pass_cb_ == VK_NULL_HANDLE) {
        plat.pass_cb_ = plat.comp_;
        vkCmdWriteTimestamp(plat.pass_cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            plat.ts_pool_,
                            2 * kMaxPasses * plat.frame_ + 2 * plat.pending_pass_idx_);
    }
    Kernel::Hot* k = res.GetHot(d.kernel);
    assert(d.step_index < kMaxStepsPerFrame);
    // Barrier between consecutive compute dispatches so step k+1 sees step
    // k's SSBO writes. Single global memory barrier -- only one buffer pair
    // is in play here. Skipped on the first step (nothing to wait on).
    if (d.step_index > 0) {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(plat.comp_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb,
                             0, nullptr, 0, nullptr);
    }
    VkDescriptorSet set = plat.compute_sets_[d.step_index];

    const size_t n = d.buffers.size();
    // #219 Chunk E: per-Dispatch descriptor-write scratch moved off the heap
    // onto the stack via fixed-cap arrays. Particle sim binds 3 buffers; cap
    // 8 leaves headroom for future kernels without ever allocating.
    assert(n <= kMaxBuffersPerDispatch &&
           "Dispatch buffer count exceeds kMaxBuffersPerDispatch -- raise cap");
    std::array<VkDescriptorBufferInfo, kMaxBuffersPerDispatch> infos{};
    std::array<VkWriteDescriptorSet, kMaxBuffersPerDispatch> writes{};
    for (size_t i = 0; i < n; ++i) {
        const BoundBuffer& b = d.buffers[i];
        uint32_t off = 0;
        VkBuffer buf = res.plat.GetVkBuffer(alloc,b.buffer, &off);
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
        vkUpdateDescriptorSets(plat.device_, static_cast<uint32_t>(n), writes.data(), 0,
                               nullptr);
    }

    vkCmdBindPipeline(plat.comp_, VK_PIPELINE_BIND_POINT_COMPUTE, k->plat.vk_pipeline);
    vkCmdBindDescriptorSets(plat.comp_, VK_PIPELINE_BIND_POINT_COMPUTE, k->plat.vk_layout,
                            0, 1, &set, 0, nullptr);
    vkCmdDispatch(plat.comp_, d.groups_x, d.groups_y, d.groups_z);
}

void CommandRecorder::BeginRenderPass(Resources& res, const SwapResolveTarget& target,
                                      const RenderPassDesc& desc) {
    // Surfaceless (vk render-to-texture) path: swap_chain is nullptr, every
    // swap pass desc routes final_target_ as desc.color[0].target so the
    // offscreen path below picks up the right framebuffer.
    if (plat.pending_pass_idx_ != UINT32_MAX && plat.pass_cb_ == VK_NULL_HANDLE) {
        plat.pass_cb_ = plat.gfx_;
        vkCmdWriteTimestamp(plat.pass_cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            plat.ts_pool_,
                            2 * kMaxPasses * plat.frame_ + 2 * plat.pending_pass_idx_);
    }

    // Sampled inputs from prior passes -> shader-read, for BOTH swapchain and
    // offscreen passes (e.g. composite samples offscreen color + depth while
    // rendering into the swapchain).
    for (const Handle<Texture>& in : desc.input_textures) {
        transition(plat.gfx_, res, in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    const bool is_swapchain = desc.color.empty() ||
                              desc.color[0].target.IsNull();
    VkExtent2D extent{desc.width, desc.height};

    if (is_swapchain) {
        SwapChain& sc = *target.plat.swap_chain;
        VkRenderPassBeginInfo rpi{};
        rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpi.renderPass = sc.plat.renderPass;
        rpi.framebuffer = sc.plat.swapChainFramebuffers[plat.image_index_];
        rpi.renderArea.offset = {0, 0};
        rpi.renderArea.extent = sc.plat.swapChainExtent;
        extent = sc.plat.swapChainExtent;
        VkClearValue clears[2]{};
        if (!desc.color.empty()) {
            clears[0].color = {{desc.color[0].clear[0], desc.color[0].clear[1],
                                desc.color[0].clear[2], desc.color[0].clear[3]}};
        }
        clears[1].depthStencil = {desc.depth.clear_depth, 0};
        rpi.clearValueCount = 2;
        rpi.pClearValues = clears;
        vkCmdBeginRenderPass(plat.gfx_, &rpi, VK_SUBPASS_CONTENTS_INLINE);
    } else {
        // #206 multi-color: iterate desc.color for every attachment. Each
        // gets transitioned to COLOR_ATTACHMENT_OPTIMAL, its view collected,
        // and its format hashed into the cache key.
        const uint32_t color_count = static_cast<uint32_t>(desc.color.size());
        const bool has_depth = !desc.depth.depth.IsNull();
        VkImageView color_views[OffscreenTargetCache::kMaxColors]{};
        VkImageView depth_view = VK_NULL_HANDLE;
        OffscreenTargetCache::RpKey key;
        key.color_count = color_count;
        key.has_depth = has_depth;
        for (uint32_t i = 0; i < color_count; ++i) {
            transition(plat.gfx_, res, desc.color[i].target,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            Texture::Cold* c = res.textures.GetCold(desc.color[i].target);
            color_views[i] = reinterpret_cast<VkImageView>(
                res.GetHot(desc.color[i].target)->api_view);
            key.colors[i] = to_vk_format(c->format);
            key.color_store[i] = to_vk_store(desc.color[i].store);  // #222 Phase A.2
            if (i == 0) {
                key.color_load = to_vk_load(desc.color[i].load);
            }
        }
        if (has_depth) {
            transition(plat.gfx_, res, desc.depth.depth,
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            Texture::Cold* c = res.textures.GetCold(desc.depth.depth);
            depth_view = reinterpret_cast<VkImageView>(
                res.GetHot(desc.depth.depth)->api_view);
            key.depth = to_vk_format(c->format);
            key.depth_load = to_vk_load(desc.depth.load);
            key.depth_store = to_vk_store(desc.depth.store);  // #222 Phase A.2
        }
        VkRenderPass rp = get_offscreen_rp(plat.offscreen_, key);
        // Attachment order matches the renderpass: colors[0..N) then depth.
        VkImageView views[OffscreenTargetCache::kMaxColors + 1]{};
        uint32_t view_count = 0;
        for (uint32_t i = 0; i < color_count; ++i) {
            views[view_count++] = color_views[i];
        }
        if (has_depth) views[view_count++] = depth_view;
        VkFramebuffer fb = get_offscreen_fb(plat.offscreen_, rp, views,
                                            view_count,
                                            extent.width, extent.height);
        VkClearValue clears[OffscreenTargetCache::kMaxColors + 1]{};
        uint32_t clear_n = 0;
        for (uint32_t i = 0; i < color_count; ++i) {
            clears[clear_n++].color = {{desc.color[i].clear[0],
                                        desc.color[i].clear[1],
                                        desc.color[i].clear[2],
                                        desc.color[i].clear[3]}};
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
        vkCmdBeginRenderPass(plat.gfx_, &rpi, VK_SUBPASS_CONTENTS_INLINE);
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
    vkCmdSetViewport(plat.gfx_, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(plat.gfx_, 0, 1, &scissor);
}

void CommandRecorder::DrawMeshes(Resources& res, Allocator& alloc, const MeshDrawList& list) {
    VkCommandBuffer cb = plat.gfx_;
    (void)alloc;  // descriptors now written once at init (Frames::WriteUnlitDescriptors).

    // #237 fix: globals + drawtmp descriptors are written ONCE at engine
    // init pointing at the master kDynamic buffer. Per-pass we only
    // supply the dynamic offset at vkCmdBindDescriptorSets time. Avoids
    // VUID-vkUpdateDescriptorSets-None-03047 (set in use by pending cmd).

    Shader::Hot* unlit = res.GetHot(list.pipeline);
    if (!unlit) {
        return;
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->plat.vk_pipeline);

    // set 0 globals: bind once for the whole pass.
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->plat.vk_layout, 0, 1,
                            &plat.globals_set_, 1, &list.globals_offset);

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
                                    unlit->plat.vk_layout, 1, 1, &ms, 0, nullptr);
        }
        uint32_t pos_off = 0;
        VkBuffer pos_buf =
            res.plat.GetVkBuffer(alloc,draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot], &pos_off);
        // #221 Skinning F5: skinned draws carry a per-actor byte offset
        // into the shared pos buffer (skin_output_pool_'s actor slice).
        // Static draws keep pos_buffer_byte_offset = 0 -- net stream-0
        // bind for them is unchanged.
        const uint32_t pos_off_total = pos_off + draw.pos_buffer_byte_offset;
        if (pos_buf != last_pos_buf || pos_off_total != last_pos_off) {
            last_pos_buf = pos_buf;
            last_pos_off = pos_off_total;
            VkDeviceSize off = pos_off_total;
            vkCmdBindVertexBuffers(cb, 0, 1, &pos_buf, &off);
        }
        uint32_t attr_off = 0;
        VkBuffer attr_buf =
            res.plat.GetVkBuffer(alloc,draw.vertex_buffers[cairns::Draw::kVertexBufferAttrSlot], &attr_off);
        if (attr_buf != last_attr_buf || attr_off != last_attr_off) {
            last_attr_buf = attr_buf;
            last_attr_off = attr_off;
            VkDeviceSize off = attr_off;
            vkCmdBindVertexBuffers(cb, cairns::kMeshAttrVertexBindSlot, 1, &attr_buf, &off);
        }
        uint32_t idx_base = 0;
        VkBuffer idx_buf = res.plat.GetVkBuffer(alloc,draw.index_buffer, &idx_base);
        if (idx_buf != last_idx_buf || idx_base != last_idx_off) {
            last_idx_buf = idx_buf;
            last_idx_off = idx_base;
            vkCmdBindIndexBuffer(cb, idx_buf, idx_base, VK_INDEX_TYPE_UINT32);
        }
        const uint32_t first_index = (draw.index_offset - idx_base) / sizeof(uint32_t);
        // set 2 drawtmp: the only per-draw dynamic offset.
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->plat.vk_layout, 2, 1,
                                &plat.drawtmp_set_, 1, &draw.dynamic_buffer_offsets[1]);
        vkCmdDrawIndexed(cb, draw.triangle_count * 3, draw.instance_count, first_index,
                         static_cast<int32_t>(draw.vertex_offset), draw.instance_offset);
    }
}

void CommandRecorder::DrawPoints(Resources& res, Allocator& alloc, const PointDraw& pd) {
    VkCommandBuffer cb = plat.gfx_;
    Shader::Hot* p = res.GetHot(pd.pipeline);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->plat.vk_pipeline);
    uint32_t ssbo_off = 0;
    VkBuffer ssbo = res.plat.GetVkBuffer(alloc,pd.vertex_buffer, &ssbo_off);
    VkDeviceSize off = ssbo_off;
    vkCmdBindVertexBuffers(cb, 0, 1, &ssbo, &off);
    VkDescriptorSet point_set = plat.point_set_;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->plat.vk_layout, 0, 1,
                            &point_set, 0, nullptr);
    vkCmdDraw(cb, pd.vertex_count, 1, 0, 0);
}

void CommandRecorder::DrawImGui(Resources& res, Allocator& alloc, Handle<Shader> pipeline,
                                Handle<Texture> font, Handle<Sampler> sampler,
                                const ImDrawData* dd) {
    if (!dd || dd->CmdListsCount == 0 || dd->DisplaySize.x <= 0.0f) {
        return;
    }
    Shader::Hot* sh = res.GetHot(pipeline);
    if (!sh || !sh->plat.vk_imgui_set) {
        return;
    }

    // #236 fix: imgui font + sampler are stable post-Engine init; the
    // per-frame write was racing the prior frame's pending gfx cmd
    // buffer (VUID-vkUpdateDescriptorSets-None-03047). Only call
    // vkUpdateDescriptorSets when the (font, sampler) handles actually
    // change. The cached packed values live on ShaderHotPlat.
    const uint32_t font_packed =
        (static_cast<uint32_t>(font.generation) << 16) | font.index;
    const uint32_t sampler_packed =
        (static_cast<uint32_t>(sampler.generation) << 16) | sampler.index;
    if (font_packed != sh->plat.vk_imgui_last_font_packed ||
        sampler_packed != sh->plat.vk_imgui_last_sampler_packed) {
        VkDescriptorImageInfo ii{};
        ii.sampler =
            reinterpret_cast<VkSampler>(res.GetHot(sampler)->api_sampler);
        ii.imageView =
            reinterpret_cast<VkImageView>(res.GetHot(font)->api_view);
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = sh->plat.vk_imgui_set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(plat.device_, 1, &w, 0, nullptr);
        sh->plat.vk_imgui_last_font_packed = font_packed;
        sh->plat.vk_imgui_last_sampler_packed = sampler_packed;
    }

    vkCmdBindPipeline(plat.gfx_, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->plat.vk_pipeline);
    vkCmdBindDescriptorSets(plat.gfx_, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->plat.vk_layout, 0, 1,
                            &sh->plat.vk_imgui_set, 0, nullptr);

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
    vkCmdPushConstants(plat.gfx_, sh->plat.vk_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = fb_w;
    vp.height = fb_h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(plat.gfx_, 0, 1, &vp);

    VkBuffer master = res.plat.GetVkBumpMasterBuffer(alloc, Memory::kDynamic);
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
        vkCmdBindVertexBuffers(plat.gfx_, 0, 1, &master, &vbo);
        vkCmdBindIndexBuffer(plat.gfx_, master, ioff,
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
            vkCmdSetScissor(plat.gfx_, 0, 1, &scis);
            vkCmdDrawIndexed(plat.gfx_, cmd->ElemCount, 1, cmd->IdxOffset,
                             static_cast<int32_t>(cmd->VtxOffset), 0);
        }
    }
}

void CommandRecorder::DrawFullscreen(Resources& res, Handle<Shader> pipeline,
                                     std::span<const Handle<Texture>> textures,
                                     Handle<Sampler> sampler) {
    Shader::Hot* sh = res.GetHot(pipeline);
    VkSampler samp = reinterpret_cast<VkSampler>(
        res.GetHot(sampler)->api_sampler);
    VkDescriptorSet set = plat.composite_sets_[plat.composite_next_idx_];
    plat.composite_next_idx_ = (plat.composite_next_idx_ + 1) % kCompositeRingSize;
    const uint32_t n = static_cast<uint32_t>(textures.size());
    std::array<VkDescriptorImageInfo, 4> infos{};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t i = 0; i < n; ++i) {
        infos[i].sampler = samp;
        infos[i].imageView = reinterpret_cast<VkImageView>(
            res.GetHot(textures[i])->api_view);
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].dstArrayElement = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(plat.device_, n, writes.data(), 0, nullptr);
    vkCmdBindPipeline(plat.gfx_, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->plat.vk_pipeline);
    vkCmdBindDescriptorSets(plat.gfx_, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->plat.vk_layout,
                            0, 1, &set, 0, nullptr);
    vkCmdDraw(plat.gfx_, 3, 1, 0, 0);
}

void CommandRecorder::SetViewport(float x, float y, float w, float h) {
    // Negative height matches BeginRenderPass: keep the Metal-convention Y-flip.
    VkViewport vp{};
    vp.x = x;
    vp.y = y + h;
    vp.width = w;
    vp.height = -h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(plat.gfx_, 0, 1, &vp);
}

void CommandRecorder::SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    VkRect2D s{};
    s.offset = {x, y};
    s.extent = {w, h};
    vkCmdSetScissor(plat.gfx_, 0, 1, &s);
}

void CommandRecorder::EndRenderPass() {
    vkCmdEndRenderPass(plat.gfx_);
}

void CommandRecorder::PassTimerBegin(const char* name, bool is_compute) {
    pending_name_ = name;
    pending_slot_ = TimerStorage::SlotForPass(name);
    if (plat.pass_count_ == nullptr || plat.pass_names_ == nullptr) {
        return;
    }
    uint32_t idx;
    if (is_compute) {
        if (*plat.compute_pass_count_ >= kMaxComputePasses) {
            plat.pending_pass_idx_ = UINT32_MAX;
            return;
        }
        idx = (*plat.compute_pass_count_)++;
    } else {
        if (*plat.pass_count_ >= kMaxPasses) {
            plat.pending_pass_idx_ = UINT32_MAX;
            return;
        }
        idx = kMaxComputePasses + ((*plat.pass_count_)++);
        if (idx >= kMaxPasses) {
            plat.pending_pass_idx_ = UINT32_MAX;
            return;
        }
    }
    plat.pending_pass_idx_ = idx;
    (*plat.pass_names_)[idx] = name;
    plat.pass_cb_ = VK_NULL_HANDLE;
}

void CommandRecorder::PassTimerEnd() {
    if (plat.pending_pass_idx_ != UINT32_MAX && plat.pass_cb_ != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(plat.pass_cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                            plat.ts_pool_,
                            2 * kMaxPasses * plat.frame_ + 2 * plat.pending_pass_idx_ + 1);
    }
    plat.pending_pass_idx_ = UINT32_MAX;
    pending_name_ = nullptr;
    pending_slot_ = -1;
    plat.pass_cb_ = VK_NULL_HANDLE;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
