#pragma once

#include <vulkan/vulkan.h>
#include <stb_image_write.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace cairns::rhi {

inline bool is_bgra_format(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SNORM:
            return true;
        default:
            return false;
    }
}

inline uint32_t find_memory_type(VkPhysicalDevice physicalDevice,
                               uint32_t typeFilter,
                               VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        const bool typeOk = (typeFilter & (1u << i)) != 0;
        const bool propsOk =
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties;
        if (typeOk && propsOk) {
            return i;
        }
    }
    return 0;
}

// Reads back an already-rendered VkImage into a host buffer and writes it to a
// PNG. The image is transitioned from currentLayout to TRANSFER_SRC_OPTIMAL for
// the copy, then to finalLayout afterwards. Channels are swizzled to RGBA when
// the source format is BGRA. Self-contained: owns its staging buffer and a
// one-time command buffer, and blocks until the copy completes.
inline bool dump_image_to_png(VkDevice device,
                           VkPhysicalDevice physicalDevice,
                           VkCommandPool commandPool,
                           VkQueue queue,
                           VkImage srcImage,
                           VkImageLayout currentLayout,
                           VkImageLayout finalLayout,
                           VkFormat format,
                           uint32_t width,
                           uint32_t height,
                           const char* path) {
    const VkDeviceSize bufSize = static_cast<VkDeviceSize>(width) * height * 4;

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = bufSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(device, &bufInfo, nullptr, &buf) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements bufReq{};
    vkGetBufferMemoryRequirements(device, buf, &bufReq);
    VkMemoryAllocateInfo bufAlloc{};
    bufAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufAlloc.allocationSize = bufReq.size;
    bufAlloc.memoryTypeIndex =
        find_memory_type(physicalDevice, bufReq.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bufMem = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &bufAlloc, nullptr, &bufMem) != VK_SUCCESS) {
        vkDestroyBuffer(device, buf, nullptr);
        return false;
    }
    vkBindBufferMemory(device, buf, bufMem, 0);

    VkCommandBufferAllocateInfo cbAlloc{};
    cbAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAlloc.commandPool = commandPool;
    cbAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cbAlloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = currentLayout;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buf, 1, &region);

    if (finalLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        VkImageMemoryBarrier toFinal{};
        toFinal.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toFinal.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toFinal.newLayout = finalLayout;
        toFinal.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toFinal.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toFinal.image = srcImage;
        toFinal.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toFinal.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toFinal.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &toFinal);
    }

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceInfo, nullptr, &fence);
    vkQueueSubmit(queue, 1, &submit, fence);
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, commandPool, 1, &cmd);

    void* mapped = nullptr;
    vkMapMemory(device, bufMem, 0, bufSize, 0, &mapped);
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    std::vector<uint8_t> rgba(static_cast<size_t>(bufSize));
    const bool bgra = is_bgra_format(format);
    for (uint32_t i = 0; i < width * height; ++i) {
        if (bgra) {
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
    vkUnmapMemory(device, bufMem);

    const int ok = stbi_write_png(path, static_cast<int>(width),
                                  static_cast<int>(height), 4, rgba.data(),
                                  static_cast<int>(width * 4));

    vkDestroyBuffer(device, buf, nullptr);
    vkFreeMemory(device, bufMem, nullptr);

    return ok != 0;
}

} // namespace cairns::rhi
