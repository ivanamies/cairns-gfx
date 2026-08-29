// rhi/vulkan/resource_manager.cpp
//
// Vulkan implementation of cairns::rhi::ResourceManager.

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include "rhi/resource_manager.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan_beta.h>
#include <stb_image_write.h>

#include "rhi/vulkan/memory_allocator.hpp"
#include "rhi/command_recorder.hpp"
#include "rhi/vulkan/command_recorder_impl.hpp"
#include "rhi/swap_chain.hpp"
#include "util/render_pass_globals.hpp"
#include "util/material_gpu.hpp"

namespace cairns::rhi {

struct ResourceManager::Impl {
    BackendInitParams params;
    vulkan::MemoryAllocator memory;
    std::filesystem::path dump_path;
    uint32_t recorder_frame = 0;

    // Device objects owned by InitDevice.
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue compute_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaa_samples = VK_SAMPLE_COUNT_1_BIT;
    bool validation_enabled = false;

    // Per-frame command buffers + sync, owned by InitDevice.
    uint32_t frames_in_flight = 0;
    std::vector<VkCommandBuffer> graphics_cmds;
    std::vector<VkCommandBuffer> compute_cmds;
    std::vector<VkSemaphore> image_available;
    std::vector<VkSemaphore> render_finished;
    std::vector<VkSemaphore> compute_finished;
    std::vector<VkFence> in_flight;
    std::vector<VkFence> compute_in_flight;

    // Descriptor pool + non-bindless layouts/sets, owned by InitDevice.
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout dyn_ubo_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout compute_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout point_layout = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> dyn_ubo_sets;
    std::vector<VkDescriptorSet> compute_sets;
    std::vector<VkDescriptorSet> point_sets;

    Pool<Buffer> buffers;
    Pool<Texture> textures;
    Pool<Sampler> samplers;
    Pool<BindGroup> bind_groups;
    Pool<DynamicBuffers> dynamic_buffers;
    Pool<Kernel> kernels;
    Pool<Shader> shaders;

    uint32_t frame_index = 0;
    uint32_t uniform_align = 256;
    uint32_t storage_align = 256;

    // Bindless registry builder (one in-flight at a time).
    VkDescriptorSetLayout bindless_layout = VK_NULL_HANDLE;
    VkDescriptorPool bindless_pool = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set = VK_NULL_HANDLE;
    Handle<BindGroup> bindless_handle;
    uint32_t bindless_tex_binding = 0;
    uint32_t bindless_attr_binding = 0;
    uint32_t bindless_samp_binding = 0;
    std::vector<VkDescriptorImageInfo> bindless_tex_infos;
    std::vector<VkDescriptorBufferInfo> bindless_attr_infos;
    std::vector<VkDescriptorImageInfo> bindless_sampler_infos;
};

namespace {

const std::vector<const char*> kValidationLayers = {"VK_LAYER_KHRONOS_validation"};
const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME,
};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    std::cerr << "validation layer: " << data->pMessage << std::endl;
    return VK_FALSE;
}

VkResult create_debug_messenger(VkInstance instance,
                                const VkDebugUtilsMessengerCreateInfoEXT* ci,
                                VkDebugUtilsMessengerEXT* out) {
    auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");
    return fn ? fn(instance, ci, nullptr, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

void destroy_debug_messenger(VkInstance instance, VkDebugUtilsMessengerEXT m) {
    auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkDestroyDebugUtilsMessengerEXT");
    if (fn) {
        fn(instance, m, nullptr);
    }
}

void populate_debug_ci(VkDebugUtilsMessengerCreateInfoEXT& ci) {
    ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debug_callback;
}

bool check_validation_layer_support() {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());
    for (const char* name : kValidationLayers) {
        bool found = false;
        for (const auto& props : available) {
            if (strcmp(name, props.layerName) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

struct QueueFamilies {
    std::optional<uint32_t> graphics_compute;
    std::optional<uint32_t> present;
    bool complete() const {
        return graphics_compute.has_value() && present.has_value();
    }
};

QueueFamilies find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilies indices;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            indices.graphics_compute = i;
        }
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present);
        if (present) {
            indices.present = i;
        }
        if (indices.complete()) {
            break;
        }
    }
    return indices;
}

bool check_device_extension_support(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());
    std::set<std::string> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
    for (const auto& ext : available) {
        required.erase(ext.extensionName);
    }
    return required.empty();
}

bool device_swapchain_adequate(VkPhysicalDevice device, VkSurfaceKHR surface) {
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
    uint32_t present_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_count, nullptr);
    return format_count != 0 && present_count != 0;
}

VkSampleCountFlagBits max_usable_sample_count(VkPhysicalDevice device) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device, &props);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts &
                                props.limits.framebufferDepthSampleCounts;
    if (counts & VK_SAMPLE_COUNT_64_BIT) { return VK_SAMPLE_COUNT_64_BIT; }
    if (counts & VK_SAMPLE_COUNT_32_BIT) { return VK_SAMPLE_COUNT_32_BIT; }
    if (counts & VK_SAMPLE_COUNT_16_BIT) { return VK_SAMPLE_COUNT_16_BIT; }
    if (counts & VK_SAMPLE_COUNT_8_BIT) { return VK_SAMPLE_COUNT_8_BIT; }
    if (counts & VK_SAMPLE_COUNT_4_BIT) { return VK_SAMPLE_COUNT_4_BIT; }
    if (counts & VK_SAMPLE_COUNT_2_BIT) { return VK_SAMPLE_COUNT_2_BIT; }
    return VK_SAMPLE_COUNT_1_BIT;
}

bool is_device_suitable(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilies indices = find_queue_families(device, surface);
    bool extensions_ok = check_device_extension_support(device);
    bool swapchain_ok = extensions_ok && device_swapchain_adequate(device, surface);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(device, &features);
    return indices.complete() && extensions_ok && swapchain_ok &&
           features.samplerAnisotropy;
}

bool is_host_visible(Memory mem) {
    return mem == Memory::kUpload || mem == Memory::kDynamic ||
           mem == Memory::kReadback;
}

bool copy_via_staging(VkDevice device, VkCommandPool pool, VkQueue queue,
                      VkBuffer src, uint32_t src_offset, VkBuffer dst,
                      uint32_t dst_offset, uint32_t size) {
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &cai, &cmd) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkBufferCopy region{};
    region.srcOffset = src_offset;
    region.dstOffset = dst_offset;
    region.size = size;
    vkCmdCopyBuffer(cmd, src, dst, 1, &region);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, pool, 1, &cmd);
    return true;
}

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

VkImageUsageFlags to_vk_image_usage(TextureUsage u) {
    VkImageUsageFlags f = 0;
    if (u & kTexUsageSampled) {
        f |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (u & kTexUsageStorage) {
        f |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (u & kTexUsageColorTarget) {
        f |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (u & kTexUsageDepthTarget) {
        f |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (u & kTexUsageTransferSrc) {
        f |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (u & kTexUsageTransferDst) {
        f |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return f;
}

VkFilter to_vk_filter(Filter f) {
    return f == Filter::kNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode to_vk_address_mode(AddressMode m) {
    switch (m) {
        case AddressMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case AddressMode::kMirroredRepeat:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case AddressMode::kClampToEdge:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case AddressMode::kClampToBorder:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

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

void transition_to_transfer_dst(VkDevice device, VkCommandPool pool,
                                VkQueue queue, VkImage image,
                                uint32_t mip_levels) {
    VkCommandBuffer cmd = begin_single_time(device, pool);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    end_single_time(device, pool, queue, cmd);
}

void copy_buffer_to_image(VkDevice device, VkCommandPool pool, VkQueue queue,
                          VkBuffer buffer, uint32_t buffer_offset,
                          VkImage image, uint32_t width, uint32_t height) {
    VkCommandBuffer cmd = begin_single_time(device, pool);
    VkBufferImageCopy region{};
    region.bufferOffset = buffer_offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    end_single_time(device, pool, queue, cmd);
}

bool generate_mipmaps(VkDevice device, VkCommandPool pool, VkQueue queue,
                      VkPhysicalDevice physical, VkImage image, VkFormat format,
                      int32_t tex_width, int32_t tex_height,
                      uint32_t mip_levels) {
    VkFormatProperties format_properties;
    vkGetPhysicalDeviceFormatProperties(physical, format, &format_properties);
    if (!(format_properties.optimalTilingFeatures &
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        return false;
    }

    VkCommandBuffer cmd = begin_single_time(device, pool);
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mip_width = tex_width;
    int32_t mip_height = tex_height;
    for (uint32_t i = 1; i < mip_levels; ++i) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mip_width, mip_height, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {mip_width > 1 ? mip_width / 2 : 1,
                              mip_height > 1 ? mip_height / 2 : 1, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        if (mip_width > 1) {
            mip_width /= 2;
        }
        if (mip_height > 1) {
            mip_height /= 2;
        }
    }

    barrier.subresourceRange.baseMipLevel = mip_levels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    end_single_time(device, pool, queue, cmd);
    return true;
}

}  // namespace

ResourceManager::~ResourceManager() {
    Deinit();
}

void ResourceManager::Deinit() {
    if (!impl_) {
        return;
    }
    VkDevice dev = impl_->params.device;
    impl_->textures.ForEachLive([dev](Texture::Hot& hot, Texture::Cold& cold) {
        if (hot.api_view) {
            vkDestroyImageView(dev, static_cast<VkImageView>(hot.api_view), nullptr);
            hot.api_view = nullptr;
        }
        if (cold.api_image) {
            vkDestroyImage(dev, static_cast<VkImage>(cold.api_image), nullptr);
            cold.api_image = nullptr;
        }
    });
    impl_->samplers.ForEachLive([dev](Sampler::Hot& hot, Sampler::Cold&) {
        if (hot.api_sampler) {
            vkDestroySampler(dev, static_cast<VkSampler>(hot.api_sampler), nullptr);
            hot.api_sampler = nullptr;
        }
    });
    if (impl_->bindless_pool) {
        vkDestroyDescriptorPool(dev, impl_->bindless_pool, nullptr);
    }
    if (impl_->bindless_layout) {
        vkDestroyDescriptorSetLayout(dev, impl_->bindless_layout, nullptr);
    }
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
    impl_->shaders.ForEachLive([dev](Shader::Hot& hot, Shader::Cold&) {
        if (hot.vk_pipeline) {
            vkDestroyPipeline(dev, hot.vk_pipeline, nullptr);
            hot.vk_pipeline = VK_NULL_HANDLE;
        }
        if (hot.vk_layout) {
            vkDestroyPipelineLayout(dev, hot.vk_layout, nullptr);
            hot.vk_layout = VK_NULL_HANDLE;
        }
    });
    impl_->kernels.ForEachLive([dev](Kernel::Hot& hot, Kernel::Cold&) {
        if (hot.vk_pipeline) {
            vkDestroyPipeline(dev, hot.vk_pipeline, nullptr);
            hot.vk_pipeline = VK_NULL_HANDLE;
        }
        if (hot.vk_layout) {
            vkDestroyPipelineLayout(dev, hot.vk_layout, nullptr);
            hot.vk_layout = VK_NULL_HANDLE;
        }
    });
    const VkInstance inst = impl_->instance;
    const VkDebugUtilsMessengerEXT dbg = impl_->debug_messenger;
    const VkSurfaceKHR surf = impl_->surface;
    const VkCommandPool pool = impl_->params.command_pool;
    const bool validation = impl_->validation_enabled;
    delete impl_;  // memory allocator dtor frees device memory before teardown
    impl_ = nullptr;
    if (pool) {
        vkDestroyCommandPool(dev, pool, nullptr);
    }
    if (dev) {
        vkDestroyDevice(dev, nullptr);
    }
    if (validation && dbg) {
        destroy_debug_messenger(inst, dbg);
    }
    if (surf) {
        vkDestroySurfaceKHR(inst, surf, nullptr);
    }
    if (inst) {
        vkDestroyInstance(inst, nullptr);
    }
}

bool ResourceManager::Init(const BackendInitParams& params) {
    impl_ = new Impl();
    impl_->params = params;
    if (!impl_->memory.Init(params.device, params.physical, params.enable_bda)) {
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(params.physical, &props);
    impl_->uniform_align =
        static_cast<uint32_t>(props.limits.minUniformBufferOffsetAlignment);
    impl_->storage_align =
        static_cast<uint32_t>(props.limits.minStorageBufferOffsetAlignment);
    if (impl_->uniform_align == 0) {
        impl_->uniform_align = 1;
    }
    if (impl_->storage_align == 0) {
        impl_->storage_align = 1;
    }
    return true;
}

bool ResourceManager::InitDevice(SDL_Window* window) {
    impl_ = new Impl();

#ifdef NDEBUG
    impl_->validation_enabled = false;
#else
    impl_->validation_enabled = true;
#endif
    if (impl_->validation_enabled && !check_validation_layer_support()) {
        impl_->validation_enabled = false;
    }

    {  // instance
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "cairns";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "cairns";
        app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app.apiVersion = VK_API_VERSION_1_2;

        uint32_t sdl_count = 0;
        const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_count);
        std::vector<const char*> extensions(sdl_exts, sdl_exts + sdl_count);
        if (impl_->validation_enabled) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        ci.ppEnabledExtensionNames = extensions.data();
        VkDebugUtilsMessengerCreateInfoEXT dbg{};
        if (impl_->validation_enabled) {
            ci.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
            ci.ppEnabledLayerNames = kValidationLayers.data();
            populate_debug_ci(dbg);
            ci.pNext = &dbg;
        }
        if (vkCreateInstance(&ci, nullptr, &impl_->instance) != VK_SUCCESS) {
            return false;
        }
    }

    if (impl_->validation_enabled) {  // debug messenger
        VkDebugUtilsMessengerCreateInfoEXT ci{};
        populate_debug_ci(ci);
        create_debug_messenger(impl_->instance, &ci, &impl_->debug_messenger);
    }

    if (!SDL_Vulkan_CreateSurface(window, impl_->instance, nullptr,
                                  &impl_->surface)) {
        return false;
    }

    {  // physical device
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(impl_->instance, &count, nullptr);
        if (count == 0) {
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(impl_->instance, &count, devices.data());
        for (VkPhysicalDevice d : devices) {
            if (is_device_suitable(d, impl_->surface)) {
                impl_->params.physical = d;
                impl_->msaa_samples = max_usable_sample_count(d);
                break;
            }
        }
        if (impl_->params.physical == VK_NULL_HANDLE) {
            return false;
        }
    }

    QueueFamilies indices =
        find_queue_families(impl_->params.physical, impl_->surface);

    {  // logical device + queues
        std::set<uint32_t> unique = {indices.graphics_compute.value(),
                                     indices.present.value()};
        std::vector<VkDeviceQueueCreateInfo> queue_cis;
        const float priority = 1.0f;
        for (uint32_t fam : unique) {
            VkDeviceQueueCreateInfo qci{};
            qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qci.queueFamilyIndex = fam;
            qci.queueCount = 1;
            qci.pQueuePriorities = &priority;
            queue_cis.push_back(qci);
        }

        VkPhysicalDeviceVulkan12Features vk12{};
        vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vk12.runtimeDescriptorArray = VK_TRUE;
        vk12.descriptorBindingPartiallyBound = VK_TRUE;
        vk12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        vk12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        vk12.descriptorBindingVariableDescriptorCount = VK_TRUE;
        vk12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        vk12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        vk12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.features.samplerAnisotropy = VK_TRUE;
        features2.pNext = &vk12;

        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = static_cast<uint32_t>(queue_cis.size());
        ci.pQueueCreateInfos = queue_cis.data();
        ci.pNext = &features2;
        ci.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
        ci.ppEnabledExtensionNames = kDeviceExtensions.data();
        if (impl_->validation_enabled) {
            ci.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
            ci.ppEnabledLayerNames = kValidationLayers.data();
        }
        if (vkCreateDevice(impl_->params.physical, &ci, nullptr,
                           &impl_->params.device) != VK_SUCCESS) {
            return false;
        }
        vkGetDeviceQueue(impl_->params.device, indices.graphics_compute.value(), 0,
                         &impl_->graphics_queue);
        vkGetDeviceQueue(impl_->params.device, indices.present.value(), 0,
                         &impl_->present_queue);
        vkGetDeviceQueue(impl_->params.device, indices.graphics_compute.value(), 0,
                         &impl_->compute_queue);
        impl_->params.queue = impl_->graphics_queue;
        impl_->params.queue_family_index = indices.graphics_compute.value();
    }

    {  // command pool
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = indices.graphics_compute.value();
        if (vkCreateCommandPool(impl_->params.device, &ci, nullptr,
                                &impl_->params.command_pool) != VK_SUCCESS) {
            return false;
        }
    }

    if (!impl_->memory.Init(impl_->params.device, impl_->params.physical,
                            impl_->params.enable_bda)) {
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(impl_->params.physical, &props);
    impl_->uniform_align = std::max(
        1u, static_cast<uint32_t>(props.limits.minUniformBufferOffsetAlignment));
    impl_->storage_align = std::max(
        1u, static_cast<uint32_t>(props.limits.minStorageBufferOffsetAlignment));

    {  // per-frame command buffers + sync
        const uint32_t n = kFramesInFlight;
        impl_->frames_in_flight = n;
        impl_->graphics_cmds.resize(n);
        impl_->compute_cmds.resize(n);
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = impl_->params.command_pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = n;
        if (vkAllocateCommandBuffers(impl_->params.device, &cai,
                                     impl_->graphics_cmds.data()) != VK_SUCCESS ||
            vkAllocateCommandBuffers(impl_->params.device, &cai,
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
            if (vkCreateSemaphore(impl_->params.device, &sci, nullptr,
                                  &impl_->image_available[i]) != VK_SUCCESS ||
                vkCreateSemaphore(impl_->params.device, &sci, nullptr,
                                  &impl_->render_finished[i]) != VK_SUCCESS ||
                vkCreateSemaphore(impl_->params.device, &sci, nullptr,
                                  &impl_->compute_finished[i]) != VK_SUCCESS ||
                vkCreateFence(impl_->params.device, &fci, nullptr,
                              &impl_->in_flight[i]) != VK_SUCCESS ||
                vkCreateFence(impl_->params.device, &fci, nullptr,
                              &impl_->compute_in_flight[i]) != VK_SUCCESS) {
                return false;
            }
        }
    }

    {  // descriptor layouts + pool + per-frame sets (non-bindless)
        VkDevice dev = impl_->params.device;
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

VkDescriptorSetLayout ResourceManager::GetDynUboLayout() const {
    return impl_->dyn_ubo_layout;
}
VkDescriptorSetLayout ResourceManager::GetComputeLayout() const {
    return impl_->compute_layout;
}
VkDescriptorSetLayout ResourceManager::GetPointLayout() const {
    return impl_->point_layout;
}

VkInstance ResourceManager::GetVkInstance() const { return impl_->instance; }
VkSurfaceKHR ResourceManager::GetVkSurface() const { return impl_->surface; }
VkPhysicalDevice ResourceManager::GetVkPhysicalDevice() const {
    return impl_->params.physical;
}
VkDevice ResourceManager::GetVkDevice() const { return impl_->params.device; }
VkQueue ResourceManager::GetVkGraphicsQueue() const {
    return impl_->graphics_queue;
}
VkQueue ResourceManager::GetVkComputeQueue() const { return impl_->compute_queue; }
VkQueue ResourceManager::GetVkPresentQueue() const { return impl_->present_queue; }
VkCommandPool ResourceManager::GetVkCommandPool() const {
    return impl_->params.command_pool;
}
VkSampleCountFlagBits ResourceManager::GetVkMsaaSamples() const {
    return impl_->msaa_samples;
}

bool ResourceManager::InitSwapChain(SwapChain& sc, SDL_Window* window) {
    return sc.Init(impl_->params.device, impl_->params.physical, impl_->surface,
                   window, impl_->params.command_pool, impl_->graphics_queue,
                   impl_->msaa_samples, true);
}

bool ResourceManager::InitFrameTargets(SwapChain& sc) {
    (void)sc;  // depth/MSAA/render-pass already created in sc.Init.
    return true;
}

uint32_t ResourceManager::UboAlign() const { return impl_->uniform_align; }

Handle<Buffer> ResourceManager::CreateBuffer(const BufferDesc& d) {
    uint32_t align = 16;
    if (d.usage & kUsageUniform) {
        align = std::max(align, impl_->uniform_align);
    }
    if (d.usage & kUsageStorage) {
        align = std::max(align, impl_->storage_align);
    }

    vulkan::AllocResult r =
        impl_->memory.AllocBuffer(d.byte_size, d.usage, d.memory, align);
    if (!r.ok) {
        return Handle<Buffer>::Null;
    }

    Handle<Buffer> h = impl_->buffers.Acquire();
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    hot->heap_buffer_index = static_cast<uint16_t>(r.heap_index);
    hot->pad = 0;
    hot->offset_in_heap = r.offset;

    Buffer::Cold* cold = impl_->buffers.GetCold(h);
    cold->alloc = r.alloc;
    cold->size_bytes = d.byte_size;
    cold->usage = d.usage;
    cold->mem_type = d.memory;
    cold->debug_name = d.debug_name;

    if (!d.initial_data.empty()) {
        if (is_host_visible(d.memory)) {
            uint8_t* dst = MappedPtr(h);
            if (dst) {
                std::memcpy(dst, d.initial_data.data(), d.initial_data.size());
            }
        } else {
            uint32_t saved_cursor =
                impl_->memory.BumpSaveCursor(Memory::kUpload);
            void* staging = impl_->memory.BumpAllocate(
                static_cast<uint32_t>(d.initial_data.size()), 16,
                Memory::kUpload);
            if (staging) {
                std::memcpy(staging, d.initial_data.data(),
                            d.initial_data.size());
                uint32_t src_off = impl_->memory.BumpOffset(staging);
                uint32_t src_hi =
                    impl_->memory.BumpMasterHeapIndex(Memory::kUpload);
                VkBuffer src = impl_->memory.HeapMasterBuffer(src_hi);
                VkBuffer dst = impl_->memory.HeapMasterBuffer(r.heap_index);
                copy_via_staging(
                    impl_->params.device, impl_->params.command_pool,
                    impl_->params.queue, src, src_off, dst, r.offset,
                    static_cast<uint32_t>(d.initial_data.size()));
            }
            impl_->memory.BumpRestoreCursor(Memory::kUpload, saved_cursor);
        }
    }
    return h;
}

Handle<Texture> ResourceManager::CreateTexture(const TextureDesc& d) {
    VkFormat vk_format = to_vk_format(d.format);
    VkImageUsageFlags usage = to_vk_image_usage(d.usage);
    if (!d.initial_data.empty() || d.mip_levels > 1) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.extent.width = static_cast<uint32_t>(d.dimensions.x);
    ici.extent.height = static_cast<uint32_t>(d.dimensions.y);
    ici.extent.depth = 1;
    ici.mipLevels = d.mip_levels;
    ici.arrayLayers = d.array_layers;
    ici.format = vk_format;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ici.usage = usage;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(impl_->params.device, &ici, nullptr, &image) !=
        VK_SUCCESS) {
        return Handle<Texture>::Null;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(impl_->params.device, image, &req);
    vulkan::AllocResult r = impl_->memory.AllocImage(
        static_cast<uint32_t>(req.size), static_cast<uint32_t>(req.alignment),
        req.memoryTypeBits, Memory::kDefault);
    if (!r.ok) {
        vkDestroyImage(impl_->params.device, image, nullptr);
        return Handle<Texture>::Null;
    }
    vkBindImageMemory(impl_->params.device, image,
                      impl_->memory.HeapDeviceMemory(r.heap_index), r.offset);

    if (!d.initial_data.empty()) {
        uint32_t saved_cursor = impl_->memory.BumpSaveCursor(Memory::kUpload);
        void* staging = impl_->memory.BumpAllocate(
            static_cast<uint32_t>(d.initial_data.size()), 16, Memory::kUpload);
        if (staging) {
            std::memcpy(staging, d.initial_data.data(), d.initial_data.size());
            uint32_t src_off = impl_->memory.BumpOffset(staging);
            uint32_t src_hi = impl_->memory.BumpMasterHeapIndex(Memory::kUpload);
            VkBuffer src = impl_->memory.HeapMasterBuffer(src_hi);
            transition_to_transfer_dst(impl_->params.device,
                                       impl_->params.command_pool,
                                       impl_->params.queue, image, d.mip_levels);
            copy_buffer_to_image(impl_->params.device,
                                 impl_->params.command_pool, impl_->params.queue,
                                 src, src_off, image,
                                 static_cast<uint32_t>(d.dimensions.x),
                                 static_cast<uint32_t>(d.dimensions.y));
            generate_mipmaps(impl_->params.device, impl_->params.command_pool,
                             impl_->params.queue, impl_->params.physical, image,
                             vk_format, d.dimensions.x, d.dimensions.y,
                             d.mip_levels);
        }
        impl_->memory.BumpRestoreCursor(Memory::kUpload, saved_cursor);
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = vk_format;
    vci.subresourceRange.aspectMask = (d.usage & kTexUsageDepthTarget)
                                          ? VK_IMAGE_ASPECT_DEPTH_BIT
                                          : VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = d.mip_levels;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = d.array_layers;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(impl_->params.device, &vci, nullptr, &view) !=
        VK_SUCCESS) {
        vkDestroyImage(impl_->params.device, image, nullptr);
        return Handle<Texture>::Null;
    }

    Handle<Texture> h = impl_->textures.Acquire();
    Texture::Hot* hot = impl_->textures.GetHot(h);
    hot->api_view = view;
    hot->descriptor_index = 0;

    Texture::Cold* cold = impl_->textures.GetCold(h);
    cold->alloc = r.alloc;
    cold->api_image = image;
    cold->width = static_cast<uint32_t>(d.dimensions.x);
    cold->height = static_cast<uint32_t>(d.dimensions.y);
    cold->depth = 1;
    cold->mip_levels = d.mip_levels;
    cold->array_layers = d.array_layers;
    cold->format = d.format;
    cold->usage = d.usage;
    cold->mem_type = Memory::kDefault;
    cold->heap_buffer_index = r.heap_index;
    cold->debug_name = d.debug_name;
    return h;
}

Handle<Sampler> ResourceManager::CreateSampler(const SamplerDesc& d) {
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = to_vk_filter(d.mag_filter);
    sci.minFilter = to_vk_filter(d.min_filter);
    sci.mipmapMode = d.mip_filter == Filter::kNearest
                         ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                         : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = to_vk_address_mode(d.address_mode);
    sci.addressModeV = to_vk_address_mode(d.address_mode);
    sci.addressModeW = to_vk_address_mode(d.address_mode);
    sci.anisotropyEnable = d.max_anisotropy > 0.0f ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = d.max_anisotropy;
    sci.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sci.unnormalizedCoordinates = VK_FALSE;
    sci.compareEnable = VK_FALSE;
    sci.compareOp = VK_COMPARE_OP_ALWAYS;
    sci.mipLodBias = 0.0f;
    sci.minLod = 0.0f;
    sci.maxLod = d.max_lod;

    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(impl_->params.device, &sci, nullptr, &sampler) !=
        VK_SUCCESS) {
        return Handle<Sampler>::Null;
    }

    Handle<Sampler> h = impl_->samplers.Acquire();
    impl_->samplers.GetHot(h)->api_sampler = sampler;
    impl_->samplers.GetCold(h)->debug_name = d.debug_name;
    return h;
}

Handle<BindGroup> ResourceManager::CreateBindGroup(const BindGroupDesc&) {
    return Handle<BindGroup>::Null;
}

Handle<BindGroup> ResourceManager::CreateBindGroupFromVkDescriptorSet(
    VkDescriptorSet set) {
    Handle<BindGroup> h = impl_->bind_groups.Acquire();
    impl_->bind_groups.GetHot(h)->api_descriptor_set = set;
    return h;
}

Handle<Shader> ResourceManager::CreateShader(const ShaderDesc& desc) {
    Handle<Shader> h = impl_->shaders.Acquire();
    Shader::Hot* hot = impl_->shaders.GetHot(h);
    hot->vk_pipeline = desc.vk_pipeline;
    hot->vk_layout = desc.vk_layout;
    impl_->shaders.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

void ResourceManager::Destroy(Handle<Shader> h) {
    impl_->shaders.Release(h);
}

Shader::Hot* ResourceManager::GetHot(Handle<Shader> h) {
    return impl_->shaders.GetHot(h);
}

Handle<DynamicBuffers> ResourceManager::CreateDynamicBuffers(
    const DynamicBuffersDesc&) {
    return Handle<DynamicBuffers>::Null;
}

void ResourceManager::Destroy(Handle<Buffer> h) {
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    Buffer::Cold* cold = impl_->buffers.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    impl_->memory.FreeBuffer(hot->heap_buffer_index, cold->alloc,
                             impl_->frame_index + kFramesInFlight);
    impl_->buffers.Release(h);
}

void ResourceManager::Destroy(Handle<Texture> h) {
    Texture::Hot* hot = impl_->textures.GetHot(h);
    Texture::Cold* cold = impl_->textures.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    impl_->memory.FreeImage(cold->heap_buffer_index, cold->alloc,
                            static_cast<VkImage>(cold->api_image),
                            static_cast<VkImageView>(hot->api_view),
                            impl_->frame_index + kFramesInFlight);
    impl_->textures.Release(h);
}

void ResourceManager::Destroy(Handle<Sampler> h) {
    Sampler::Hot* hot = impl_->samplers.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_sampler) {
        vkDestroySampler(impl_->params.device,
                         static_cast<VkSampler>(hot->api_sampler), nullptr);
    }
    impl_->samplers.Release(h);
}

void ResourceManager::Destroy(Handle<BindGroup> h) {
    impl_->bind_groups.Release(h);
}

void ResourceManager::Destroy(Handle<DynamicBuffers> h) {
    impl_->dynamic_buffers.Release(h);
}

Handle<Kernel> ResourceManager::CreateKernel(const KernelDesc& desc) {
    Handle<Kernel> h = impl_->kernels.Acquire();
    Kernel::Hot* hot = impl_->kernels.GetHot(h);
    hot->vk_pipeline = desc.vk_pipeline;
    hot->vk_layout = desc.vk_layout;
    impl_->kernels.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

void ResourceManager::Destroy(Handle<Kernel> h) {
    impl_->kernels.Release(h);
}

namespace {

bool read_spv_file(const std::string& path, std::vector<char>* out) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "rhi/vk: failed to open shader: " << path << std::endl;
        return false;
    }
    const size_t size = static_cast<size_t>(file.tellg());
    out->resize(size);
    file.seekg(0);
    file.read(out->data(), size);
    file.close();
    return true;
}

VkShaderModule make_shader_module(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return m;
}

VkPrimitiveTopology to_vk_topology(PrimitiveTopology t) {
    return t == PrimitiveTopology::kPointList ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                              : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags to_vk_cull(CullMode c) {
    switch (c) {
        case CullMode::kBack:  return VK_CULL_MODE_BACK_BIT;
        case CullMode::kFront: return VK_CULL_MODE_FRONT_BIT;
        default:               return VK_CULL_MODE_NONE;
    }
}

VkFrontFace to_vk_front_face(FrontFace f) {
    return f == FrontFace::kClockwise ? VK_FRONT_FACE_CLOCKWISE
                                      : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkCompareOp to_vk_compare_op(CompareOp c) {
    switch (c) {
        case CompareOp::kNever:        return VK_COMPARE_OP_NEVER;
        case CompareOp::kLess:         return VK_COMPARE_OP_LESS;
        case CompareOp::kEqual:        return VK_COMPARE_OP_EQUAL;
        case CompareOp::kLessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::kGreater:      return VK_COMPARE_OP_GREATER;
        case CompareOp::kNotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::kGreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::kAlways:       return VK_COMPARE_OP_ALWAYS;
        default:                       return VK_COMPARE_OP_LESS;
    }
}

VkBlendFactor to_vk_blend_factor(BlendFactor b) {
    switch (b) {
        case BlendFactor::kZero:             return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::kOne:              return VK_BLEND_FACTOR_ONE;
        case BlendFactor::kSrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::kOneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        default:                             return VK_BLEND_FACTOR_ONE;
    }
}

VkSampleCountFlagBits to_vk_samples(uint32_t n) {
    switch (n) {
        case 1:  return VK_SAMPLE_COUNT_1_BIT;
        case 2:  return VK_SAMPLE_COUNT_2_BIT;
        case 4:  return VK_SAMPLE_COUNT_4_BIT;
        case 8:  return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        case 32: return VK_SAMPLE_COUNT_32_BIT;
        case 64: return VK_SAMPLE_COUNT_64_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

struct VkShaderFiles {
    const char* vert;
    const char* frag;
    const char* comp;
};
VkShaderFiles resolve_vk_shader(const char* logical) {
    if (std::strcmp(logical, "unlit") == 0) {
        return {"unlit.vert.spv", "unlit.frag.spv", nullptr};
    }
    // "particle"
    return {"particle.vert.spv", "particle.frag.spv", "particle.comp.spv"};
}

}  // namespace

Handle<Shader> ResourceManager::CreateGraphicsPipeline(
    const GraphicsPipelineDesc& desc) {
    VkDevice device = impl_->params.device;
    const VkShaderFiles files = resolve_vk_shader(desc.logical_shader);
    const std::filesystem::path dir = desc.shader_dir ? desc.shader_dir : "";

    std::vector<char> vert_code;
    std::vector<char> frag_code;
    if (!read_spv_file((dir / files.vert).string(), &vert_code) ||
        !read_spv_file((dir / files.frag).string(), &frag_code)) {
        return Handle<Shader>::Null;
    }
    VkShaderModule vert_mod = make_shader_module(device, vert_code);
    VkShaderModule frag_mod = make_shader_module(device, frag_code);
    if (!vert_mod || !frag_mod) {
        return Handle<Shader>::Null;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    std::vector<VkVertexInputBindingDescription> bindings;
    for (size_t i = 0; i < desc.vertex_buffers.size(); ++i) {
        VkVertexInputBindingDescription b{};
        b.binding = desc.vertex_buffers[i].buffer_slot;
        b.stride = desc.vertex_buffers[i].stride;
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(b);
    }
    std::vector<VkVertexInputAttributeDescription> attrs;
    for (size_t i = 0; i < desc.vertex_attributes.size(); ++i) {
        const VertexInputAttribute& a = desc.vertex_attributes[i];
        VkVertexInputAttributeDescription va{};
        va.location = a.location;
        va.binding = a.buffer_slot;
        va.format = to_vk_format(a.format);
        va.offset = a.offset;
        attrs.push_back(va);
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertex_input.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = to_vk_topology(desc.topology);
    input_assembly.primitiveRestartEnable = VK_FALSE;

    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dyn_states;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = to_vk_cull(desc.cull);
    rasterizer.frontFace = to_vk_front_face(desc.front_face);
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = to_vk_samples(desc.sample_count);
    multisampling.minSampleShading = 1.0f;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = desc.blend.enable ? VK_TRUE : VK_FALSE;
    blend_attachment.srcColorBlendFactor = to_vk_blend_factor(desc.blend.src_color);
    blend_attachment.dstColorBlendFactor = to_vk_blend_factor(desc.blend.dst_color);
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = to_vk_blend_factor(desc.blend.src_alpha);
    blend_attachment.dstAlphaBlendFactor = to_vk_blend_factor(desc.blend.dst_alpha);
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.logicOp = VK_LOGIC_OP_COPY;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend_attachment;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = to_vk_compare_op(desc.depth_compare);
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.minDepthBounds = 0.0f;
    depth_stencil.maxDepthBounds = 1.0f;
    depth_stencil.stencilTestEnable = VK_FALSE;

    VkPushConstantRange pc_range{};
    pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc_range.offset = 0;
    pc_range.size = desc.push_constant_bytes;
    std::vector<VkDescriptorSetLayout> set_layouts;
    if (desc.logical_shader && std::string(desc.logical_shader) == "unlit") {
        set_layouts = {impl_->bindless_layout, impl_->dyn_ubo_layout};
    } else {
        set_layouts = {impl_->point_layout};
    }
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = desc.push_constant_bytes ? 1 : 0;
    layout_info.pPushConstantRanges = desc.push_constant_bytes ? &pc_range : nullptr;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert_mod, nullptr);
        vkDestroyShaderModule(device, frag_mod, nullptr);
        return Handle<Shader>::Null;
    }

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vertex_input;
    pi.pInputAssemblyState = &input_assembly;
    pi.pViewportState = &viewport_state;
    pi.pRasterizationState = &rasterizer;
    pi.pMultisampleState = &multisampling;
    pi.pColorBlendState = &color_blending;
    pi.pDynamicState = &dynamic_state;
    pi.pDepthStencilState = &depth_stencil;
    pi.layout = layout;
    pi.renderPass = desc.swap_chain->renderPass;
    pi.subpass = 0;
    pi.basePipelineHandle = VK_NULL_HANDLE;
    pi.basePipelineIndex = -1;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
    vkDestroyShaderModule(device, vert_mod, nullptr);
    vkDestroyShaderModule(device, frag_mod, nullptr);
    if (res != VK_SUCCESS) {
        vkDestroyPipelineLayout(device, layout, nullptr);
        return Handle<Shader>::Null;
    }

    Handle<Shader> h = impl_->shaders.Acquire();
    Shader::Hot* hot = impl_->shaders.GetHot(h);
    hot->vk_pipeline = pipeline;
    hot->vk_layout = layout;
    impl_->shaders.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Handle<Kernel> ResourceManager::CreateComputePipeline(
    const ComputePipelineDesc& desc) {
    VkDevice device = impl_->params.device;
    const VkShaderFiles files = resolve_vk_shader(desc.logical_shader);
    const std::filesystem::path dir = desc.shader_dir ? desc.shader_dir : "";

    std::vector<char> comp_code;
    if (!read_spv_file((dir / files.comp).string(), &comp_code)) {
        return Handle<Kernel>::Null;
    }
    VkShaderModule comp_mod = make_shader_module(device, comp_code);
    if (!comp_mod) {
        return Handle<Kernel>::Null;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = comp_mod;
    stage.pName = "main";

    const VkDescriptorSetLayout compute_layouts[1] = {impl_->compute_layout};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = compute_layouts;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, comp_mod, nullptr);
        return Handle<Kernel>::Null;
    }

    VkComputePipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage = stage;
    pi.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
    vkDestroyShaderModule(device, comp_mod, nullptr);
    if (res != VK_SUCCESS) {
        vkDestroyPipelineLayout(device, layout, nullptr);
        return Handle<Kernel>::Null;
    }

    Handle<Kernel> h = impl_->kernels.Acquire();
    Kernel::Hot* hot = impl_->kernels.GetHot(h);
    hot->vk_pipeline = pipeline;
    hot->vk_layout = layout;
    impl_->kernels.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Kernel::Hot* ResourceManager::GetHot(Handle<Kernel> h) {
    return impl_->kernels.GetHot(h);
}

Handle<BindGroup> ResourceManager::CreateBindlessRegistry(
    const BindlessRegistryDesc& desc) {
    VkDevice device = impl_->params.device;

    VkDescriptorBindingFlags binding_flags[3] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
    flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_info.bindingCount = 3;
    flags_info.pBindingFlags = binding_flags;

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = desc.texture_slot;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = desc.max_textures;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = desc.attr_buffer_slot;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = desc.max_attr_buffers;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = desc.sampler_slot;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[2].descriptorCount = desc.max_samplers;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    layout_info.pNext = &flags_info;
    if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr,
                                    &impl_->bindless_layout) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }

    VkDescriptorPoolSize pool_sizes[3]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[0].descriptorCount = desc.max_textures;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = desc.max_attr_buffers;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_sizes[2].descriptorCount = desc.max_samplers;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(device, &pool_info, nullptr,
                               &impl_->bindless_pool) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = impl_->bindless_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &impl_->bindless_layout;
    if (vkAllocateDescriptorSets(device, &alloc_info,
                                 &impl_->bindless_set) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }

    impl_->bindless_tex_binding = desc.texture_slot;
    impl_->bindless_attr_binding = desc.attr_buffer_slot;
    impl_->bindless_samp_binding = desc.sampler_slot;
    impl_->bindless_tex_infos.clear();
    impl_->bindless_attr_infos.clear();
    impl_->bindless_sampler_infos.clear();

    Handle<BindGroup> h = impl_->bind_groups.Acquire();
    impl_->bind_groups.GetHot(h)->api_descriptor_set = impl_->bindless_set;
    impl_->bind_groups.GetCold(h)->debug_name = desc.debug_name;
    impl_->bindless_handle = h;
    return h;
}

uint32_t ResourceManager::BindlessAddTexture(Handle<BindGroup>, Handle<Texture> tex) {
    Texture::Hot* hot = impl_->textures.GetHot(tex);
    VkDescriptorImageInfo img{};
    img.imageView = static_cast<VkImageView>(hot->api_view);
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    impl_->bindless_tex_infos.push_back(img);
    return static_cast<uint32_t>(impl_->bindless_tex_infos.size()) - 1;
}

uint32_t ResourceManager::BindlessAddAttrBuffer(Handle<BindGroup>, Handle<Buffer> buf) {
    uint32_t off = 0;
    VkBuffer vk = GetVkBuffer(buf, &off);
    VkDescriptorBufferInfo info{};
    info.buffer = vk;
    info.offset = off;
    info.range = GetBufferByteSize(buf);
    impl_->bindless_attr_infos.push_back(info);
    return static_cast<uint32_t>(impl_->bindless_attr_infos.size()) - 1;
}

uint32_t ResourceManager::BindlessAddSampler(Handle<BindGroup>, Handle<Sampler> samp) {
    Sampler::Hot* hot = impl_->samplers.GetHot(samp);
    VkDescriptorImageInfo info{};
    info.sampler = static_cast<VkSampler>(hot->api_sampler);
    impl_->bindless_sampler_infos.push_back(info);
    return static_cast<uint32_t>(impl_->bindless_sampler_infos.size()) - 1;
}

void ResourceManager::BindlessFinalize(Handle<BindGroup>) {
    std::vector<VkWriteDescriptorSet> writes;
    if (!impl_->bindless_tex_infos.empty()) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = impl_->bindless_set;
        w.dstBinding = impl_->bindless_tex_binding;
        w.dstArrayElement = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.descriptorCount = static_cast<uint32_t>(impl_->bindless_tex_infos.size());
        w.pImageInfo = impl_->bindless_tex_infos.data();
        writes.push_back(w);
    }
    if (!impl_->bindless_attr_infos.empty()) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = impl_->bindless_set;
        w.dstBinding = impl_->bindless_attr_binding;
        w.dstArrayElement = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = static_cast<uint32_t>(impl_->bindless_attr_infos.size());
        w.pBufferInfo = impl_->bindless_attr_infos.data();
        writes.push_back(w);
    }
    if (!impl_->bindless_sampler_infos.empty()) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = impl_->bindless_set;
        w.dstBinding = impl_->bindless_samp_binding;
        w.dstArrayElement = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.descriptorCount = static_cast<uint32_t>(impl_->bindless_sampler_infos.size());
        w.pImageInfo = impl_->bindless_sampler_infos.data();
        writes.push_back(w);
    }
    if (!writes.empty()) {
        vkUpdateDescriptorSets(impl_->params.device,
                               static_cast<uint32_t>(writes.size()), writes.data(),
                               0, nullptr);
    }
}

VkDescriptorSetLayout ResourceManager::GetBindlessLayout(Handle<BindGroup>) {
    return impl_->bindless_layout;
}

Buffer::Hot* ResourceManager::GetHot(Handle<Buffer> h) {
    return impl_->buffers.GetHot(h);
}

Texture::Hot* ResourceManager::GetHot(Handle<Texture> h) {
    return impl_->textures.GetHot(h);
}

Sampler::Hot* ResourceManager::GetHot(Handle<Sampler> h) {
    return impl_->samplers.GetHot(h);
}

BindGroup::Hot* ResourceManager::GetHot(Handle<BindGroup> h) {
    return impl_->bind_groups.GetHot(h);
}

DynamicBuffers::Hot* ResourceManager::GetHot(Handle<DynamicBuffers> h) {
    return impl_->dynamic_buffers.GetHot(h);
}

void* ResourceManager::BumpAllocate(uint32_t bytes, uint32_t align, Memory mem) {
    return impl_->memory.BumpAllocate(bytes, align, mem);
}

uint32_t ResourceManager::BumpOffset(void* ptr) const {
    return impl_->memory.BumpOffset(ptr);
}

Handle<Buffer> ResourceManager::BumpMasterBuffer(Memory mem) const {
    Handle<Buffer> h;
    h.index = static_cast<uint16_t>(impl_->memory.BumpMasterHeapIndex(mem));
    h.generation = 0;
    return h;
}

void ResourceManager::BeginFrame() {
    impl_->frame_index++;
    uint32_t slot = impl_->frame_index % kFramesInFlight;
    impl_->memory.RetireFrame(slot);
    impl_->memory.BeginFrame(impl_->frame_index);
}

void ResourceManager::EndFrame() {}

uint32_t ResourceManager::GetBufferByteSize(Handle<Buffer> h) const {
    Buffer::Cold* cold = impl_->buffers.GetCold(h);
    if (!cold) {
        return 0;
    }
    return cold->size_bytes;
}

VkBuffer ResourceManager::GetVkBumpMasterBuffer(Memory mem) {
    void* p = BumpAllocate(1, 1, mem);
    (void)p;
    uint32_t hi = impl_->memory.BumpMasterHeapIndex(mem);
    return impl_->memory.HeapMasterBuffer(hi);
}

uint32_t ResourceManager::BufferBaseOffset(Handle<Buffer> h) {
    uint32_t off = 0;
    GetVkBuffer(h, &off);
    return off;
}

VkBuffer ResourceManager::GetVkBuffer(Handle<Buffer> h, uint32_t* out_offset) {
    if (h.generation == 0) {
        if (out_offset) {
            *out_offset = 0;
        }
        return impl_->memory.HeapMasterBuffer(h.index);
    }
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    if (!hot) {
        if (out_offset) {
            *out_offset = 0;
        }
        return VK_NULL_HANDLE;
    }
    if (out_offset) {
        *out_offset = hot->offset_in_heap;
    }
    return impl_->memory.HeapMasterBuffer(hot->heap_buffer_index);
}

uint8_t* ResourceManager::MappedPtr(Handle<Buffer> h) {
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    if (!hot) {
        return nullptr;
    }
    uint8_t* base =
        static_cast<uint8_t*>(impl_->memory.HeapMappedPtr(hot->heap_buffer_index));
    if (!base) {
        return nullptr;
    }
    return base + hot->offset_in_heap;
}

void ResourceManager::SetDumpPath(const std::filesystem::path& path) {
    impl_->dump_path = path;
}

FrameContext ResourceManager::BeginFrame(SwapChain& sc) {
    const uint32_t cf = impl_->recorder_frame;
    VkDevice dev = impl_->params.device;

    vkWaitForFences(dev, 1, &impl_->compute_in_flight[cf], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &impl_->compute_in_flight[cf]);
    vkResetCommandBuffer(impl_->compute_cmds[cf], 0);

    vkWaitForFences(dev, 1, &impl_->in_flight[cf], VK_TRUE, UINT64_MAX);
    BeginFrame();  // bump ring reset

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
        this, &sc, cf, image_index, impl_->graphics_cmds[cf],
        impl_->compute_cmds[cf], dev, impl_->dyn_ubo_sets[cf],
        impl_->compute_sets[cf], impl_->point_sets[cf]};
    return fc;
}

void ResourceManager::EndFrame(FrameContext& fc) {
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
        dump_swapchain_image(impl_->params.device, impl_->params.physical,
                             impl_->params.command_pool, impl_->params.queue,
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
