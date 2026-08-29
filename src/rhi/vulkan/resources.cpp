// rhi/vulkan/resources.cpp  (Phase 0g-a: pools + destroy/get + frame counter)

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>

#include "rhi/resources.hpp"
#include "rhi/device.hpp"
#include "rhi/allocator.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/vulkan/internal/resources_impl.hpp"
#include "rhi/vulkan/internal/device_impl.hpp"
#include "rhi/vulkan/internal/allocator_impl.hpp"

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

bool Resources::Init(Device& device, Allocator& alloc) {
    if (impl_) {
        return true;
    }
    impl_ = new Impl();
    impl_->device = device.impl_->device;
    impl_->command_pool = device.impl_->command_pool;
    impl_->queue = device.impl_->graphics_queue;
    impl_->physical = device.impl_->physical;
    impl_->alloc = &alloc;
    return true;
}

void Resources::Deinit() {
    if (!impl_) {
        return;
    }
    delete impl_;
    impl_ = nullptr;
}

void Resources::AdvanceFrame() {
    impl_->frame_index++;
    impl_->alloc->AdvanceFrame(impl_->frame_index);
}

uint32_t Resources::FrameIndex() const { return impl_->frame_index; }

void Resources::Destroy(Handle<Buffer> h) {
    Buffer::Hot* hot = buffers.GetHot(h);
    Buffer::Cold* cold = buffers.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    impl_->alloc->impl_->memory.FreeBuffer(
        hot->heap_buffer_index, cold->alloc,
        impl_->frame_index + kFramesInFlight);
    buffers.Release(h);
}

void Resources::Destroy(Handle<Texture> h) {
    Texture::Hot* hot = textures.GetHot(h);
    Texture::Cold* cold = textures.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    impl_->alloc->impl_->memory.FreeImage(
        cold->heap_buffer_index, cold->alloc,
        static_cast<VkImage>(cold->api_image),
        static_cast<VkImageView>(hot->api_view),
        impl_->frame_index + kFramesInFlight);
    textures.Release(h);
}

void Resources::Destroy(Handle<Sampler> h) {
    Sampler::Hot* hot = samplers.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_sampler) {
        vkDestroySampler(impl_->device, static_cast<VkSampler>(hot->api_sampler),
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

Handle<Buffer> Resources::CreateBuffer(const BufferDesc& d) {
    uint32_t align = 16;
    if (d.usage & kUsageUniform) {
        align = std::max(align, impl_->alloc->impl_->uniform_align);
    }
    if (d.usage & kUsageStorage) {
        align = std::max(align, impl_->alloc->impl_->storage_align);
    }

    vulkan::AllocResult r =
        impl_->alloc->impl_->memory.AllocBuffer(d.byte_size, d.usage, d.memory, align);
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
        if (is_host_visible(d.memory)) {
            uint8_t* dst = MappedPtr(h);
            if (dst) {
                std::memcpy(dst, d.initial_data.data(), d.initial_data.size());
            }
        } else {
            uint32_t saved_cursor =
                impl_->alloc->impl_->memory.BumpSaveCursor(Memory::kUpload);
            void* staging = impl_->alloc->impl_->memory.BumpAllocate(
                static_cast<uint32_t>(d.initial_data.size()), 16,
                Memory::kUpload);
            if (staging) {
                std::memcpy(staging, d.initial_data.data(),
                            d.initial_data.size());
                uint32_t src_off = impl_->alloc->impl_->memory.BumpOffset(staging);
                uint32_t src_hi =
                    impl_->alloc->impl_->memory.BumpMasterHeapIndex(Memory::kUpload);
                VkBuffer src = impl_->alloc->impl_->memory.HeapMasterBuffer(src_hi);
                VkBuffer dst = impl_->alloc->impl_->memory.HeapMasterBuffer(r.heap_index);
                copy_via_staging(
                    impl_->device, impl_->command_pool,
                    impl_->queue, src, src_off, dst, r.offset,
                    static_cast<uint32_t>(d.initial_data.size()));
            }
            impl_->alloc->impl_->memory.BumpRestoreCursor(Memory::kUpload, saved_cursor);
        }
    }
    return h;
}

Handle<Texture> Resources::CreateTexture(const TextureDesc& d) {
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
    if (vkCreateImage(impl_->device, &ici, nullptr, &image) !=
        VK_SUCCESS) {
        return Handle<Texture>::Null;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(impl_->device, image, &req);
    vulkan::AllocResult r = impl_->alloc->impl_->memory.AllocImage(
        static_cast<uint32_t>(req.size), static_cast<uint32_t>(req.alignment),
        req.memoryTypeBits, Memory::kDefault);
    if (!r.ok) {
        vkDestroyImage(impl_->device, image, nullptr);
        return Handle<Texture>::Null;
    }
    vkBindImageMemory(impl_->device, image,
                      impl_->alloc->impl_->memory.HeapDeviceMemory(r.heap_index), r.offset);

    if (!d.initial_data.empty()) {
        uint32_t saved_cursor = impl_->alloc->impl_->memory.BumpSaveCursor(Memory::kUpload);
        void* staging = impl_->alloc->impl_->memory.BumpAllocate(
            static_cast<uint32_t>(d.initial_data.size()), 16, Memory::kUpload);
        if (staging) {
            std::memcpy(staging, d.initial_data.data(), d.initial_data.size());
            uint32_t src_off = impl_->alloc->impl_->memory.BumpOffset(staging);
            uint32_t src_hi = impl_->alloc->impl_->memory.BumpMasterHeapIndex(Memory::kUpload);
            VkBuffer src = impl_->alloc->impl_->memory.HeapMasterBuffer(src_hi);
            transition_to_transfer_dst(impl_->device,
                                       impl_->command_pool,
                                       impl_->queue, image, d.mip_levels);
            copy_buffer_to_image(impl_->device,
                                 impl_->command_pool, impl_->queue,
                                 src, src_off, image,
                                 static_cast<uint32_t>(d.dimensions.x),
                                 static_cast<uint32_t>(d.dimensions.y));
            generate_mipmaps(impl_->device, impl_->command_pool,
                             impl_->queue, impl_->physical, image,
                             vk_format, d.dimensions.x, d.dimensions.y,
                             d.mip_levels);
        }
        impl_->alloc->impl_->memory.BumpRestoreCursor(Memory::kUpload, saved_cursor);
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
    if (vkCreateImageView(impl_->device, &vci, nullptr, &view) !=
        VK_SUCCESS) {
        vkDestroyImage(impl_->device, image, nullptr);
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
    if (vkCreateSampler(impl_->device, &sci, nullptr, &sampler) !=
        VK_SUCCESS) {
        return Handle<Sampler>::Null;
    }

    Handle<Sampler> h = samplers.Acquire();
    samplers.GetHot(h)->api_sampler = sampler;
    samplers.GetCold(h)->debug_name = d.debug_name;
    return h;
}

Handle<BindGroup> Resources::CreateBindGroup(const BindGroupDesc&) {
    return Handle<BindGroup>::Null;
}

Handle<DynamicBuffers> Resources::CreateDynamicBuffers(
    const DynamicBuffersDesc&) {
    return Handle<DynamicBuffers>::Null;
}

VkBuffer Resources::GetVkBumpMasterBuffer(Memory mem) {
    void* p = impl_->alloc->BumpAllocate(1, 1, mem);
    (void)p;
    uint32_t hi = impl_->alloc->impl_->memory.BumpMasterHeapIndex(mem);
    return impl_->alloc->impl_->memory.HeapMasterBuffer(hi);
}

uint32_t Resources::BufferBaseOffset(Handle<Buffer> h) {
    uint32_t off = 0;
    GetVkBuffer(h, &off);
    return off;
}

VkBuffer Resources::GetVkBuffer(Handle<Buffer> h, uint32_t* out_offset) {
    if (h.generation == 0) {
        if (out_offset) {
            *out_offset = 0;
        }
        return impl_->alloc->impl_->memory.HeapMasterBuffer(h.index);
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
    return impl_->alloc->impl_->memory.HeapMasterBuffer(hot->heap_buffer_index);
}

uint8_t* Resources::MappedPtr(Handle<Buffer> h) {
    Buffer::Hot* hot = buffers.GetHot(h);
    if (!hot) {
        return nullptr;
    }
    uint8_t* base =
        static_cast<uint8_t*>(impl_->alloc->impl_->memory.HeapMappedPtr(hot->heap_buffer_index));
    if (!base) {
        return nullptr;
    }
    return base + hot->offset_in_heap;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
