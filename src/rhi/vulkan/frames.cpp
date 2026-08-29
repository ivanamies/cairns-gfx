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
#include "rhi/vulkan/command_recorder_impl.hpp"
#include "rhi/vulkan/internal/frames_impl.hpp"

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

bool Frames::Init(Device& device, Resources& resources) {
    if (impl_) {
        return true;
    }
    impl_ = new Impl();
    impl_->device = device.device_;
    impl_->command_pool = device.command_pool_;
    impl_->physical = device.physical_;
    impl_->graphics_queue = device.graphics_queue_;
    impl_->compute_queue = device.compute_queue_;
    impl_->present_queue = device.present_queue_;
    impl_->res = &resources;

    {  // per-frame command buffers + sync
        const uint32_t n = kFramesInFlight;
        impl_->frames_in_flight = n;
        impl_->graphics_cmds.resize(n);
        impl_->compute_cmds.resize(n);
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = impl_->command_pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = n;
        if (vkAllocateCommandBuffers(impl_->device, &cai,
                                     impl_->graphics_cmds.data()) != VK_SUCCESS ||
            vkAllocateCommandBuffers(impl_->device, &cai,
                                     impl_->compute_cmds.data()) != VK_SUCCESS) {
            return false;
        }

        impl_->image_available.resize(n);
        impl_->render_finished.resize(n);
        impl_->compute_finished.resize(n);
        impl_->in_flight.resize(n);
        impl_->compute_in_flight.resize(n);
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < n; ++i) {
            if (vkCreateSemaphore(impl_->device, &sci, nullptr,
                                  &impl_->image_available[i]) != VK_SUCCESS ||
                vkCreateSemaphore(impl_->device, &sci, nullptr,
                                  &impl_->render_finished[i]) != VK_SUCCESS ||
                vkCreateSemaphore(impl_->device, &sci, nullptr,
                                  &impl_->compute_finished[i]) != VK_SUCCESS ||
                vkCreateFence(impl_->device, &fci, nullptr,
                              &impl_->in_flight[i]) != VK_SUCCESS ||
                vkCreateFence(impl_->device, &fci, nullptr,
                              &impl_->compute_in_flight[i]) != VK_SUCCESS) {
                return false;
            }
        }
    }

    {  // descriptor layouts + pool + per-frame sets (non-bindless)
        VkDevice dev = impl_->device;
        const uint32_t n = kFramesInFlight;

        {  // point layout (empty: particle render reads ssbo as a vertex buffer)
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &impl_->point_layout) !=
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
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &impl_->compute_layout) !=
                VK_SUCCESS) {
                return false;
            }
        }
        {  // dynamic-UBO layout: globals@0, material@1, drawtmp@2
            VkDescriptorSetLayoutBinding b[3]{};
            for (uint32_t i = 0; i < 3; ++i) {
                b[i].binding = i;
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                b[i].descriptorCount = 1;
                b[i].stageFlags =
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo li{};
            li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            li.bindingCount = 3;
            li.pBindings = b;
            if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &impl_->dyn_ubo_layout) !=
                VK_SUCCESS) {
                return false;
            }
        }

        VkDescriptorPoolSize sizes[3]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = n;
        sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[1].descriptorCount = 2 * n;
        sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        sizes[2].descriptorCount = 3 * n;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.poolSizeCount = 3;
        pci.pPoolSizes = sizes;
        pci.maxSets = 3 * n;
        if (vkCreateDescriptorPool(dev, &pci, nullptr, &impl_->descriptor_pool) !=
            VK_SUCCESS) {
            return false;
        }

        auto alloc_sets = [&](VkDescriptorSetLayout layout,
                              std::vector<VkDescriptorSet>& out) -> bool {
            std::vector<VkDescriptorSetLayout> layouts(n, layout);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = impl_->descriptor_pool;
            ai.descriptorSetCount = n;
            ai.pSetLayouts = layouts.data();
            out.resize(n);
            return vkAllocateDescriptorSets(dev, &ai, out.data()) == VK_SUCCESS;
        };
        if (!alloc_sets(impl_->point_layout, impl_->point_sets) ||
            !alloc_sets(impl_->compute_layout, impl_->compute_sets) ||
            !alloc_sets(impl_->dyn_ubo_layout, impl_->dyn_ubo_sets)) {
            return false;
        }
    }
    return true;
}

bool Frames::InitTargets(SwapChain& sc) {
    (void)sc;  // depth/MSAA/render-pass already created in sc.Init on Vulkan.
    return true;
}

void Frames::Deinit() {
    if (!impl_) {
        return;
    }
    VkDevice dev = impl_->device;
    for (uint32_t i = 0; i < impl_->frames_in_flight; ++i) {
        vkDestroySemaphore(dev, impl_->image_available[i], nullptr);
        vkDestroySemaphore(dev, impl_->render_finished[i], nullptr);
        vkDestroySemaphore(dev, impl_->compute_finished[i], nullptr);
        vkDestroyFence(dev, impl_->in_flight[i], nullptr);
        vkDestroyFence(dev, impl_->compute_in_flight[i], nullptr);
    }
    if (impl_->descriptor_pool) {
        vkDestroyDescriptorPool(dev, impl_->descriptor_pool, nullptr);
    }
    if (impl_->dyn_ubo_layout) {
        vkDestroyDescriptorSetLayout(dev, impl_->dyn_ubo_layout, nullptr);
    }
    if (impl_->compute_layout) {
        vkDestroyDescriptorSetLayout(dev, impl_->compute_layout, nullptr);
    }
    if (impl_->point_layout) {
        vkDestroyDescriptorSetLayout(dev, impl_->point_layout, nullptr);
    }
    delete impl_;
    impl_ = nullptr;
}

void Frames::SetDumpPath(const std::filesystem::path& path) {
    impl_->dump_path = path;
}

FrameContext Frames::Begin(SwapChain& sc) {
    const uint32_t cf = impl_->recorder_frame;
    VkDevice dev = impl_->device;

    vkWaitForFences(dev, 1, &impl_->compute_in_flight[cf], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &impl_->compute_in_flight[cf]);
    vkResetCommandBuffer(impl_->compute_cmds[cf], 0);

    vkWaitForFences(dev, 1, &impl_->in_flight[cf], VK_TRUE, UINT64_MAX);
    impl_->res->AdvanceFrame();  // bump ring reset

    uint32_t image_index = 0;
    vkAcquireNextImageKHR(dev, sc.swapChain, UINT64_MAX, impl_->image_available[cf],
                          VK_NULL_HANDLE, &image_index);
    vkResetFences(dev, 1, &impl_->in_flight[cf]);
    vkResetCommandBuffer(impl_->graphics_cmds[cf], 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(impl_->compute_cmds[cf], &bi);
    vkBeginCommandBuffer(impl_->graphics_cmds[cf], &bi);

    FrameContext fc;
    fc.frame_index = cf;
    fc.swapchain_image_index = image_index;
    fc.cmd.impl_ = new CommandRecorder::Impl{
        impl_->res, &sc, cf, image_index, impl_->graphics_cmds[cf],
        impl_->compute_cmds[cf], dev, impl_->dyn_ubo_sets[cf],
        impl_->compute_sets[cf], impl_->point_sets[cf]};
    return fc;
}

void Frames::End(FrameContext& fc) {
    CommandRecorder::Impl* ri = fc.cmd.impl_;
    const uint32_t cf = fc.frame_index;

    vkEndCommandBuffer(ri->comp);
    VkSubmitInfo csi{};
    csi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    csi.commandBufferCount = 1;
    csi.pCommandBuffers = &impl_->compute_cmds[cf];
    csi.signalSemaphoreCount = 1;
    csi.pSignalSemaphores = &impl_->compute_finished[cf];
    vkQueueSubmit(impl_->compute_queue, 1, &csi, impl_->compute_in_flight[cf]);

    vkEndCommandBuffer(ri->gfx);
    VkSubmitInfo gsi{};
    gsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore wait_sems[2] = {impl_->compute_finished[cf],
                                impl_->image_available[cf]};
    VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    gsi.waitSemaphoreCount = 2;
    gsi.pWaitSemaphores = wait_sems;
    gsi.pWaitDstStageMask = wait_stages;
    gsi.commandBufferCount = 1;
    gsi.pCommandBuffers = &impl_->graphics_cmds[cf];
    gsi.signalSemaphoreCount = 1;
    gsi.pSignalSemaphores = &impl_->render_finished[cf];
    vkQueueSubmit(impl_->graphics_queue, 1, &gsi, impl_->in_flight[cf]);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &impl_->render_finished[cf];
    VkSwapchainKHR swapchains[1] = {ri->sc->swapChain};
    pi.swapchainCount = 1;
    pi.pSwapchains = swapchains;
    pi.pImageIndices = &fc.swapchain_image_index;
    vkQueuePresentKHR(impl_->present_queue, &pi);

    if (!impl_->dump_path.empty()) {
        vkQueueWaitIdle(impl_->present_queue);
        dump_swapchain_image(impl_->device, impl_->physical,
                             impl_->command_pool, impl_->graphics_queue,
                             ri->sc->swapChainImages[fc.swapchain_image_index],
                             ri->sc->swapChainImageFormat,
                             ri->sc->swapChainExtent.width,
                             ri->sc->swapChainExtent.height,
                             impl_->dump_path.string().c_str());
        impl_->dump_path.clear();
    }

    impl_->recorder_frame = (cf + 1) % impl_->frames_in_flight;
    delete ri;
    fc.cmd.impl_ = nullptr;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
