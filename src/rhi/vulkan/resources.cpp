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
#include "rhi/frames.hpp"
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
        case Format::kR32Uint: return VK_FORMAT_R32_UINT;
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
    plat.device_ = device.plat.device_;
    plat.command_pool_ = device.plat.command_pool_;
    plat.queue_ = device.plat.graphics_queue_;
    plat.physical_ = device.plat.physical_;
    plat.resources_ = this;
    inited_ = true;
    return true;
}

void Resources::Deinit() {
    if (!inited_) {
        return;
    }
    VkDevice dev = plat.device_;
    if (plat.material_pool_) {
        vkDestroyDescriptorPool(dev, plat.material_pool_, nullptr);
        plat.material_pool_ = VK_NULL_HANDLE;
    }
    if (plat.material_set_layout_) {
        vkDestroyDescriptorSetLayout(dev, plat.material_set_layout_, nullptr);
        plat.material_set_layout_ = VK_NULL_HANDLE;
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
    plat.frame_index_++;
    alloc.AdvanceFrame(plat.frame_index_);
}

uint32_t Resources::FrameIndex() const { return plat.frame_index_; }

void Resources::Destroy(Allocator& alloc, Handle<Buffer> h) {
    Buffer::Hot* hot = buffers.GetHot(h);
    Buffer::Cold* cold = buffers.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    alloc.plat.memory_.FreeBuffer(
        hot->heap_buffer_index, cold->alloc,
        plat.frame_index_ + kFramesInFlight);
    buffers.Release(h);
}

void Resources::Destroy(Allocator& alloc, Handle<Texture> h) {
    Texture::Hot* hot = textures.GetHot(h);
    Texture::Cold* cold = textures.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    alloc.plat.memory_.FreeImage(
        cold->heap_buffer_index, cold->alloc,
        static_cast<VkImage>(cold->api_image),
        static_cast<VkImageView>(hot->api_view),
        plat.frame_index_ + kFramesInFlight);
    textures.Release(h);
}

void Resources::Destroy(Handle<Sampler> h) {
    Sampler::Hot* hot = samplers.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_sampler) {
        vkDestroySampler(plat.device_, static_cast<VkSampler>(hot->api_sampler),
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
        align = std::max(align, alloc.plat.uniform_align_);
    }
    if (d.usage & kUsageStorage) {
        align = std::max(align, alloc.plat.storage_align_);
    }

    vulkan::AllocResult r =
        alloc.plat.memory_.AllocBuffer(d.byte_size, d.usage, d.memory, align);
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
        uint8_t* dst = plat.MappedPtr(alloc, h);
        if (dst) {
            std::memcpy(dst + dst_offset, data.data(), data.size());
        }
        return;
    }
    const uint32_t saved_cursor = alloc.plat.memory_.BumpSaveCursor(Memory::kUpload);
    const uint32_t ring_bytes = alloc.plat.memory_.BumpRingBytes(Memory::kUpload);
    const uint32_t cap = (ring_bytes > saved_cursor + 16u)
                             ? (ring_bytes - saved_cursor - 16u)
                             : 0u;
    const size_t total = data.size();
    VkBuffer dst_buf = alloc.plat.memory_.HeapMasterBuffer(hot->heap_buffer_index);
    size_t done = 0;
    while (done < total && cap > 0u) {
        const uint32_t chunk =
            static_cast<uint32_t>(std::min<size_t>(total - done, cap));
        uint32_t src_off = 0;
        void* staging = alloc.plat.memory_.BumpAllocate(chunk, 16, Memory::kUpload,
                                                   &src_off);
        if (!staging) {
            break;
        }
        std::memcpy(staging, data.data() + done, chunk);
        uint32_t src_hi = alloc.plat.memory_.BumpMasterHeapIndex(Memory::kUpload);
        VkBuffer src = alloc.plat.memory_.HeapMasterBuffer(src_hi);
        copy_via_staging(plat.device_, plat.command_pool_, plat.queue_, src, src_off, dst_buf,
                         static_cast<uint32_t>(hot->offset_in_heap +
                                               dst_offset + done),
                         chunk);
        alloc.plat.memory_.BumpRestoreCursor(Memory::kUpload, saved_cursor);
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
    if (vkCreateImage(plat.device_, &ici, nullptr, &image) !=
        VK_SUCCESS) {
        return Handle<Texture>::Null;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(plat.device_, image, &req);
    vulkan::AllocResult r = alloc.plat.memory_.AllocImage(
        static_cast<uint32_t>(req.size), static_cast<uint32_t>(req.alignment),
        req.memoryTypeBits, Memory::kDefault);
    if (!r.ok) {
        vkDestroyImage(plat.device_, image, nullptr);
        return Handle<Texture>::Null;
    }
    vkBindImageMemory(plat.device_, image,
                      alloc.plat.memory_.HeapDeviceMemory(r.heap_index), r.offset);

    if (!d.initial_data.empty()) {
        uint32_t saved_cursor = alloc.plat.memory_.BumpSaveCursor(Memory::kUpload);
        uint32_t src_off = 0;
        void* staging = alloc.plat.memory_.BumpAllocate(
            static_cast<uint32_t>(d.initial_data.size()), 16, Memory::kUpload, &src_off);
        if (staging) {
            std::memcpy(staging, d.initial_data.data(), d.initial_data.size());
            uint32_t src_hi = alloc.plat.memory_.BumpMasterHeapIndex(Memory::kUpload);
            VkBuffer src = alloc.plat.memory_.HeapMasterBuffer(src_hi);
            transition_to_transfer_dst(plat.device_,
                                       plat.command_pool_,
                                       plat.queue_, image, d.mip_levels);
            copy_buffer_to_image(plat.device_,
                                 plat.command_pool_, plat.queue_,
                                 src, src_off, image,
                                 static_cast<uint32_t>(d.dimensions.x),
                                 static_cast<uint32_t>(d.dimensions.y));
            generate_mipmaps(plat.device_, plat.command_pool_,
                             plat.queue_, plat.physical_, image,
                             vk_format, d.dimensions.x, d.dimensions.y,
                             d.mip_levels);
        }
        alloc.plat.memory_.BumpRestoreCursor(Memory::kUpload, saved_cursor);
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
    if (vkCreateImageView(plat.device_, &vci, nullptr, &view) !=
        VK_SUCCESS) {
        vkDestroyImage(plat.device_, image, nullptr);
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
    if (vkCreateSampler(plat.device_, &sci, nullptr, &sampler) !=
        VK_SUCCESS) {
        return Handle<Sampler>::Null;
    }

    Handle<Sampler> h = samplers.Acquire();
    samplers.GetHot(h)->api_sampler = sampler;
    samplers.GetCold(h)->debug_name = d.debug_name;
    return h;
}

VkDescriptorSetLayout ResourcesPlat::MaterialSetLayout() {
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
    plat.MaterialSetLayout();  // ensure shared layout + pool exist
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = plat.material_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &plat.material_set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(plat.device_, &ai, &set) != VK_SUCCESS) {
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
    vkUpdateDescriptorSets(plat.device_, 1, &w, 0, nullptr);

    Handle<BindGroup> h = bind_groups.Acquire();
    bind_groups.GetHot(h)->api_descriptor_set = set;
    bind_groups.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Handle<BindGroup> Resources::CreateSkinGroupA(Allocator& alloc,
                                                Frames& frames,
                                                const BindGroupDesc& desc) {
    if (desc.buffers.size() != 2) {
        return Handle<BindGroup>::Null;
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = frames.plat.descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &frames.plat.skin_group_a_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(plat.device_, &ai, &set) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }
    VkDescriptorBufferInfo bi[2]{};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        const BufferBinding& bb = desc.buffers[i];
        uint32_t master_off = 0;
        VkBuffer vk_buf = plat.GetVkBuffer(alloc, bb.buffer, &master_off);
        bi[i].buffer = vk_buf;
        bi[i].offset = master_off + bb.offset;
        bi[i].range = (bb.range == 0) ? VK_WHOLE_SIZE
                                       : static_cast<VkDeviceSize>(bb.range);
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = bb.slot;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].descriptorCount = 1;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(plat.device_, 2, w, 0, nullptr);
    Handle<BindGroup> h = bind_groups.Acquire();
    bind_groups.GetHot(h)->api_descriptor_set = set;
    bind_groups.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Handle<DynamicBuffers> Resources::CreateDynamicBuffers(
    const DynamicBuffersDesc&) {
    return Handle<DynamicBuffers>::Null;
}

VkBuffer ResourcesPlat::GetVkBumpMasterBuffer(Allocator& alloc, Memory mem) {
    void* p = alloc.BumpAllocate(1, 1, mem);
    (void)p;
    uint32_t hi = alloc.plat.memory_.BumpMasterHeapIndex(mem);
    return alloc.plat.memory_.HeapMasterBuffer(hi);
}

uint32_t Resources::BufferBaseOffset(Allocator& alloc, Handle<Buffer> h) {
    uint32_t off = 0;
    plat.GetVkBuffer(alloc, h, &off);
    return off;
}

VkBuffer ResourcesPlat::GetVkBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t* out_offset) {
    if (h.generation == 0) {
        if (out_offset) {
            *out_offset = 0;
        }
        return alloc.plat.memory_.HeapMasterBuffer(h.index);
    }
    Buffer::Hot* hot = resources_->buffers.GetHot(h);
    if (!hot) {
        if (out_offset) {
            *out_offset = 0;
        }
        return VK_NULL_HANDLE;
    }
    if (out_offset) {
        *out_offset = hot->offset_in_heap;
    }
    return alloc.plat.memory_.HeapMasterBuffer(hot->heap_buffer_index);
}

uint8_t* ResourcesPlat::MappedPtr(Allocator& alloc, Handle<Buffer> h) {
    Buffer::Hot* hot = resources_->buffers.GetHot(h);
    if (!hot) {
        return nullptr;
    }
    uint8_t* base =
        static_cast<uint8_t*>(alloc.plat.memory_.HeapMappedPtr(hot->heap_buffer_index));
    if (!base) {
        return nullptr;
    }
    return base + hot->offset_in_heap;
}

// Image→host-visible buffer→RGBA8 swizzle. Image is BGRA8 today (matches the
// engine's final_target_ format). Layout assumed SHADER_READ_ONLY_OPTIMAL on
// entry (we're reading the engine's last-render output).
bool Resources::ReadBackTextureRgba(Handle<Texture> h,
                                      std::vector<uint8_t>& out_rgba,
                                      uint32_t& out_w, uint32_t& out_h) {
    Texture::Cold* cold = textures.GetCold(h);
    if (!cold) {
        return false;
    }
    VkImage img = static_cast<VkImage>(cold->api_image);
    if (img == VK_NULL_HANDLE) {
        return false;
    }
    const uint32_t w = cold->width;
    const uint32_t hgt = cold->height;
    const VkDeviceSize buf_size = static_cast<VkDeviceSize>(w) * hgt * 4;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = buf_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(plat.device_, &bci, nullptr, &buf) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(plat.device_, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(plat.physical_, &mp);
    uint32_t type_idx = 0;
    bool found_type = false;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const auto need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & need) == need) {
            type_idx = i;
            found_type = true;
            break;
        }
    }
    if (!found_type) {
        vkDestroyBuffer(plat.device_, buf, nullptr);
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type_idx;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(plat.device_, &mai, nullptr, &mem);
    vkBindBufferMemory(plat.device_, buf, mem, 0);

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = plat.command_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(plat.device_, &cai, &cb);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // Texture could be in SHADER_READ_ONLY_OPTIMAL (windowed dump after a
    // sampling pass) or COLOR_ATTACHMENT_OPTIMAL (surfaceless: swap pass
    // left it that way; offscreen RP's finalLayout). Use COLOR_ATTACHMENT
    // as a safe oldLayout -- valid for both since the offscreen cache
    // never leaves the image in some third layout.
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = img;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &to_src);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, hgt, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            buf, 1, &region);

    VkImageMemoryBarrier to_shader = to_src;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          0, nullptr, 0, nullptr, 1, &to_shader);

    vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(plat.queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(plat.queue_);

    void* mapped = nullptr;
    vkMapMemory(plat.device_, mem, 0, buf_size, 0, &mapped);
    out_rgba.resize(static_cast<size_t>(buf_size));
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    for (uint32_t i = 0; i < w * hgt; ++i) {
        out_rgba[i * 4 + 0] = src[i * 4 + 2];
        out_rgba[i * 4 + 1] = src[i * 4 + 1];
        out_rgba[i * 4 + 2] = src[i * 4 + 0];
        out_rgba[i * 4 + 3] = src[i * 4 + 3];
    }
    vkUnmapMemory(plat.device_, mem);
    out_w = w;
    out_h = hgt;

    vkFreeCommandBuffers(plat.device_, plat.command_pool_, 1, &cb);
    vkDestroyBuffer(plat.device_, buf, nullptr);
    vkFreeMemory(plat.device_, mem, nullptr);
    return true;
}

// #207 single-texel R32U readback for pick. vkCmdCopyImageToBuffer of a
// 1x1 region into a host-visible staging buffer, waits, reads the uint32.
// Caller is expected to have drained in-flight rendering before calling
// (we don't add cross-frame sync beyond an immediate queueWaitIdle).
bool Resources::ReadBackTextureR32UTexel(Handle<Texture> h, uint32_t x,
                                         uint32_t y, uint32_t& out_value) {
    Texture::Cold* cold = textures.GetCold(h);
    if (!cold) {
        return false;
    }
    VkImage img = static_cast<VkImage>(cold->api_image);
    if (img == VK_NULL_HANDLE || x >= cold->width || y >= cold->height) {
        return false;
    }
    const VkDeviceSize buf_size = 4;  // one R32U texel
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = buf_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(plat.device_, &bci, nullptr, &buf) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(plat.device_, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(plat.physical_, &mp);
    uint32_t type_idx = 0;
    bool found_type = false;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const auto need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & need) == need) {
            type_idx = i;
            found_type = true;
            break;
        }
    }
    if (!found_type) {
        vkDestroyBuffer(plat.device_, buf, nullptr);
        return false;
    }
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type_idx;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(plat.device_, &mai, nullptr, &mem);
    vkBindBufferMemory(plat.device_, buf, mem, 0);

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = plat.command_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(plat.device_, &cai, &cb);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // id_target_'s actual layout depends on whether the outline pass ran
    // this frame (SHADER_READ_ONLY) or not (still COLOR_ATTACHMENT from
    // the forward pass). Read the layout tracker maintained by
    // command_recorder's transition() and feed that as oldLayout so the
    // barrier is correct in both cases.
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = cold->plat.vk_layout;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = img;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_src.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &to_src);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            buf, 1, &region);

    // Restore the original layout so the graph's per-pass transition()
    // doesn't emit a redundant barrier next frame (and so the tracker
    // stays consistent with what the GPU sees).
    VkImageMemoryBarrier to_orig = to_src;
    to_orig.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_orig.newLayout = cold->plat.vk_layout;
    to_orig.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    to_orig.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                          0, nullptr, 0, nullptr, 1, &to_orig);

    vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(plat.queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(plat.queue_);

    void* mapped = nullptr;
    vkMapMemory(plat.device_, mem, 0, buf_size, 0, &mapped);
    out_value = *static_cast<const uint32_t*>(mapped);
    vkUnmapMemory(plat.device_, mem);

    vkFreeCommandBuffers(plat.device_, plat.command_pool_, 1, &cb);
    vkDestroyBuffer(plat.device_, buf, nullptr);
    vkFreeMemory(plat.device_, mem, nullptr);
    return true;
}

// vkCmdClearColorImage on a freshly-acquired image; transition into
// SHADER_READ_ONLY_OPTIMAL so the subsequent dump path can read it.
bool Resources::ClearColorTexture(Handle<Texture> h, const float color[4]) {
    Texture::Cold* cold = textures.GetCold(h);
    if (!cold) {
        return false;
    }
    VkImage img = static_cast<VkImage>(cold->api_image);
    if (img == VK_NULL_HANDLE) {
        return false;
    }
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = plat.command_pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(plat.device_, &cai, &cb) != VK_SUCCESS) {
        return false;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = img;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                          0, nullptr, 1, &to_dst);
    VkClearColorValue cc{};
    cc.float32[0] = color[0];
    cc.float32[1] = color[1];
    cc.float32[2] = color[2];
    cc.float32[3] = color[3];
    VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          &cc, 1, &r);
    VkImageMemoryBarrier to_shader = to_dst;
    to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          0, nullptr, 0, nullptr, 1, &to_shader);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(plat.queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(plat.queue_);
    vkFreeCommandBuffers(plat.device_, plat.command_pool_, 1, &cb);
    return true;
}

// vk surfaceless render-to-texture (#199): target.plat.swap_chain stays
// null (sentinel for "no swapchain"); Frames::Begin / EndSubmit / Present
// + CommandRecorder::BeginRenderPass all gate on swap_chain == nullptr.
// The graph's swap pass writes final_target_ as desc.color[0].target, so
// BeginRenderPass picks up the offscreen framebuffer over the texture's
// VkImageView via the existing offscreen-target cache -- nothing new on
// the renderpass side.
SwapResolveTarget Resources::MakeSurfacelessSwapResolveTarget(
    Handle<Texture> /*h*/, uint32_t w, uint32_t h_px) {
    SwapResolveTarget t;
    t.width = w;
    t.height = h_px;
    t.plat.swap_chain = nullptr;
    return t;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
