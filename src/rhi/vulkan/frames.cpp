// rhi/vulkan/frames.cpp

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <vulkan/vulkan.h>

#include <vector>

#include <stb_image_write.h>

#include "rhi/frames.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/resource_manager.hpp"  // kFramesInFlight
#include "rhi/swap_chain.hpp"
#include "rhi/swap_resolve_target.hpp"
#include "rhi/command_recorder.hpp"
#include "util/material_gpu.hpp"        // DrawTmp
#include "util/render_pass_globals.hpp" // RenderPassGlobals
#include "util/timer.hpp"

namespace cairns::rhi {

namespace {

VkCommandBuffer begin_single_time(VkDevice device, VkCommandPool pool) {
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void end_single_time(VkDevice device, VkCommandPool pool, VkQueue queue,
                     VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

uint32_t find_memory_type_idx(VkPhysicalDevice phys, uint32_t type_bits,
                              VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

void dump_swapchain_image(VkDevice device, VkPhysicalDevice phys,
                          VkCommandPool pool, VkQueue queue, VkImage image,
                          VkFormat format, uint32_t w, uint32_t h,
                          const char* path) {
    const VkDeviceSize buf_size = static_cast<VkDeviceSize>(w) * h * 4;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = buf_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    vkCreateBuffer(device, &bci, nullptr, &buf);

    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(device, buf, &mr);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_memory_type_idx(
        phys, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(device, &mai, nullptr, &mem);
    vkBindBufferMemory(device, buf, mem, 0);

    VkCommandBuffer cmd = begin_single_time(device, pool);
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_src.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_src);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1,
                           &region);

    VkImageMemoryBarrier to_present = to_src;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &to_present);
    end_single_time(device, pool, queue, cmd);

    void* mapped = nullptr;
    vkMapMemory(device, mem, 0, buf_size, 0, &mapped);
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    const bool is_bgra = (format == VK_FORMAT_B8G8R8A8_SRGB ||
                          format == VK_FORMAT_B8G8R8A8_UNORM);
    std::vector<uint8_t> rgba(static_cast<size_t>(buf_size));
    for (uint32_t i = 0; i < w * h; ++i) {
        if (is_bgra) {
            rgba[i * 4 + 0] = src[i * 4 + 2];
            rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4 + 0];
            rgba[i * 4 + 3] = src[i * 4 + 3];
        } else {
            rgba[i * 4 + 0] = src[i * 4 + 0];
            rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4 + 2];
            rgba[i * 4 + 3] = src[i * 4 + 3];
        }
    }
    vkUnmapMemory(device, mem);
    stbi_write_png(path, static_cast<int>(w), static_cast<int>(h), 4, rgba.data(),
                   static_cast<int>(w * 4));

    vkDestroyBuffer(device, buf, nullptr);
    vkFreeMemory(device, mem, nullptr);
}

}  // namespace

Frames::~Frames() { Deinit(); }

bool Frames::Init(Device& device) {
    if (inited_) {
        return true;
    }
    plat.device_ = device.plat.device_;
    plat.command_pool_ = device.plat.command_pool_;
    plat.physical_ = device.plat.physical_;
    plat.graphics_queue_ = device.plat.graphics_queue_;
    plat.compute_queue_ = device.plat.compute_queue_;
    plat.present_queue_ = device.plat.present_queue_;
    plat.ts_period_ns_ = device.plat.timestamp_period_ns_;
    plat.host_query_reset_ = device.plat.host_query_reset_;
    plat.vk_reset_query_pool_ = device.plat.vk_reset_query_pool_;
    {
        VkQueryPoolCreateInfo qpi{};
        qpi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2 * kMaxPasses * kFramesInFlight;
        if (vkCreateQueryPool(plat.device_, &qpi, nullptr, &plat.ts_pool_) != VK_SUCCESS) {
            return false;
        }
        plat.pass_names_.assign(kFramesInFlight, {});
        plat.pass_count_.assign(kFramesInFlight, 0);
        plat.compute_pass_count_.assign(kFramesInFlight, 0);
    }

    {  // per-frame command buffers + sync
        const uint32_t n = kFramesInFlight;
        plat.frames_in_flight_ = n;
        plat.graphics_cmds_.resize(n);
        plat.compute_cmds_.resize(n);
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = plat.command_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = n;
        if (vkAllocateCommandBuffers(plat.device_, &cai,
                                     plat.graphics_cmds_.data()) != VK_SUCCESS ||
            vkAllocateCommandBuffers(plat.device_, &cai,
                                     plat.compute_cmds_.data()) != VK_SUCCESS) {
            return false;
        }

        plat.image_available_.resize(n);
        plat.render_finished_.resize(n);
        plat.compute_finished_.resize(n);
        plat.in_flight_.resize(n);
        plat.compute_in_flight_.resize(n);
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < n; ++i) {
            if (vkCreateSemaphore(plat.device_, &sci, nullptr,
                                  &plat.image_available_[i]) != VK_SUCCESS ||
                vkCreateSemaphore(plat.device_, &sci, nullptr,
                                  &plat.render_finished_[i]) != VK_SUCCESS ||
                vkCreateSemaphore(plat.device_, &sci, nullptr,
                                  &plat.compute_finished_[i]) != VK_SUCCESS ||
                vkCreateFence(plat.device_, &fci, nullptr,
                              &plat.in_flight_[i]) != VK_SUCCESS ||
                vkCreateFence(plat.device_, &fci, nullptr,
                              &plat.compute_in_flight_[i]) != VK_SUCCESS) {
                return false;
            }
        }
    }

    {  // descriptor layouts + pool + per-frame sets (non-bindless)
        VkDevice dev = plat.device_;
        const uint32_t n = kFramesInFlight;

        {  // point layout (empty: particle render reads ssbo as a vertex buffer)
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &plat.point_layout_) !=
                VK_SUCCESS) {
                return false;
            }
        }
        {  // compute layout: UBO(dt)@0, SSBO read@1, SSBO write@2
            VkDescriptorSetLayoutBinding b[3]{};
            b[0].binding = 0;
            b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1;
            b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[2].binding = 2;
            b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            li.bindingCount = 3;
            li.pBindings = b;
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &plat.compute_layout_) !=
                VK_SUCCESS) {
                return false;
            }
        }
        {  // #221 Skinning Phase 4 -- Group B (frame-global). Set 0 of the
           // skin kernel: dynUBO Params @ 0, dynSSBO palettes @ 1, dynSSBO
           // InstanceMeta @ 2, SSBO output pool whole @ 3. Dynamic offsets
           // give us per-batch slicing of the kDynamic ring without
           // re-writing the descriptor set each frame.
            VkDescriptorSetLayoutBinding b[4]{};
            b[0].binding = 0;
            b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1;
            b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[2].binding = 2;
            b[2].descriptorCount = 1;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[3].binding = 3;
            b[3].descriptorCount = 1;
            b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            li.bindingCount = 4;
            li.pBindings = b;
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr,
                                            &plat.skin_group_b_layout_) !=
                VK_SUCCESS) {
                return false;
            }
        }
        {  // #221 Phase 5b -- anim_eval set layout (13 bindings; see
           // assets/anim_eval.comp.glsl). One DYNAMIC_UBO for actor records,
           // 12 SSBOs for scene tables + scratch + palette out.
            VkDescriptorSetLayoutBinding b[13]{};
            b[0].binding = 0;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            for (uint32_t i = 1; i < 13; ++i) {
                b[i].binding = i;
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            for (uint32_t i = 0; i < 13; ++i) {
                b[i].descriptorCount = 1;
                b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            li.bindingCount = 13;
            li.pBindings = b;
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr,
                                            &plat.anim_eval_layout_) !=
                VK_SUCCESS) {
                return false;
            }
        }
        {  // #221 Skinning Phase 4 -- Group A (per-mesh). Set 1 of the
           // skin kernel: SSBO positions slice @ 0, SSBO skin-attrs slice
           // @ 1. Built at load via Resources::CreateBindGroup; one set
           // per skinned mesh.
            VkDescriptorSetLayoutBinding b[2]{};
            b[0].binding = 0;
            b[0].descriptorCount = 1;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            b[1].binding = 1;
            b[1].descriptorCount = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            li.bindingCount = 2;
            li.pBindings = b;
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr,
                                            &plat.skin_group_a_layout_) !=
                VK_SUCCESS) {
                return false;
            }
        }
        // Per-frequency single dynamic-UBO layouts (Aaltonen split): set 0 globals
        // (once/frame), set 2 drawtmp (per draw). Structurally identical but kept as
        // distinct named layouts.
        auto make_dyn_ubo_layout = [&](VkDescriptorSetLayout* out) -> bool {
            VkDescriptorSetLayoutBinding b{};
            b.binding = 0;
            b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            li.bindingCount = 1;
            li.pBindings = &b;
            return vkCreateDescriptorSetLayout(dev, &li, nullptr, out) == VK_SUCCESS;
        };
        if (!make_dyn_ubo_layout(&plat.globals_set_layout_) ||
            !make_dyn_ubo_layout(&plat.drawtmp_set_layout_)) {
            return false;
        }

        // Composite descriptor set layout: 3 COMBINED_IMAGE_SAMPLER frag.
        // Binding 0 = primary color (all of composite_pip, depthviz, outline).
        // Binding 1 = secondary tex (id_off for outline). Binding 2 =
        // highlights texture (R32U 1xN; outline-only). composite_pip and
        // depthviz don't statically access bindings 1/2 so writing them is
        // a no-op for those pipelines.
        {
            VkDescriptorSetLayoutBinding b[3]{};
            b[0].binding = 0;
            b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[0].descriptorCount = 1;
            b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[1].binding = 1;
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[1].descriptorCount = 1;
            b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[2].binding = 2;
            b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[2].descriptorCount = 1;
            b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            li.bindingCount = 3;
            li.pBindings = b;
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr,
                                            &plat.composite_set_layout_) !=
                VK_SUCCESS) {
                return false;
            }
        }

        // Compute uses kMaxStepsPerFrame sets per slot (Fiedler N-step sim).
        // Composite uses kCompositeRingSize sets per slot (PIP multi-draw fix).
        // UBO count: point(n) + compute(n * kMaxStepsPerFrame).
        // SSBO count: compute (2 * n * kMaxStepsPerFrame) + skin Group B (n,
        //   1 SSBO @ binding 3 = output pool whole).
        // DYNAMIC UBO: globals(n) + drawtmp(n) + skin Group B Params (n).
        // DYNAMIC SSBO: skin Group B palettes (n) + InstanceMeta (n).
        // COMBINED_IMAGE_SAMPLER: composite (n * kCompositeRingSize).
        // maxSets: 3 single-set + compute + composite + skin_group_b (n).
        VkDescriptorPoolSize sizes[5]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = n + n * kMaxStepsPerFrame;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        // #221 Phase 9 (vk): + skin Group A (2 SSBO per skinned mesh,
        // independent of frame-in-flight count; 1024 budget per plan v7).
        // #221 Phase 5b (vk): + anim_eval (12 SSBO per frame-in-flight).
        sizes[1].descriptorCount =
            2 * n * kMaxStepsPerFrame + n + 2 * kMaxSkinnedMeshes + 12 * n;
        sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        // #221 Phase 5b: + anim_eval ActorRecord (1 dynUBO per frame-in-flight).
        sizes[2].descriptorCount = 2 * n + n + n;
        sizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        // 3 bindings per composite set (#207 outline shares the layout:
        // color + id + highlights).
        sizes[3].descriptorCount = 3 * n * kCompositeRingSize;
        // #221 Phase 4: skin Group B uses 2 SSBO_DYNAMIC (palettes + InstanceMeta).
        sizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        sizes[4].descriptorCount = 2 * n;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.poolSizeCount = 5;
        pci.pPoolSizes = sizes;
        // 3 single-set layouts + compute (kMaxStepsPerFrame) + composite
        // (kCompositeRingSize) + skin_group_b (1) per slot. Plus skin
        // Group A: one set per loaded skinned mesh (1024 budget).
        pci.maxSets = 3 * n + n * kMaxStepsPerFrame +
                       n * kCompositeRingSize + n + kMaxSkinnedMeshes + n;
        if (vkCreateDescriptorPool(dev, &pci, nullptr, &plat.descriptor_pool_) !=
            VK_SUCCESS) {
            return false;
        }

        auto alloc_sets = [&](VkDescriptorSetLayout layout,
                              std::vector<VkDescriptorSet>& out) -> bool {
            std::vector<VkDescriptorSetLayout> layouts(n, layout);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = plat.descriptor_pool_;
            ai.descriptorSetCount = n;
            ai.pSetLayouts = layouts.data();
            out.resize(n);
            return vkAllocateDescriptorSets(dev, &ai, out.data()) == VK_SUCCESS;
        };
        if (!alloc_sets(plat.point_layout_, plat.point_sets_) ||
            !alloc_sets(plat.globals_set_layout_, plat.globals_sets_) ||
            !alloc_sets(plat.drawtmp_set_layout_, plat.drawtmp_sets_) ||
            !alloc_sets(plat.skin_group_b_layout_, plat.skin_group_b_sets_) ||
            !alloc_sets(plat.anim_eval_layout_, plat.anim_eval_sets_)) {
            return false;
        }
        plat.compute_sets_.resize(n);
        {
            const uint32_t total = n * kMaxStepsPerFrame;
            std::vector<VkDescriptorSetLayout> layouts(total, plat.compute_layout_);
            std::vector<VkDescriptorSet> flat(total);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = plat.descriptor_pool_;
            ai.descriptorSetCount = total;
            ai.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(dev, &ai, flat.data()) != VK_SUCCESS) {
                return false;
            }
            for (uint32_t f = 0; f < n; ++f) {
                for (uint32_t k = 0; k < kMaxStepsPerFrame; ++k) {
                    plat.compute_sets_[f][k] = flat[f * kMaxStepsPerFrame + k];
                }
            }
        }
        plat.composite_sets_.resize(n);
        {
            const uint32_t total = n * kCompositeRingSize;
            std::vector<VkDescriptorSetLayout> layouts(total, plat.composite_set_layout_);
            std::vector<VkDescriptorSet> flat(total);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = plat.descriptor_pool_;
            ai.descriptorSetCount = total;
            ai.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(dev, &ai, flat.data()) != VK_SUCCESS) {
                return false;
            }
            for (uint32_t f = 0; f < n; ++f) {
                for (uint32_t k = 0; k < kCompositeRingSize; ++k) {
                    plat.composite_sets_[f][k] = flat[f * kCompositeRingSize + k];
                }
            }
        }
        plat.offscreen_target_cache_.device = dev;
    }
    inited_ = true;
    return true;
}

bool Frames::InitTargets(Resources&, Allocator&, uint32_t, uint32_t) {
    // depth/MSAA/render-pass already created in sc.plat.Init on Vulkan.
    return true;
}

void Frames::Deinit() {
    if (!inited_) {
        return;
    }
    VkDevice dev = plat.device_;
    for (uint32_t i = 0; i < plat.frames_in_flight_; ++i) {
        vkDestroySemaphore(dev, plat.image_available_[i], nullptr);
        vkDestroySemaphore(dev, plat.render_finished_[i], nullptr);
        vkDestroySemaphore(dev, plat.compute_finished_[i], nullptr);
        vkDestroyFence(dev, plat.in_flight_[i], nullptr);
        vkDestroyFence(dev, plat.compute_in_flight_[i], nullptr);
    }
    if (plat.descriptor_pool_) {
        vkDestroyDescriptorPool(dev, plat.descriptor_pool_, nullptr);
    }
    if (plat.globals_set_layout_) {
        vkDestroyDescriptorSetLayout(dev, plat.globals_set_layout_, nullptr);
    }
    if (plat.drawtmp_set_layout_) {
        vkDestroyDescriptorSetLayout(dev, plat.drawtmp_set_layout_, nullptr);
    }
    if (plat.compute_layout_) {
        vkDestroyDescriptorSetLayout(dev, plat.compute_layout_, nullptr);
    }
    if (plat.skin_group_b_layout_) {
        vkDestroyDescriptorSetLayout(dev, plat.skin_group_b_layout_, nullptr);
    }
    if (plat.skin_group_a_layout_) {
        vkDestroyDescriptorSetLayout(dev, plat.skin_group_a_layout_, nullptr);
    }
    if (plat.anim_eval_layout_) {
        vkDestroyDescriptorSetLayout(dev, plat.anim_eval_layout_, nullptr);
    }
    if (plat.point_layout_) {
        vkDestroyDescriptorSetLayout(dev, plat.point_layout_, nullptr);
    }
    if (plat.composite_set_layout_) {
        vkDestroyDescriptorSetLayout(dev, plat.composite_set_layout_, nullptr);
    }
    plat.offscreen_target_cache_.Deinit();
    if (plat.ts_pool_) {
        vkDestroyQueryPool(dev, plat.ts_pool_, nullptr);
        plat.ts_pool_ = VK_NULL_HANDLE;
    }
    inited_ = false;
}

void Frames::SetDumpPath(const std::filesystem::path& path) {
    dump_path_ = path;
}

// Framebuffers in the offscreen cache are sized at create-time against the
// prior swap dims; the (w, h) check inside get_offscreen_fb wouldn't match
// the new dims so they'd grow unboundedly. Wipe them on resize; render passes
// (keyed on format, not dims) survive.
void Frames::OnSurfaceResize() {
    plat.offscreen_target_cache_.FlushFramebuffers();
}

void Frames::WriteUnlitDescriptors(Resources& resources, Allocator& alloc) {
    if (plat.globals_sets_.empty() || plat.drawtmp_sets_.empty()) {
        return;
    }
    VkBuffer dyn_master =
        resources.plat.GetVkBumpMasterBuffer(alloc, Memory::kDynamic);
    if (dyn_master == VK_NULL_HANDLE) {
        return;
    }
    const uint32_t globals_range =
        static_cast<uint32_t>(sizeof(RenderPassGlobals));
    const uint32_t drawtmp_range = static_cast<uint32_t>(sizeof(DrawTmp));
    for (uint32_t i = 0; i < plat.globals_sets_.size(); ++i) {
        VkDescriptorBufferInfo bi[2]{};
        VkWriteDescriptorSet w[2]{};
        bi[0].buffer = dyn_master;
        bi[0].offset = 0;
        bi[0].range = globals_range;
        bi[1].buffer = dyn_master;
        bi[1].offset = 0;
        bi[1].range = drawtmp_range;
        for (uint32_t k = 0; k < 2; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstBinding = 0;
            w[k].descriptorCount = 1;
            w[k].descriptorType =
                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[k].pBufferInfo = &bi[k];
        }
        w[0].dstSet = plat.globals_sets_[i];
        w[1].dstSet = plat.drawtmp_sets_[i];
        vkUpdateDescriptorSets(plat.device_, 2, w, 0, nullptr);
    }
}

void Frames::WriteSkinGroupBDescriptors(Resources& resources,
                                          Allocator& alloc,
                                          Handle<Buffer> output_pool,
                                          Handle<Buffer> palette_buf) {
    if (plat.skin_group_b_sets_.empty() || output_pool.IsNull()) {
        return;
    }
    VkBuffer dyn_master =
        resources.plat.GetVkBumpMasterBuffer(alloc, Memory::kDynamic);
    uint32_t pool_master_off = 0;
    VkBuffer pool_buf = resources.plat.GetVkBuffer(alloc, output_pool,
                                                    &pool_master_off);
    if (dyn_master == VK_NULL_HANDLE || pool_buf == VK_NULL_HANDLE) {
        return;
    }
    // #221 Phase 5b: when palette_buf is non-null, binding 1 (palettes) points
    // at the persistent palette_out_buf_ (written by anim_eval) instead of
    // the kDynamic ring; the per-batch dynamic offset still selects the
    // bucket-relative palette window in mat4 stride.
    VkBuffer palette_target = dyn_master;
    uint32_t palette_master_off = 0;
    if (!palette_buf.IsNull()) {
        VkBuffer pal = resources.plat.GetVkBuffer(alloc, palette_buf,
                                                    &palette_master_off);
        if (pal != VK_NULL_HANDLE) {
            palette_target = pal;
        }
    }
    for (VkDescriptorSet set : plat.skin_group_b_sets_) {
        if (set == VK_NULL_HANDLE) {
            continue;
        }
        VkDescriptorBufferInfo bi[4]{};
        VkWriteDescriptorSet w[4]{};
        bi[0].buffer = dyn_master;
        bi[0].offset = 0;
        bi[0].range = 64u;
        bi[1].buffer = palette_target;
        bi[1].offset = palette_master_off;
        // #222 Phase 0.4 audit: original 65536 covers only 4 instances at
        // kAnimMaxJoints=256 (4*256*64). LoL fan-out at 500 actors / 100
        // distinct meshes hits 5/bucket -- 81920 bytes -- latently
        // overrunning this descriptor's bound view. VK_WHOLE_SIZE fixes the
        // bound size but VUID-06715 forbids any nonzero dynamic offset
        // alongside, breaking the per-batch dispatch model entirely.
        // Bumping to 1 MB covers 64-instance buckets comfortably; the
        // matching `dyn_off + range <= buffer_size` constraint is satisfied
        // because palette_out_buf_ keeps a >= range tail past the last
        // batch's start (16 MB buffer; max packed batches at 1024 actors *
        // 256 joints * 64 B = 16 MB; one slot's worth of overshoot fits
        // because we never fill the buffer to the brim AND have not raised
        // kAnimActorsCap past 1024). Phase D.3 retires this binding by
        // collapsing skin Group B into DynamicBuffers; do NOT inflate
        // further without growing palette_out_buf_ to match.
        bi[1].range = 1u << 20;
        bi[2].buffer = dyn_master;
        bi[2].offset = 0;
        bi[2].range = 16384u;
        bi[3].buffer = pool_buf;
        bi[3].offset = pool_master_off;
        bi[3].range = VK_WHOLE_SIZE;
        for (uint32_t i = 0; i < 4; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].pBufferInfo = &bi[i];
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        vkUpdateDescriptorSets(plat.device_, 4, w, 0, nullptr);
    }
}

void Frames::WriteAnimEvalDescriptors(
    Resources& resources, Allocator& alloc,
    Handle<Buffer> scene_headers,
    Handle<Buffer> parent_buf,
    Handle<Buffer> topo_buf,
    Handle<Buffer> bind_pose_buf,
    Handle<Buffer> channels_buf,
    Handle<Buffer> samplers_buf,
    Handle<Buffer> times_buf,
    Handle<Buffer> values_buf,
    Handle<Buffer> joint_nodes_buf,
    Handle<Buffer> inverse_binds_buf,
    Handle<Buffer> world_scratch,
    Handle<Buffer> palette_out) {
    if (plat.anim_eval_sets_.empty()) {
        return;
    }
    VkBuffer dyn_master =
        resources.plat.GetVkBumpMasterBuffer(alloc, Memory::kDynamic);
    if (dyn_master == VK_NULL_HANDLE) {
        return;
    }
    Handle<Buffer> ssbo_handles[12] = {
        scene_headers, parent_buf, topo_buf, bind_pose_buf,
        channels_buf, samplers_buf, times_buf, values_buf,
        joint_nodes_buf, inverse_binds_buf, world_scratch, palette_out,
    };
    uint32_t ssbo_offs[12]{};
    VkBuffer ssbo_bufs[12]{};
    for (uint32_t i = 0; i < 12; ++i) {
        if (ssbo_handles[i].IsNull()) {
            return;
        }
        ssbo_bufs[i] =
            resources.plat.GetVkBuffer(alloc, ssbo_handles[i], &ssbo_offs[i]);
        if (ssbo_bufs[i] == VK_NULL_HANDLE) {
            return;
        }
    }
    for (VkDescriptorSet set : plat.anim_eval_sets_) {
        if (set == VK_NULL_HANDLE) {
            continue;
        }
        VkDescriptorBufferInfo bi[13]{};
        VkWriteDescriptorSet w[13]{};
        bi[0].buffer = dyn_master;
        bi[0].offset = 0;
        // ActorRecord size 16 B; max kAnimActorsCap = 1024 records => 16 KB.
        bi[0].range = 16384u;
        for (uint32_t i = 0; i < 12; ++i) {
            bi[1 + i].buffer = ssbo_bufs[i];
            bi[1 + i].offset = ssbo_offs[i];
            bi[1 + i].range = VK_WHOLE_SIZE;
        }
        for (uint32_t i = 0; i < 13; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].pBufferInfo = &bi[i];
            w[i].descriptorType = (i == 0)
                ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        vkUpdateDescriptorSets(plat.device_, 13, w, 0, nullptr);
    }
}

FrameContext Frames::Begin(Resources& resources, Allocator& alloc,
                            const SwapResolveTarget& target) {
    // Surfaceless mode (target.plat.swap_chain == nullptr): no swapchain
    // image to acquire, no image_available semaphore to signal, no present.
    // Frames goes through the same render-pass + offscreen-framebuffer path
    // (BeginRenderPass routes via the offscreen target cache when the swap
    // pass's color target is non-null final_target_).
    const bool surfaceless = target.plat.swap_chain == nullptr;
    SwapChain* sc = surfaceless ? nullptr : target.plat.swap_chain;
    const uint32_t cf = plat.recorder_frame_;
    VkDevice dev = plat.device_;

    {
        cairns::Timer t_fw("fence_wait", 11);
        vkWaitForFences(dev, 1, &plat.compute_in_flight_[cf], VK_TRUE, UINT64_MAX);
        vkWaitForFences(dev, 1, &plat.in_flight_[cf], VK_TRUE, UINT64_MAX);
    }

    // Both queues' slot-`cf` timestamps are now resolved -- read them BEFORE
    // resetting fences / cmd buffers / the query pool itself.
    {
        const uint32_t ncomp = plat.compute_pass_count_[cf];
        const uint32_t ngfx = plat.pass_count_[cf];
        if (ncomp > 0 || ngfx > 0) {
            std::array<uint64_t, 2 * kMaxPasses> ticks{};
            vkGetQueryPoolResults(dev, plat.ts_pool_, 2 * kMaxPasses * cf,
                                  2 * kMaxPasses,
                                  ticks.size() * sizeof(uint64_t), ticks.data(),
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            auto report = [&](uint32_t pass_idx) {
                const double ns =
                    (static_cast<double>(ticks[2 * pass_idx + 1] - ticks[2 * pass_idx])) *
                    static_cast<double>(plat.ts_period_ns_);
                const char* nm = plat.pass_names_[cf][pass_idx];
                if (nm) {
                    TimerStorage::Span(TimerStorage::SlotForPass(nm), nm,
                                       static_cast<uint64_t>(ns / 1000.0));
                }
            };
            for (uint32_t p = 0; p < ncomp; ++p) {
                report(p);
            }
            for (uint32_t p = 0; p < ngfx; ++p) {
                report(kMaxComputePasses + p);
            }
        }
        plat.pass_count_[cf] = 0;
        plat.compute_pass_count_[cf] = 0;
    }
    if (plat.host_query_reset_) {
        plat.vk_reset_query_pool_(dev, plat.ts_pool_, 2 * kMaxPasses * cf,
                             2 * kMaxPasses);
    }

    vkResetFences(dev, 1, &plat.compute_in_flight_[cf]);
    vkResetCommandBuffer(plat.compute_cmds_[cf], 0);
    resources.AdvanceFrame(alloc);  // bump ring reset

    uint32_t image_index = 0;
    if (sc) {
        cairns::Timer t_acq("acquire_wait", 10);
        std::lock_guard<std::mutex> lk(plat.swapchain_mutex_);
        VkResult acquire = vkAcquireNextImageKHR(dev, sc->plat.swapChain, UINT64_MAX,
                                                 plat.image_available_[cf], VK_NULL_HANDLE,
                                                 &image_index);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR ||
            acquire == VK_ERROR_SURFACE_LOST_KHR) {
            plat.recreate_pending_.store(true, std::memory_order_release);
            FrameContext fc{};
            fc.skip_frame = true;
            return fc;
        }
    }
    vkResetFences(dev, 1, &plat.in_flight_[cf]);
    vkResetCommandBuffer(plat.graphics_cmds_[cf], 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(plat.compute_cmds_[cf], &bi);
    vkBeginCommandBuffer(plat.graphics_cmds_[cf], &bi);

    // Fallback path when hostQueryReset is unavailable: per-queue cmd reset
    // so the compute subrange's reset is ordered against this frame's compute
    // writes, and likewise for graphics.
    if (!plat.host_query_reset_) {
        vkCmdResetQueryPool(plat.compute_cmds_[cf], plat.ts_pool_,
                            2 * kMaxPasses * cf,
                            2 * kMaxComputePasses);
        vkCmdResetQueryPool(plat.graphics_cmds_[cf], plat.ts_pool_,
                            2 * kMaxPasses * cf + 2 * kMaxComputePasses,
                            2 * (kMaxPasses - kMaxComputePasses));
    }

    FrameContext fc;
    fc.frame_index = cf;
    fc.swapchain_image_index = image_index;
    fc.cmd.plat.frame_ = cf;
    fc.cmd.plat.image_index_ = image_index;
    fc.cmd.plat.gfx_ = plat.graphics_cmds_[cf];
    fc.cmd.plat.comp_ = plat.compute_cmds_[cf];
    fc.cmd.plat.device_ = dev;
    fc.cmd.plat.globals_set_ = plat.globals_sets_[cf];
    fc.cmd.plat.drawtmp_set_ = plat.drawtmp_sets_[cf];
    fc.cmd.plat.compute_sets_ = plat.compute_sets_[cf];
    fc.cmd.plat.skin_group_b_set_ = plat.skin_group_b_sets_[cf];
    fc.cmd.plat.anim_eval_set_ = plat.anim_eval_sets_[cf];
    fc.cmd.plat.point_set_ = plat.point_sets_[cf];
    fc.cmd.plat.composite_sets_ = plat.composite_sets_[cf];
    fc.cmd.plat.composite_next_idx_ = 0;
    fc.cmd.plat.offscreen_ = &plat.offscreen_target_cache_;
    fc.cmd.plat.ts_pool_ = plat.ts_pool_;
    fc.cmd.plat.pass_names_ = &plat.pass_names_[cf];
    fc.cmd.plat.pass_count_ = &plat.pass_count_[cf];
    fc.cmd.plat.compute_pass_count_ = &plat.compute_pass_count_[cf];
    fc.cmd.plat.pass_cb_ = VK_NULL_HANDLE;
    fc.cmd.plat.pending_pass_idx_ = UINT32_MAX;
    fc.cmd.pending_name_ = nullptr;
    fc.cmd.pending_slot_ = -1;
    return fc;
}

// Render-thread safe. End all open command buffers + vkQueueSubmit both
// queues. Does NOT call vkQueuePresentKHR -- see Present below.
void Frames::EndSubmit(const SwapResolveTarget& target, FrameContext& fc) {
    const bool surfaceless = target.plat.swap_chain == nullptr;
    CommandRecorder& ri = fc.cmd;
    const uint32_t cf = fc.frame_index;

    vkEndCommandBuffer(ri.plat.comp_);
    VkSubmitInfo csi{};
    csi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    csi.commandBufferCount = 1;
    csi.pCommandBuffers = &plat.compute_cmds_[cf];
    csi.signalSemaphoreCount = 1;
    csi.pSignalSemaphores = &plat.compute_finished_[cf];
    vkQueueSubmit(plat.compute_queue_, 1, &csi, plat.compute_in_flight_[cf]);

    vkEndCommandBuffer(ri.plat.gfx_);
    VkSubmitInfo gsi{};
    gsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    // Surfaceless: no image_available (no acquire). Wait only on compute.
    VkSemaphore wait_sems[2] = {plat.compute_finished_[cf],
                                plat.image_available_[cf]};
    VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    gsi.waitSemaphoreCount = surfaceless ? 1u : 2u;
    gsi.pWaitSemaphores = wait_sems;
    gsi.pWaitDstStageMask = wait_stages;
    gsi.commandBufferCount = 1;
    gsi.pCommandBuffers = &plat.graphics_cmds_[cf];
    // Surfaceless: nobody will wait on render_finished (no present).
    gsi.signalSemaphoreCount = surfaceless ? 0u : 1u;
    gsi.pSignalSemaphores = surfaceless ? nullptr : &plat.render_finished_[cf];
    vkQueueSubmit(plat.graphics_queue_, 1, &gsi, plat.in_flight_[cf]);
    plat.recorder_frame_ = (cf + 1) % plat.frames_in_flight_;
}

// MAIN-THREAD ONLY. vkQueuePresentKHR on MoltenVK calls into CALayer
// (-[CALayer setNeedsDisplayInRect:]) which is documented main-thread-only.
// Calling from a render-thread worker fires CA_ASSERT_MAIN_THREAD_TRANSACTIONS
// under Instruments (and is undefined behavior otherwise).
void Frames::Present(const SwapResolveTarget& target, FrameContext& fc) {
    if (fc.skip_frame) {
        return;
    }
    const uint32_t cf = fc.frame_index;
    // Surfaceless: no swapchain to present to. Wait for the graphics
    // submit to finish so io.dumpTexture / final_target_ sampling sees
    // the rendered pixels, then advance the frame counter.
    if (target.plat.swap_chain == nullptr) {
        vkWaitForFences(plat.device_, 1, &plat.in_flight_[cf], VK_TRUE, UINT64_MAX);
        return;
    }
    SwapChain& sc = *target.plat.swap_chain;

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &plat.render_finished_[cf];
    VkSwapchainKHR swapchains[1] = {sc.plat.swapChain};
    pi.swapchainCount = 1;
    pi.pSwapchains = swapchains;
    pi.pImageIndices = &fc.swapchain_image_index;
    VkResult present;
    {
        std::lock_guard<std::mutex> lk(plat.swapchain_mutex_);
        present = vkQueuePresentKHR(plat.present_queue_, &pi);
    }
    if (present == VK_ERROR_OUT_OF_DATE_KHR) {
        plat.recreate_pending_.store(true, std::memory_order_release);
    }

    if (!dump_path_.empty()) {
        vkQueueWaitIdle(plat.present_queue_);
        dump_swapchain_image(plat.device_, plat.physical_,
                             plat.command_pool_, plat.graphics_queue_,
                             sc.plat.swapChainImages[fc.swapchain_image_index],
                             sc.plat.swapChainImageFormat,
                             sc.plat.swapChainExtent.width,
                             sc.plat.swapChainExtent.height,
                             dump_path_.string().c_str());
        dump_path_.clear();
    }

}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
