// rhi/vulkan/frames.cpp

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <vulkan/vulkan.h>

#include <vector>

#include <stb_image_write.h>

#include "rhi/frames.hpp"
#include "rhi/device.hpp"
#include "rhi/frame_capture.hpp"
#include "rhi/gpu_profiler.hpp"
#include "rhi/offscreen_targets.hpp"
#include "rhi/pipelines.hpp"
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

bool Frames::Init(Device& device, Pipelines& pipelines) {
    PipelinesPlat& pp = pipelines.plat;
    if (inited_) {
        return true;
    }
    plat.device_ = device.plat.device_;
    plat.command_pool_ = device.plat.command_pool_;
    plat.physical_ = device.plat.physical_;
    plat.graphics_queue_ = device.plat.graphics_queue_;
    plat.compute_queue_ = device.plat.compute_queue_;
    plat.present_queue_ = device.plat.present_queue_;
    // #222 Phase F.1: query pool + per-FIF tracker init moved to
    // GpuProfiler::Init (called from engine before Frames::Init).

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

    // #222 Phase F.4: descriptor set layouts moved to Pipelines::Init;
    // Frames only owns per-FIF sets + the pool below.
    {  // pool + per-frame sets (layouts read from PipelinesPlat).
        VkDevice dev = plat.device_;
        const uint32_t n = kFramesInFlight;


        // Pool sizing post-E.2 (point_sets_ retired):
        //   SSBO: skin Group A (2 per skinned mesh, kMaxSkinnedMeshes
        //     budget) + dyn_skin_group_b_ binding 3 (n) + dyn_anim_eval_
        //     bindings 1..12 (12n) + dyn_particle_parity_[2] (4n).
        //   UBO_DYN: globals (n) + drawtmp (n) + dyn_globals_/drawtmp_
        //     (2n) + dyn_skin_group_b_ params (n) + dyn_anim_eval_ records
        //     (n) + dyn_particle_parity_[2] dt (2n).
        //   SSBO_DYN: dyn_skin_group_b_ palettes + meta (2n).
        //   COMBINED_IMAGE_SAMPLER: composite (3 * n * kCompositeRingSize).
        //   maxSets: globals (n) + drawtmp (n) + composite
        //     (n * kCompositeRingSize) + skin Group A (kMaxSkinnedMeshes)
        //     + dyn_globals_ + dyn_drawtmp_ (2n) + dyn_skin_group_b_ (n)
        //     + dyn_anim_eval_ (n) + dyn_particle_parity_[2] (2n).
        VkDescriptorPoolSize sizes[4]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[0].descriptorCount =
            n + 2 * kMaxSkinnedMeshes + 12 * n + 4 * n;
        sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        sizes[1].descriptorCount = n + n + 2 * n + n + n + 2 * n;
        sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[2].descriptorCount = 3 * n * kCompositeRingSize;
        sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        sizes[3].descriptorCount = 2 * n;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.poolSizeCount = 4;
        pci.pPoolSizes = sizes;
        pci.maxSets = 2 * n + n * kCompositeRingSize + kMaxSkinnedMeshes +
                       2 * n + n + n + 2 * n;
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
        if (!alloc_sets(pp.globals_set_layout_, plat.globals_sets_) ||
            !alloc_sets(pp.drawtmp_set_layout_, plat.drawtmp_sets_)) {
            return false;
        }
        // #222 Phase D.3/D.4 cleanup: per-step compute_sets_ + per-FIF
        // skin_group_b_sets_ + anim_eval_sets_ retired. Particle / skin
        // Group B / anim_eval all use DynamicBuffers created in engine.
        plat.composite_sets_.resize(n);
        {
            const uint32_t total = n * kCompositeRingSize;
            std::vector<VkDescriptorSetLayout> layouts(total, pp.composite_set_layout_);
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
    // #222 Phase F.1: profiler teardown moved to GpuProfiler::Deinit.
    // #222 Phase F.3: offscreen-target cache teardown moved to
    // OffscreenTargets::Deinit.
    // #222 Phase F.4: descriptor set layout teardown moved to
    // Pipelines::Deinit.
    inited_ = false;
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

FrameContext Frames::Begin(Resources& resources, Allocator& alloc,
                            GpuProfiler& gpu_profiler,
                            OffscreenTargets& offscreen_targets,
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
    // #228 F1 (v2): drain any pending free whose retire_frame <=
    // current frame_index_. Both fences for slot |cf| signaled above
    // prove the kFIF-frames-ago frame is GPU-done; per-resource
    // retire_frame stamping covers the between-frames push case the
    // v1 per-slot bucket got wrong.
    resources.DrainDeferredFrees(alloc, resources.FrameIndex());

    // Both queues' slot-`cf` timestamps are now resolved -- read them BEFORE
    // resetting fences / cmd buffers / the query pool itself.
    GpuProfilerPlat& gp = gpu_profiler.plat;
    {
        const uint32_t ncomp = gp.compute_pass_count_[cf];
        const uint32_t ngfx = gp.pass_count_[cf];
        if (ncomp > 0 || ngfx > 0) {
            std::array<uint64_t, 2 * kMaxPasses> ticks{};
            vkGetQueryPoolResults(dev, gp.ts_pool_, 2 * kMaxPasses * cf,
                                  2 * kMaxPasses,
                                  ticks.size() * sizeof(uint64_t), ticks.data(),
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            auto report = [&](uint32_t pass_idx) {
                const double ns =
                    (static_cast<double>(ticks[2 * pass_idx + 1] - ticks[2 * pass_idx])) *
                    static_cast<double>(gp.ts_period_ns_);
                const char* nm = gp.pass_names_[cf][pass_idx];
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
        gp.pass_count_[cf] = 0;
        gp.compute_pass_count_[cf] = 0;
    }
    if (gp.host_query_reset_) {
        gp.vk_reset_query_pool_(dev, gp.ts_pool_, 2 * kMaxPasses * cf,
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
    if (!gp.host_query_reset_) {
        vkCmdResetQueryPool(plat.compute_cmds_[cf], gp.ts_pool_,
                            2 * kMaxPasses * cf,
                            2 * kMaxComputePasses);
        vkCmdResetQueryPool(plat.graphics_cmds_[cf], gp.ts_pool_,
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
    fc.cmd.plat.composite_sets_ = plat.composite_sets_[cf];
    fc.cmd.plat.composite_next_idx_ = 0;
    fc.cmd.plat.offscreen_ = &offscreen_targets.plat.cache;
    fc.cmd.plat.profiler_.ts_pool_ = gp.ts_pool_;
    fc.cmd.plat.profiler_.pass_names_ = &gp.pass_names_[cf];
    fc.cmd.plat.profiler_.pass_count_ = &gp.pass_count_[cf];
    fc.cmd.plat.profiler_.compute_pass_count_ = &gp.compute_pass_count_[cf];
    fc.cmd.plat.pass_cb_ = VK_NULL_HANDLE;
    fc.cmd.plat.pending_pass_idx_ = UINT32_MAX;
    fc.cmd.pending_name_ = nullptr;
    fc.cmd.pending_slot_ = -1;
    return fc;
}

void Frames::PresentFromFinalTarget(SwapChain& /*swapchain*/,
                                      Allocator& /*alloc*/,
                                      Resources& /*resources*/,
                                      Handle<Texture> /*src_target*/) {
}

// Render-thread safe. End all open command buffers + vkQueueSubmit both
// queues. Does NOT call vkQueuePresentKHR -- see Present below.
void Frames::EndSubmit(const SwapResolveTarget& target,
                        FrameCapture& /*frame_capture*/, FrameContext& fc) {
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
void Frames::Present(const SwapResolveTarget& target,
                      FrameCapture& frame_capture, FrameContext& fc) {
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

    if (frame_capture.Pending()) {
        vkQueueWaitIdle(plat.present_queue_);
        dump_swapchain_image(plat.device_, plat.physical_,
                             plat.command_pool_, plat.graphics_queue_,
                             sc.plat.swapChainImages[fc.swapchain_image_index],
                             sc.plat.swapChainImageFormat,
                             sc.plat.swapChainExtent.width,
                             sc.plat.swapChainExtent.height,
                             frame_capture.Path().string().c_str());
        frame_capture.Clear();
    }

}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
