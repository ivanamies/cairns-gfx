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
#include "rhi/command_recorder.hpp"

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
    device_ = device.device_;
    command_pool_ = device.command_pool_;
    physical_ = device.physical_;
    graphics_queue_ = device.graphics_queue_;
    compute_queue_ = device.compute_queue_;
    present_queue_ = device.present_queue_;

    {  // per-frame command buffers + sync
        const uint32_t n = kFramesInFlight;
        frames_in_flight_ = n;
        graphics_cmds_.resize(n);
        compute_cmds_.resize(n);
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = command_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = n;
        if (vkAllocateCommandBuffers(device_, &cai,
                                     graphics_cmds_.data()) != VK_SUCCESS ||
            vkAllocateCommandBuffers(device_, &cai,
                                     compute_cmds_.data()) != VK_SUCCESS) {
            return false;
        }

        image_available_.resize(n);
        render_finished_.resize(n);
        compute_finished_.resize(n);
        in_flight_.resize(n);
        compute_in_flight_.resize(n);
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < n; ++i) {
            if (vkCreateSemaphore(device_, &sci, nullptr,
                                  &image_available_[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device_, &sci, nullptr,
                                  &render_finished_[i]) != VK_SUCCESS ||
                vkCreateSemaphore(device_, &sci, nullptr,
                                  &compute_finished_[i]) != VK_SUCCESS ||
                vkCreateFence(device_, &fci, nullptr,
                              &in_flight_[i]) != VK_SUCCESS ||
                vkCreateFence(device_, &fci, nullptr,
                              &compute_in_flight_[i]) != VK_SUCCESS) {
                return false;
            }
        }
    }

    {  // descriptor layouts + pool + per-frame sets (non-bindless)
        VkDevice dev = device_;
        const uint32_t n = kFramesInFlight;

        {  // point layout (empty: particle render reads ssbo as a vertex buffer)
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &point_layout_) !=
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
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &compute_layout_) !=
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
        if (!make_dyn_ubo_layout(&globals_set_layout_) ||
            !make_dyn_ubo_layout(&drawtmp_set_layout_)) {
            return false;
        }

        VkDescriptorPoolSize sizes[3]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = n;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[1].descriptorCount = 2 * n;
        sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        sizes[2].descriptorCount = 2 * n;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.poolSizeCount = 3;
        pci.pPoolSizes = sizes;
        pci.maxSets = 4 * n;
        if (vkCreateDescriptorPool(dev, &pci, nullptr, &descriptor_pool_) !=
            VK_SUCCESS) {
            return false;
        }

        auto alloc_sets = [&](VkDescriptorSetLayout layout,
                              std::vector<VkDescriptorSet>& out) -> bool {
            std::vector<VkDescriptorSetLayout> layouts(n, layout);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = descriptor_pool_;
            ai.descriptorSetCount = n;
            ai.pSetLayouts = layouts.data();
            out.resize(n);
            return vkAllocateDescriptorSets(dev, &ai, out.data()) == VK_SUCCESS;
        };
        if (!alloc_sets(point_layout_, point_sets_) ||
            !alloc_sets(compute_layout_, compute_sets_) ||
            !alloc_sets(globals_set_layout_, globals_sets_) ||
            !alloc_sets(drawtmp_set_layout_, drawtmp_sets_)) {
            return false;
        }
    }
    inited_ = true;
    return true;
}

bool Frames::InitTargets(Resources&, Allocator&, SwapChain&) {
    // depth/MSAA/render-pass already created in sc.Init on Vulkan.
    return true;
}

void Frames::Deinit() {
    if (!inited_) {
        return;
    }
    VkDevice dev = device_;
    for (uint32_t i = 0; i < frames_in_flight_; ++i) {
        vkDestroySemaphore(dev, image_available_[i], nullptr);
        vkDestroySemaphore(dev, render_finished_[i], nullptr);
        vkDestroySemaphore(dev, compute_finished_[i], nullptr);
        vkDestroyFence(dev, in_flight_[i], nullptr);
        vkDestroyFence(dev, compute_in_flight_[i], nullptr);
    }
    if (descriptor_pool_) {
        vkDestroyDescriptorPool(dev, descriptor_pool_, nullptr);
    }
    if (globals_set_layout_) {
        vkDestroyDescriptorSetLayout(dev, globals_set_layout_, nullptr);
    }
    if (drawtmp_set_layout_) {
        vkDestroyDescriptorSetLayout(dev, drawtmp_set_layout_, nullptr);
    }
    if (compute_layout_) {
        vkDestroyDescriptorSetLayout(dev, compute_layout_, nullptr);
    }
    if (point_layout_) {
        vkDestroyDescriptorSetLayout(dev, point_layout_, nullptr);
    }
    inited_ = false;
}

void Frames::SetDumpPath(const std::filesystem::path& path) {
    dump_path_ = path;
}

FrameContext Frames::Begin(Resources& resources, Allocator& alloc, SwapChain& sc) {
    const uint32_t cf = recorder_frame_;
    VkDevice dev = device_;

    vkWaitForFences(dev, 1, &compute_in_flight_[cf], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &compute_in_flight_[cf]);
    vkResetCommandBuffer(compute_cmds_[cf], 0);

    vkWaitForFences(dev, 1, &in_flight_[cf], VK_TRUE, UINT64_MAX);
    resources.AdvanceFrame(alloc);  // bump ring reset

    uint32_t image_index = 0;
    VkResult acquire = vkAcquireNextImageKHR(dev, sc.swapChain, UINT64_MAX,
                                             image_available_[cf], VK_NULL_HANDLE,
                                             &image_index);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        sc.RecreateSwapChain();
        vkAcquireNextImageKHR(dev, sc.swapChain, UINT64_MAX, image_available_[cf],
                              VK_NULL_HANDLE, &image_index);
    }
    vkResetFences(dev, 1, &in_flight_[cf]);
    vkResetCommandBuffer(graphics_cmds_[cf], 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(compute_cmds_[cf], &bi);
    vkBeginCommandBuffer(graphics_cmds_[cf], &bi);

    FrameContext fc;
    fc.frame_index = cf;
    fc.swapchain_image_index = image_index;
    fc.cmd.frame_ = cf;
    fc.cmd.image_index_ = image_index;
    fc.cmd.gfx_ = graphics_cmds_[cf];
    fc.cmd.comp_ = compute_cmds_[cf];
    fc.cmd.device_ = dev;
    fc.cmd.globals_set_ = globals_sets_[cf];
    fc.cmd.drawtmp_set_ = drawtmp_sets_[cf];
    fc.cmd.compute_set_ = compute_sets_[cf];
    fc.cmd.point_set_ = point_sets_[cf];
    return fc;
}

void Frames::End(SwapChain& sc, FrameContext& fc) {
    CommandRecorder& ri = fc.cmd;
    const uint32_t cf = fc.frame_index;

    vkEndCommandBuffer(ri.comp_);
    VkSubmitInfo csi{};
    csi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    csi.commandBufferCount = 1;
    csi.pCommandBuffers = &compute_cmds_[cf];
    csi.signalSemaphoreCount = 1;
    csi.pSignalSemaphores = &compute_finished_[cf];
    vkQueueSubmit(compute_queue_, 1, &csi, compute_in_flight_[cf]);

    vkEndCommandBuffer(ri.gfx_);
    VkSubmitInfo gsi{};
    gsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore wait_sems[2] = {compute_finished_[cf],
                                image_available_[cf]};
    VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    gsi.waitSemaphoreCount = 2;
    gsi.pWaitSemaphores = wait_sems;
    gsi.pWaitDstStageMask = wait_stages;
    gsi.commandBufferCount = 1;
    gsi.pCommandBuffers = &graphics_cmds_[cf];
    gsi.signalSemaphoreCount = 1;
    gsi.pSignalSemaphores = &render_finished_[cf];
    vkQueueSubmit(graphics_queue_, 1, &gsi, in_flight_[cf]);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_[cf];
    VkSwapchainKHR swapchains[1] = {sc.swapChain};
    pi.swapchainCount = 1;
    pi.pSwapchains = swapchains;
    pi.pImageIndices = &fc.swapchain_image_index;
    const VkResult present = vkQueuePresentKHR(present_queue_, &pi);

    if (!dump_path_.empty()) {
        vkQueueWaitIdle(present_queue_);
        dump_swapchain_image(device_, physical_,
                             command_pool_, graphics_queue_,
                             sc.swapChainImages[fc.swapchain_image_index],
                             sc.swapChainImageFormat,
                             sc.swapChainExtent.width,
                             sc.swapChainExtent.height,
                             dump_path_.string().c_str());
        dump_path_.clear();
    }

    if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
        sc.RecreateSwapChain();
    }

    recorder_frame_ = (cf + 1) % frames_in_flight_;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
