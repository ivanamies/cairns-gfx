// rhi/metal/resource_manager.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi/resource_manager.hpp"

#include <algorithm>
#include <cstring>

#include <Metal/Metal.hpp>

#include "rhi/metal/memory_allocator.hpp"

namespace cairns::rhi {

struct ResourceManager::Impl {
    BackendInitParams params;
    metal::MemoryAllocator memory;

    Pool<Buffer> buffers;
    Pool<Texture> textures;
    Pool<Sampler> samplers;
    Pool<BindGroup> bind_groups;
    Pool<DynamicBuffers> dynamic_buffers;
    Pool<Shader> shaders;
    Pool<Kernel> kernels;

    uint32_t frame_index = 1;
};

namespace {

bool is_host_visible(Memory mem) {
    return mem == Memory::kUpload || mem == Memory::kDynamic ||
           mem == Memory::kReadback;
}

MTL::StorageMode storage_mode_for(Memory mem) {
    switch (mem) {
        case Memory::kDefault:   return MTL::StorageModePrivate;
        case Memory::kUpload:    return MTL::StorageModeShared;
        case Memory::kReadback:  return MTL::StorageModeShared;
        case Memory::kDynamic:   return MTL::StorageModeShared;
        case Memory::kTransient: return MTL::StorageModeMemoryless;
        default:                 return MTL::StorageModePrivate;
    }
}

MTL::PixelFormat to_mtl_pixel_format(Format f) {
    switch (f) {
        case Format::kR8Unorm:    return MTL::PixelFormatR8Unorm;
        case Format::kRg8Unorm:   return MTL::PixelFormatRG8Unorm;
        case Format::kRgba8Unorm: return MTL::PixelFormatRGBA8Unorm;
        case Format::kRgba8Srgb:  return MTL::PixelFormatRGBA8Unorm_sRGB;
        case Format::kBgra8Unorm: return MTL::PixelFormatBGRA8Unorm;
        case Format::kBgra8Srgb:  return MTL::PixelFormatBGRA8Unorm_sRGB;
        case Format::kR16F:       return MTL::PixelFormatR16Float;
        case Format::kRgba16F:    return MTL::PixelFormatRGBA16Float;
        case Format::kR32F:       return MTL::PixelFormatR32Float;
        case Format::kRgba32F:    return MTL::PixelFormatRGBA32Float;
        case Format::kD32F:       return MTL::PixelFormatDepth32Float;
        default:                  return MTL::PixelFormatInvalid;
    }
}

MTL::TextureUsage to_mtl_texture_usage(TextureUsage u, uint32_t mip_levels) {
    NS::UInteger out = MTL::TextureUsageUnknown;
    if (u & kTexUsageSampled)     { out |= MTL::TextureUsageShaderRead; }
    if (u & kTexUsageStorage)     { out |= MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite; }
    if (u & kTexUsageColorTarget) { out |= MTL::TextureUsageRenderTarget; }
    if (u & kTexUsageDepthTarget) { out |= MTL::TextureUsageRenderTarget; }
    if (mip_levels > 1)           { out |= MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead; }
    return static_cast<MTL::TextureUsage>(out);
}

MTL::SamplerMinMagFilter to_mtl_min_mag(Filter f) {
    return f == Filter::kNearest ? MTL::SamplerMinMagFilterNearest
                                 : MTL::SamplerMinMagFilterLinear;
}

MTL::SamplerMipFilter to_mtl_mip_filter(Filter f) {
    return f == Filter::kNearest ? MTL::SamplerMipFilterNearest
                                 : MTL::SamplerMipFilterLinear;
}

MTL::SamplerAddressMode to_mtl_address_mode(AddressMode m) {
    switch (m) {
        case AddressMode::kMirroredRepeat: return MTL::SamplerAddressModeMirrorRepeat;
        case AddressMode::kClampToEdge:    return MTL::SamplerAddressModeClampToEdge;
        case AddressMode::kClampToBorder:  return MTL::SamplerAddressModeClampToZero;
        default:                           return MTL::SamplerAddressModeRepeat;
    }
}

}  // namespace

ResourceManager::~ResourceManager() {
    Deinit();
}

void ResourceManager::Deinit() {
    delete impl_;
    impl_ = nullptr;
}

bool ResourceManager::Init(const BackendInitParams& params) {
    impl_ = new Impl();
    impl_->params = params;
    if (!impl_->memory.Init(params.device)) {
        return false;
    }
    return true;
}

Handle<Buffer> ResourceManager::CreateBuffer(const BufferDesc& d) {
    metal::AllocResult r =
        impl_->memory.AllocBuffer(d.byte_size, d.usage, d.memory, 16);
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
            void* staging = impl_->memory.BumpAllocate(
                static_cast<uint32_t>(d.initial_data.size()), 16,
                Memory::kUpload);
            if (staging) {
                std::memcpy(staging, d.initial_data.data(),
                            d.initial_data.size());
                uint32_t src_off = impl_->memory.BumpOffset(staging);
                uint32_t src_hi =
                    impl_->memory.BumpMasterHeapIndex(Memory::kUpload);
                MTL::Buffer* src = impl_->memory.HeapMasterBuffer(src_hi);
                MTL::Buffer* dst_buf =
                    impl_->memory.HeapMasterBuffer(r.heap_index);
                MTL::CommandBuffer* cmd =
                    impl_->params.queue->commandBuffer();
                MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
                blit->copyFromBuffer(src, src_off, dst_buf, r.offset,
                                     static_cast<uint32_t>(
                                         d.initial_data.size()));
                blit->endEncoding();
                cmd->commit();
                cmd->waitUntilCompleted();
            }
        }
    }
    return h;
}

Handle<Texture> ResourceManager::CreateTexture(const TextureDesc& d) {
    MTL::TextureDescriptor* td = MTL::TextureDescriptor::alloc()->init();
    if (d.sample_count > 1) {
        td->setTextureType(MTL::TextureType2DMultisample);
        td->setSampleCount(static_cast<NS::UInteger>(d.sample_count));
    } else {
        td->setTextureType(MTL::TextureType2D);
    }
    td->setPixelFormat(to_mtl_pixel_format(d.format));
    td->setWidth(static_cast<NS::UInteger>(d.dimensions.x));
    td->setHeight(static_cast<NS::UInteger>(d.dimensions.y));
    td->setMipmapLevelCount(static_cast<NS::UInteger>(d.mip_levels));
    td->setArrayLength(static_cast<NS::UInteger>(d.array_layers));
    td->setUsage(to_mtl_texture_usage(d.usage, d.mip_levels));
    td->setStorageMode(storage_mode_for(d.memory));
    td->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);

    MTL::SizeAndAlign sa = impl_->params.device->heapTextureSizeAndAlign(td);

    metal::AllocResult r = impl_->memory.AllocImage(
        static_cast<uint32_t>(sa.size),
        static_cast<uint32_t>(sa.align),
        d.memory);
    if (!r.ok) {
        td->release();
        return Handle<Texture>::Null;
    }

    MTL::Heap* heap = impl_->memory.HeapHandle(r.heap_index);
    MTL::Texture* tex = heap->newTexture(td, r.offset);
    td->release();
    if (!tex) {
        impl_->memory.FreeImage(r.heap_index, r.alloc, nullptr,
                                impl_->frame_index + kFramesInFlight);
        return Handle<Texture>::Null;
    }

    Handle<Texture> h = impl_->textures.Acquire();
    Texture::Hot* hot = impl_->textures.GetHot(h);
    hot->api_view = tex;
    hot->descriptor_index = 0;

    Texture::Cold* cold = impl_->textures.GetCold(h);
    cold->alloc = r.alloc;
    cold->api_image = tex;
    cold->width = static_cast<uint32_t>(d.dimensions.x);
    cold->height = static_cast<uint32_t>(d.dimensions.y);
    cold->depth = 1;
    cold->mip_levels = d.mip_levels;
    cold->array_layers = d.array_layers;
    cold->format = d.format;
    cold->usage = d.usage;
    cold->mem_type = d.memory;
    cold->heap_buffer_index = r.heap_index;
    cold->debug_name = d.debug_name;

    if (!d.initial_data.empty()) {
        uint32_t bytes_per_row = static_cast<uint32_t>(d.initial_data.size()) /
                                 static_cast<uint32_t>(d.dimensions.y);
        MTL::Buffer* staging = impl_->params.device->newBuffer(
            d.initial_data.size(), MTL::ResourceStorageModeShared);
        if (staging) {
            std::memcpy(staging->contents(), d.initial_data.data(),
                        d.initial_data.size());
            MTL::CommandBuffer* cmd = impl_->params.queue->commandBuffer();
            MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
            blit->copyFromBuffer(
                staging, 0, bytes_per_row, 0,
                MTL::Size{static_cast<NS::UInteger>(d.dimensions.x),
                          static_cast<NS::UInteger>(d.dimensions.y), 1},
                tex, 0, 0, MTL::Origin{0, 0, 0});
            if (d.mip_levels > 1) {
                blit->generateMipmaps(tex);
            }
            blit->endEncoding();
            cmd->commit();
            cmd->waitUntilCompleted();
            staging->release();
        }
    }

    return h;
}

Handle<Sampler> ResourceManager::CreateSampler(const SamplerDesc& d) {
    MTL::SamplerDescriptor* desc = MTL::SamplerDescriptor::alloc()->init();
    desc->setSupportArgumentBuffers(true);
    desc->setMinFilter(to_mtl_min_mag(d.min_filter));
    desc->setMagFilter(to_mtl_min_mag(d.mag_filter));
    desc->setMipFilter(to_mtl_mip_filter(d.mip_filter));
    desc->setSAddressMode(to_mtl_address_mode(d.address_mode));
    desc->setTAddressMode(to_mtl_address_mode(d.address_mode));
    desc->setRAddressMode(to_mtl_address_mode(d.address_mode));
    if (d.max_anisotropy > 0.0f) {
        desc->setMaxAnisotropy(static_cast<NS::UInteger>(d.max_anisotropy));
    }
    if (d.max_lod > 0.0f) {
        desc->setLodMaxClamp(d.max_lod);
    }

    MTL::SamplerState* sampler = impl_->params.device->newSamplerState(desc);
    desc->release();
    if (!sampler) {
        return Handle<Sampler>::Null;
    }

    Handle<Sampler> h = impl_->samplers.Acquire();
    Sampler::Hot* hot = impl_->samplers.GetHot(h);
    hot->api_sampler = sampler;

    Sampler::Cold* cold = impl_->samplers.GetCold(h);
    cold->debug_name = d.debug_name;

    return h;
}

Handle<BindGroup> ResourceManager::CreateBindGroup(const BindGroupDesc&) {
    return Handle<BindGroup>::Null;
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
                            hot->api_view,
                            impl_->frame_index + kFramesInFlight);
    impl_->textures.Release(h);
}

void ResourceManager::Destroy(Handle<Sampler> h) {
    Sampler::Hot* hot = impl_->samplers.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_sampler) {
        hot->api_sampler->release();
        hot->api_sampler = nullptr;
    }
    impl_->samplers.Release(h);
}

void ResourceManager::Destroy(Handle<BindGroup> h) {
    impl_->bind_groups.Release(h);
}

void ResourceManager::Destroy(Handle<DynamicBuffers> h) {
    impl_->dynamic_buffers.Release(h);
}

void ResourceManager::Destroy(Handle<Shader> h) {
    Shader::Hot* hot = impl_->shaders.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_pso) {
        hot->api_pso->release();
        hot->api_pso = nullptr;
    }
    impl_->shaders.Release(h);
}

void ResourceManager::Destroy(Handle<Kernel> h) {
    Kernel::Hot* hot = impl_->kernels.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_pso) {
        hot->api_pso->release();
        hot->api_pso = nullptr;
    }
    impl_->kernels.Release(h);
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

Shader::Hot* ResourceManager::GetHot(Handle<Shader> h) {
    return impl_->shaders.GetHot(h);
}

Kernel::Hot* ResourceManager::GetHot(Handle<Kernel> h) {
    return impl_->kernels.GetHot(h);
}

Handle<Shader> ResourceManager::CreateShader(const ShaderDesc& d) {
    if (!d.api_pso) {
        return Handle<Shader>::Null;
    }
    Handle<Shader> h = impl_->shaders.Acquire();
    Shader::Hot* hot = impl_->shaders.GetHot(h);
    hot->api_pso = d.api_pso;
    Shader::Cold* cold = impl_->shaders.GetCold(h);
    cold->debug_name = d.debug_name;
    return h;
}

Handle<Kernel> ResourceManager::CreateKernel(const KernelDesc& d) {
    if (!d.api_pso) {
        return Handle<Kernel>::Null;
    }
    Handle<Kernel> h = impl_->kernels.Acquire();
    Kernel::Hot* hot = impl_->kernels.GetHot(h);
    hot->api_pso = d.api_pso;
    Kernel::Cold* cold = impl_->kernels.GetCold(h);
    cold->debug_name = d.debug_name;
    return h;
}

void* ResourceManager::BumpAllocate(uint32_t bytes, uint32_t align,
                                    Memory mem) {
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

MTL::Buffer* ResourceManager::GetMtlBuffer(Handle<Buffer> h,
                                            uint32_t* out_offset) {
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    if (!hot) {
        if (out_offset) {
            *out_offset = 0;
        }
        return nullptr;
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
    uint8_t* base = static_cast<uint8_t*>(
        impl_->memory.HeapMappedPtr(hot->heap_buffer_index));
    if (!base) {
        return nullptr;
    }
    return base + hot->offset_in_heap;
}

MTL::Heap* ResourceManager::GetMtlHeap(Handle<Buffer> h) {
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    if (!hot) {
        return nullptr;
    }
    return impl_->memory.HeapHandle(hot->heap_buffer_index);
}

MTL::Buffer* ResourceManager::GetBumpMasterBuffer(Memory mem) const {
    uint32_t hi = impl_->memory.BumpMasterHeapIndex(mem);
    if (hi == metal::kInvalidBlock) {
        return nullptr;
    }
    return impl_->memory.HeapMasterBuffer(hi);
}

Handle<BindGroup> ResourceManager::CreateBindGroupFromMtlBuffer(MTL::Buffer* buf,
                                                                  uint32_t offset) {
    Handle<BindGroup> h = impl_->bind_groups.Acquire();
    BindGroup::Hot* hot = impl_->bind_groups.GetHot(h);
    hot->api_descriptor_set = buf;
    hot->arg_buf_offset = offset;
    BindGroup::Cold* cold = impl_->bind_groups.GetCold(h);
    cold->debug_name = nullptr;
    return h;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
