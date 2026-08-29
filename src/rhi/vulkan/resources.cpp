// rhi/vulkan/resources.cpp  (Phase 0g-a: pools + destroy/get + frame counter)

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cassert>
#include <cstring>

#include "rhi/resources.hpp"
#include "rhi/device.hpp"
#include "rhi/allocator.hpp"
#include "rhi/resource_manager.hpp"

namespace cairns::rhi {

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

Resources::~Resources() { Deinit(); }

bool Resources::Init(Device& device) {
    if (inited_) {
        return true;
    }
    device_ = device.device_;
    command_pool_ = device.command_pool_;
    queue_ = device.graphics_queue_;
    physical_ = device.physical_;
    inited_ = true;
    return true;
}

void Resources::Deinit() {
    if (!inited_) {
        return;
    }
    VkDevice dev = device_;
    if (material_pool_) {
        vkDestroyDescriptorPool(dev, material_pool_, nullptr);
        material_pool_ = VK_NULL_HANDLE;
    }
    if (material_set_layout_) {
        vkDestroyDescriptorSetLayout(dev, material_set_layout_, nullptr);
        material_set_layout_ = VK_NULL_HANDLE;
    }
    textures.ForEachLive([dev](Texture::Hot& hot, Texture::Cold& cold) {
        if (hot.api_view) {
            vkDestroyImageView(dev, static_cast<VkImageView>(hot.api_view), nullptr);
            hot.api_view = nullptr;
        }
        if (cold.api_image) {
            vkDestroyImage(dev, static_cast<VkImage>(cold.api_image), nullptr);
            cold.api_image = nullptr;
        }
    });
    samplers.ForEachLive([dev](Sampler::Hot& hot, Sampler::Cold&) {
        if (hot.api_sampler) {
            vkDestroySampler(dev, static_cast<VkSampler>(hot.api_sampler), nullptr);
            hot.api_sampler = nullptr;
        }
    });
    inited_ = false;
}

void Resources::AdvanceFrame(Allocator& alloc) {
    frame_index_++;
    alloc.AdvanceFrame(frame_index_);
}

uint32_t Resources::FrameIndex() const { return frame_index_; }

void Resources::Destroy(Allocator& alloc, Handle<Buffer> h) {
    Buffer::Hot* hot = buffers.GetHot(h);
    Buffer::Cold* cold = buffers.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    alloc.memory_.FreeBuffer(
        hot->heap_buffer_index, cold->alloc,
        frame_index_ + kFramesInFlight);
    buffers.Release(h);
}

void Resources::Destroy(Allocator& alloc, Handle<Texture> h) {
    Texture::Hot* hot = textures.GetHot(h);
    Texture::Cold* cold = textures.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    alloc.memory_.FreeImage(
        cold->heap_buffer_index, cold->alloc,
        static_cast<VkImage>(cold->api_image),
        static_cast<VkImageView>(hot->api_view),
        frame_index_ + kFramesInFlight);
    textures.Release(h);
}

void Resources::Destroy(Handle<Sampler> h) {
    Sampler::Hot* hot = samplers.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_sampler) {
        vkDestroySampler(device_, static_cast<VkSampler>(hot->api_sampler),
                         nullptr);
    }
    samplers.Release(h);
}

void Resources::Destroy(Handle<BindGroup> h) { bind_groups.Release(h); }

void Resources::Destroy(Handle<DynamicBuffers> h) { dynamic_buffers.Release(h); }

void Resources::Destroy(Handle<Shader> h) { shaders.Release(h); }

void Resources::Destroy(Handle<Kernel> h) { kernels.Release(h); }

Buffer::Hot* Resources::GetHot(Handle<Buffer> h) { return buffers.GetHot(h); }
Texture::Hot* Resources::GetHot(Handle<Texture> h) { return textures.GetHot(h); }
Sampler::Hot* Resources::GetHot(Handle<Sampler> h) { return samplers.GetHot(h); }
BindGroup::Hot* Resources::GetHot(Handle<BindGroup> h) { return bind_groups.GetHot(h); }
DynamicBuffers::Hot* Resources::GetHot(Handle<DynamicBuffers> h) {
    return dynamic_buffers.GetHot(h);
}
Shader::Hot* Resources::GetHot(Handle<Shader> h) { return shaders.GetHot(h); }
Kernel::Hot* Resources::GetHot(Handle<Kernel> h) { return kernels.GetHot(h); }

uint32_t Resources::GetBufferByteSize(Handle<Buffer> h) {
    Buffer::Cold* cold = buffers.GetCold(h);
    if (!cold) {
        return 0;
    }
    return cold->size_bytes;
}

Handle<Buffer> Resources::CreateBuffer(Allocator& alloc, const BufferDesc& d) {
    uint32_t align = 16;
    if (d.usage & kUsageUniform) {
        align = std::max(align, alloc.uniform_align_);
    }
    if (d.usage & kUsageStorage) {
        align = std::max(align, alloc.storage_align_);
    }

    vulkan::AllocResult r =
        alloc.memory_.AllocBuffer(d.byte_size, d.usage, d.memory, align);
    if (!r.ok) {
        return Handle<Buffer>::Null;
    }

    Handle<Buffer> h = buffers.Acquire();
    Buffer::Hot* hot = buffers.GetHot(h);
    hot->heap_buffer_index = static_cast<uint16_t>(r.heap_index);
    hot->pad = 0;
    hot->offset_in_heap = r.offset;

    Buffer::Cold* cold = buffers.GetCold(h);
    cold->alloc = r.alloc;
    cold->size_bytes = d.byte_size;
    cold->usage = d.usage;
    cold->mem_type = d.memory;
    cold->debug_name = d.debug_name;

    if (!d.initial_data.empty()) {
        UploadBuffer(alloc, h, 0, d.initial_data);
    }
    return h;
}

void Resources::UploadBuffer(Allocator& alloc, Handle<Buffer> h,
                              uint32_t dst_offset,
                              std::span<const uint8_t> data) {
    if (data.empty()) {
        return;
    }
    Buffer::Hot* hot = GetHot(h);
    Buffer::Cold* cold = buffers.GetCold(h);
    assert(hot && cold && "UploadBuffer: bad handle");
    if (is_host_visible(cold->mem_type)) {
        uint8_t* dst = MappedPtr(alloc, h);
        if (dst) {
            std::memcpy(dst + dst_offset, data.data(), data.size());
        }
        return;
    }
    const uint32_t saved_cursor = alloc.memory_.BumpSaveCursor(Memory::kUpload);
    const uint32_t ring_bytes = alloc.memory_.BumpRingBytes(Memory::kUpload);
    const uint32_t cap = (ring_bytes > saved_cursor + 16u)
                             ? (ring_bytes - saved_cursor - 16u)
                             : 0u;
    const size_t total = data.size();
    VkBuffer dst_buf = alloc.memory_.HeapMasterBuffer(hot->heap_buffer_index);
    size_t done = 0;
    while (done < total && cap > 0u) {
        const uint32_t chunk =
            static_cast<uint32_t>(std::min<size_t>(total - done, cap));
        uint32_t src_off = 0;
        void* staging = alloc.memory_.BumpAllocate(chunk, 16, Memory::kUpload,
                                                   &src_off);
        if (!staging) {
            break;
        }
        std::memcpy(staging, data.data() + done, chunk);
        uint32_t src_hi = alloc.memory_.BumpMasterHeapIndex(Memory::kUpload);
        VkBuffer src = alloc.memory_.HeapMasterBuffer(src_hi);
        copy_via_staging(device_, command_pool_, queue_, src, src_off, dst_buf,
                         static_cast<uint32_t>(hot->offset_in_heap +
                                               dst_offset + done),
                         chunk);
        alloc.memory_.BumpRestoreCursor(Memory::kUpload, saved_cursor);
        done += chunk;
    }
}

Handle<Texture> Resources::CreateTexture(Allocator& alloc, const TextureDesc& d) {
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
    if (vkCreateImage(device_, &ici, nullptr, &image) !=
        VK_SUCCESS) {
        return Handle<Texture>::Null;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, image, &req);
    vulkan::AllocResult r = alloc.memory_.AllocImage(
        static_cast<uint32_t>(req.size), static_cast<uint32_t>(req.alignment),
        req.memoryTypeBits, Memory::kDefault);
    if (!r.ok) {
        vkDestroyImage(device_, image, nullptr);
        return Handle<Texture>::Null;
    }
    vkBindImageMemory(device_, image,
                      alloc.memory_.HeapDeviceMemory(r.heap_index), r.offset);

    if (!d.initial_data.empty()) {
        uint32_t saved_cursor = alloc.memory_.BumpSaveCursor(Memory::kUpload);
        uint32_t src_off = 0;
        void* staging = alloc.memory_.BumpAllocate(
            static_cast<uint32_t>(d.initial_data.size()), 16, Memory::kUpload, &src_off);
        if (staging) {
            std::memcpy(staging, d.initial_data.data(), d.initial_data.size());
            uint32_t src_hi = alloc.memory_.BumpMasterHeapIndex(Memory::kUpload);
            VkBuffer src = alloc.memory_.HeapMasterBuffer(src_hi);
            transition_to_transfer_dst(device_,
                                       command_pool_,
                                       queue_, image, d.mip_levels);
            copy_buffer_to_image(device_,
                                 command_pool_, queue_,
                                 src, src_off, image,
                                 static_cast<uint32_t>(d.dimensions.x),
                                 static_cast<uint32_t>(d.dimensions.y));
            generate_mipmaps(device_, command_pool_,
                             queue_, physical_, image,
                             vk_format, d.dimensions.x, d.dimensions.y,
                             d.mip_levels);
        }
        alloc.memory_.BumpRestoreCursor(Memory::kUpload, saved_cursor);
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
    if (vkCreateImageView(device_, &vci, nullptr, &view) !=
        VK_SUCCESS) {
        vkDestroyImage(device_, image, nullptr);
        return Handle<Texture>::Null;
    }

    Handle<Texture> h = textures.Acquire();
    Texture::Hot* hot = textures.GetHot(h);
    hot->api_view = view;
    hot->descriptor_index = 0;

    Texture::Cold* cold = textures.GetCold(h);
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

Handle<Sampler> Resources::CreateSampler(const SamplerDesc& d) {
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
    if (vkCreateSampler(device_, &sci, nullptr, &sampler) !=
        VK_SUCCESS) {
        return Handle<Sampler>::Null;
    }

    Handle<Sampler> h = samplers.Acquire();
    samplers.GetHot(h)->api_sampler = sampler;
    samplers.GetCold(h)->debug_name = d.debug_name;
    return h;
}

VkDescriptorSetLayout Resources::MaterialSetLayout() {
    if (material_set_layout_ != VK_NULL_HANDLE) {
        return material_set_layout_;
    }
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &b;
    vkCreateDescriptorSetLayout(device_, &li, nullptr, &material_set_layout_);

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 4096;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 4096;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    vkCreateDescriptorPool(device_, &pi, nullptr, &material_pool_);
    return material_set_layout_;
}

Handle<BindGroup> Resources::CreateBindGroup(const BindGroupDesc& desc) {
    MaterialSetLayout();  // ensure shared layout + pool exist
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = material_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &material_set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &ai, &set) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }
    // set 2 = one combined image+sampler: pair textures[0] with samplers[0].
    VkDescriptorImageInfo img{};
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!desc.textures.empty()) {
        img.imageView = static_cast<VkImageView>(
            textures.GetHot(desc.textures[0].texture)->api_view);
    }
    if (!desc.samplers.empty()) {
        img.sampler = static_cast<VkSampler>(
            samplers.GetHot(desc.samplers[0].sampler)->api_sampler);
    }
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo = &img;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);

    Handle<BindGroup> h = bind_groups.Acquire();
    bind_groups.GetHot(h)->api_descriptor_set = set;
    bind_groups.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Handle<DynamicBuffers> Resources::CreateDynamicBuffers(
    const DynamicBuffersDesc&) {
    return Handle<DynamicBuffers>::Null;
}

VkBuffer Resources::GetVkBumpMasterBuffer(Allocator& alloc, Memory mem) {
    void* p = alloc.BumpAllocate(1, 1, mem);
    (void)p;
    uint32_t hi = alloc.memory_.BumpMasterHeapIndex(mem);
    return alloc.memory_.HeapMasterBuffer(hi);
}

uint32_t Resources::BufferBaseOffset(Allocator& alloc, Handle<Buffer> h) {
    uint32_t off = 0;
    GetVkBuffer(alloc, h, &off);
    return off;
}

VkBuffer Resources::GetVkBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t* out_offset) {
    if (h.generation == 0) {
        if (out_offset) {
            *out_offset = 0;
        }
        return alloc.memory_.HeapMasterBuffer(h.index);
    }
    Buffer::Hot* hot = buffers.GetHot(h);
    if (!hot) {
        if (out_offset) {
            *out_offset = 0;
        }
        return VK_NULL_HANDLE;
    }
    if (out_offset) {
        *out_offset = hot->offset_in_heap;
    }
    return alloc.memory_.HeapMasterBuffer(hot->heap_buffer_index);
}

uint8_t* Resources::MappedPtr(Allocator& alloc, Handle<Buffer> h) {
    Buffer::Hot* hot = buffers.GetHot(h);
    if (!hot) {
        return nullptr;
    }
    uint8_t* base =
        static_cast<uint8_t*>(alloc.memory_.HeapMappedPtr(hot->heap_buffer_index));
    if (!base) {
        return nullptr;
    }
    return base + hot->offset_in_heap;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
