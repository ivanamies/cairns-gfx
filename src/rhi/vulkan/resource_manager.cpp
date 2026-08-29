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
#include <string>
#include <vector>

#include "rhi/vulkan/memory_allocator.hpp"
#include "rhi/command_recorder.hpp"
#include "rhi/swap_chain.hpp"
#include "util/render_pass_globals.hpp"
#include "util/material_gpu.hpp"

namespace cairns::rhi {

struct ResourceManager::Impl {
    BackendInitParams params;
    vulkan::MemoryAllocator memory;
    VkFrameResources frame_res;
    uint32_t recorder_frame = 0;

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
    delete impl_;
    impl_ = nullptr;
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
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(desc.set_layouts.size());
    layout_info.pSetLayouts = desc.set_layouts.data();
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
    pi.renderPass = desc.render_pass;
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

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(desc.set_layouts.size());
    layout_info.pSetLayouts = desc.set_layouts.data();

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

VkBuffer ResourceManager::GetVkBuffer(Handle<Buffer> h, uint32_t* out_offset) {
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

void ResourceManager::VkRegisterFrame(const VkFrameResources& res) {
    impl_->frame_res = res;
}

struct CommandRecorder::Impl {
    ResourceManager* rm = nullptr;
    SwapChain* sc = nullptr;
    VkFrameResources fr;
    uint32_t frame = 0;
    uint32_t image_index = 0;
    VkCommandBuffer gfx = VK_NULL_HANDLE;
    VkCommandBuffer comp = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
};

void CommandRecorder::Dispatch(const ComputeDispatch& d) {
    Kernel::Hot* k = impl_->rm->GetHot(d.kernel);
    VkDescriptorSet set = impl_->fr.compute_sets[impl_->frame];
    vkCmdBindPipeline(impl_->comp, VK_PIPELINE_BIND_POINT_COMPUTE, k->vk_pipeline);
    vkCmdBindDescriptorSets(impl_->comp, VK_PIPELINE_BIND_POINT_COMPUTE, k->vk_layout,
                            0, 1, &set, 0, nullptr);
    vkCmdDispatch(impl_->comp, d.groups_x, d.groups_y, d.groups_z);
}

void CommandRecorder::BeginRenderPass(const RenderPassDesc& desc) {
    VkRenderPassBeginInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpi.renderPass = impl_->sc->renderPass;
    rpi.framebuffer = impl_->sc->swapChainFramebuffers[impl_->image_index];
    rpi.renderArea.offset = {0, 0};
    rpi.renderArea.extent = impl_->sc->swapChainExtent;
    VkClearValue clears[2]{};
    if (!desc.color.empty()) {
        clears[0].color = {{desc.color[0].clear[0], desc.color[0].clear[1],
                            desc.color[0].clear[2], desc.color[0].clear[3]}};
    }
    clears[1].depthStencil = {desc.depth.clear_depth, 0};
    rpi.clearValueCount = 2;
    rpi.pClearValues = clears;
    vkCmdBeginRenderPass(impl_->gfx, &rpi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(impl_->sc->swapChainExtent.width);
    viewport.height = static_cast<float>(impl_->sc->swapChainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(impl_->gfx, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = impl_->sc->swapChainExtent;
    vkCmdSetScissor(impl_->gfx, 0, 1, &scissor);
}

void CommandRecorder::DrawMeshes(const MeshDrawList& list) {
    VkCommandBuffer cb = impl_->gfx;
    VkDescriptorSet dyn_set = impl_->fr.dyn_ubo_sets[impl_->frame];

    VkBuffer bump_buf = impl_->rm->GetVkBumpMasterBuffer(Memory::kDynamic);
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
    vkUpdateDescriptorSets(impl_->device, 3, writes.data(), 0, nullptr);

    Shader::Hot* unlit = impl_->rm->GetHot(list.pipeline);
    VkDescriptorSet bindless =
        static_cast<VkDescriptorSet>(impl_->rm->GetHot(list.bindless)->api_descriptor_set);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->vk_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->vk_layout, 0, 1,
                            &bindless, 0, nullptr);

    for (size_t i = 0; i < list.sorted_indices.size(); ++i) {
        const cairns::Draw& draw = list.draws[list.sorted_indices[i]];
        uint32_t pos_off = 0;
        VkBuffer pos_buf =
            impl_->rm->GetVkBuffer(draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot], &pos_off);
        VkDeviceSize pos_off_dev =
            pos_off + static_cast<VkDeviceSize>(draw.vertex_offset) * 16u;
        vkCmdBindVertexBuffers(cb, 0, 1, &pos_buf, &pos_off_dev);
        uint32_t idx_base = 0;
        VkBuffer idx_buf = impl_->rm->GetVkBuffer(draw.index_buffer, &idx_base);
        vkCmdBindIndexBuffer(cb, idx_buf, idx_base, VK_INDEX_TYPE_UINT32);
        const uint32_t first_index = (draw.index_offset - idx_base) / sizeof(uint32_t);
        std::array<uint32_t, 3> dyn_offsets = {list.globals_offset,
                                               draw.dynamic_buffer_offsets[0],
                                               draw.dynamic_buffer_offsets[1]};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, unlit->vk_layout, 1, 1,
                                &dyn_set, 3, dyn_offsets.data());
        const uint32_t base_vertex = draw.vertex_offset;
        vkCmdPushConstants(cb, unlit->vk_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(uint32_t), &base_vertex);
        vkCmdDrawIndexed(cb, draw.triangle_count * 3, draw.instance_count, first_index, 0,
                         draw.instance_offset);
    }
}

void CommandRecorder::DrawPoints(const PointDraw& pd) {
    VkCommandBuffer cb = impl_->gfx;
    Shader::Hot* p = impl_->rm->GetHot(pd.pipeline);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->vk_pipeline);
    uint32_t ssbo_off = 0;
    VkBuffer ssbo = impl_->rm->GetVkBuffer(pd.vertex_buffer, &ssbo_off);
    VkDeviceSize off = ssbo_off;
    vkCmdBindVertexBuffers(cb, 0, 1, &ssbo, &off);
    VkDescriptorSet point_set = impl_->fr.point_sets[impl_->frame];
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->vk_layout, 0, 1,
                            &point_set, 0, nullptr);
    vkCmdDraw(cb, pd.vertex_count, 1, 0, 0);
}

void CommandRecorder::EndRenderPass() {
    vkCmdEndRenderPass(impl_->gfx);
}

FrameContext ResourceManager::BeginFrame(SwapChain& sc) {
    VkFrameResources& fr = impl_->frame_res;
    const uint32_t cf = impl_->recorder_frame;
    VkDevice dev = impl_->params.device;

    vkWaitForFences(dev, 1, &fr.compute_in_flight[cf], VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &fr.compute_in_flight[cf]);
    vkResetCommandBuffer(fr.compute_cmds[cf], 0);

    vkWaitForFences(dev, 1, &fr.in_flight[cf], VK_TRUE, UINT64_MAX);
    BeginFrame();  // bump ring reset

    uint32_t image_index = 0;
    vkAcquireNextImageKHR(dev, sc.swapChain, UINT64_MAX, fr.image_available[cf],
                          VK_NULL_HANDLE, &image_index);
    vkResetFences(dev, 1, &fr.in_flight[cf]);
    vkResetCommandBuffer(fr.graphics_cmds[cf], 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(fr.compute_cmds[cf], &bi);
    vkBeginCommandBuffer(fr.graphics_cmds[cf], &bi);

    FrameContext fc;
    fc.frame_index = cf;
    fc.swapchain_image_index = image_index;
    fc.cmd.impl_ = new CommandRecorder::Impl{this, &sc, fr, cf, image_index,
                                             fr.graphics_cmds[cf], fr.compute_cmds[cf], dev};
    return fc;
}

void ResourceManager::EndFrame(FrameContext& fc) {
    CommandRecorder::Impl* ri = fc.cmd.impl_;
    VkFrameResources& fr = impl_->frame_res;
    const uint32_t cf = fc.frame_index;

    vkEndCommandBuffer(ri->comp);
    VkSubmitInfo csi{};
    csi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    csi.commandBufferCount = 1;
    csi.pCommandBuffers = &fr.compute_cmds[cf];
    csi.signalSemaphoreCount = 1;
    csi.pSignalSemaphores = &fr.compute_finished[cf];
    vkQueueSubmit(fr.compute_queue, 1, &csi, fr.compute_in_flight[cf]);

    vkEndCommandBuffer(ri->gfx);
    VkSubmitInfo gsi{};
    gsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkSemaphore wait_sems[2] = {fr.compute_finished[cf], fr.image_available[cf]};
    VkPipelineStageFlags wait_stages[2] = {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    gsi.waitSemaphoreCount = 2;
    gsi.pWaitSemaphores = wait_sems;
    gsi.pWaitDstStageMask = wait_stages;
    gsi.commandBufferCount = 1;
    gsi.pCommandBuffers = &fr.graphics_cmds[cf];
    gsi.signalSemaphoreCount = 1;
    gsi.pSignalSemaphores = &fr.render_finished[cf];
    vkQueueSubmit(fr.graphics_queue, 1, &gsi, fr.in_flight[cf]);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &fr.render_finished[cf];
    VkSwapchainKHR swapchains[1] = {ri->sc->swapChain};
    pi.swapchainCount = 1;
    pi.pSwapchains = swapchains;
    pi.pImageIndices = &fc.swapchain_image_index;
    vkQueuePresentKHR(fr.present_queue, &pi);

    impl_->recorder_frame = (cf + 1) % fr.frames_in_flight;
    delete ri;
    fc.cmd.impl_ = nullptr;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
