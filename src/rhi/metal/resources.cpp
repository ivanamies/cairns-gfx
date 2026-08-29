// rhi/metal/resources.cpp  (Phase 0g-a: pools + destroy/get + frame counter)

#include "util/define.hpp"

#if CAIRNS_METAL

#include <Metal/Metal.hpp>

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
        case Format::kRg32F:      return MTL::PixelFormatRG32Float;
        case Format::kRgba32F:    return MTL::PixelFormatRGBA32Float;
        case Format::kR32Uint:    return MTL::PixelFormatR32Uint;
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

Resources::~Resources() { Deinit(); }

bool Resources::Init(Device& device) {
    if (inited_) {
        return true;
    }
    plat.device_ = device.plat.device_;
    plat.queue_ = device.plat.queue_;
    plat.resources_ = this;
    inited_ = true;
    return true;
}

void Resources::Deinit() {
    if (!inited_) {
        return;
    }
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
        cold->heap_buffer_index, cold->alloc, hot->api_view,
        plat.frame_index_ + kFramesInFlight);
    textures.Release(h);
}

void Resources::Destroy(Handle<Sampler> h) {
    Sampler::Hot* hot = samplers.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_sampler) {
        hot->api_sampler->release();
        hot->api_sampler = nullptr;
    }
    samplers.Release(h);
}

void Resources::Destroy(Handle<BindGroup> h) { bind_groups.Release(h); }

void Resources::Destroy(Handle<DynamicBuffers> h) { dynamic_buffers.Release(h); }

void Resources::Destroy(Handle<Shader> h) {
    Shader::Hot* hot = shaders.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_pso) {
        hot->api_pso->release();
        hot->api_pso = nullptr;
    }
    shaders.Release(h);
}

void Resources::Destroy(Handle<Kernel> h) {
    Kernel::Hot* hot = kernels.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_pso) {
        hot->api_pso->release();
        hot->api_pso = nullptr;
    }
    kernels.Release(h);
}

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
    metal::AllocResult r =
        alloc.plat.memory_.AllocBuffer(d.byte_size, d.usage, d.memory, 16);
    assert(r.ok && "AllocBuffer failed");
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
    MTL::Buffer* dst_buf =
        alloc.plat.memory_.HeapMasterBuffer(hot->heap_buffer_index);
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
        MTL::Buffer* src = alloc.plat.memory_.HeapMasterBuffer(src_hi);
        MTL::CommandBuffer* cmd = plat.queue_->commandBuffer();
        MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
        blit->copyFromBuffer(src, src_off, dst_buf,
                             hot->offset_in_heap + dst_offset + done, chunk);
        blit->endEncoding();
        cmd->commit();
        cmd->waitUntilCompleted();
        alloc.plat.memory_.BumpRestoreCursor(Memory::kUpload, saved_cursor);
        done += chunk;
    }
}

Handle<Texture> Resources::CreateTexture(Allocator& alloc, const TextureDesc& d) {
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

    MTL::SizeAndAlign sa = plat.device_->heapTextureSizeAndAlign(td);

    metal::AllocResult r = alloc.plat.memory_.AllocImage(
        static_cast<uint32_t>(sa.size),
        static_cast<uint32_t>(sa.align),
        d.memory);
    assert(r.ok && "AllocImage failed");
    if (!r.ok) {
        td->release();
        return Handle<Texture>::Null;
    }

    MTL::Heap* heap = alloc.plat.memory_.HeapHandle(r.heap_index);
    assert(heap && "null heap for image");
    MTL::Texture* tex = heap->newTexture(td, r.offset);
    td->release();
    assert(tex && "heap newTexture failed");
    if (!tex) {
        alloc.plat.memory_.FreeImage(r.heap_index, r.alloc, nullptr,
                                plat.frame_index_ + kFramesInFlight);
        return Handle<Texture>::Null;
    }

    Handle<Texture> h = textures.Acquire();
    Texture::Hot* hot = textures.GetHot(h);
    hot->api_view = tex;

    Texture::Cold* cold = textures.GetCold(h);
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
        MTL::Buffer* staging = plat.device_->newBuffer(
            d.initial_data.size(), MTL::ResourceStorageModeShared);
        if (staging) {
            std::memcpy(staging->contents(), d.initial_data.data(),
                        d.initial_data.size());
            MTL::CommandBuffer* cmd = plat.queue_->commandBuffer();
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

Handle<Sampler> Resources::CreateSampler(const SamplerDesc& d) {
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

    MTL::SamplerState* sampler = plat.device_->newSamplerState(desc);
    desc->release();
    if (!sampler) {
        return Handle<Sampler>::Null;
    }

    Handle<Sampler> h = samplers.Acquire();
    Sampler::Hot* hot = samplers.GetHot(h);
    hot->api_sampler = sampler;

    Sampler::Cold* cold = samplers.GetCold(h);
    cold->debug_name = d.debug_name;

    return h;
}

Handle<BindGroup> Resources::CreateBindGroup(const BindGroupDesc& desc) {
    // set 2 = a small per-material argument buffer: texture @ id 0, sampler @ id 1.
    auto* texArg = MTL::ArgumentDescriptor::alloc()->init();
    texArg->setDataType(MTL::DataTypeTexture);
    texArg->setIndex(0);
    texArg->setAccess(MTL::ArgumentAccessReadOnly);
    auto* sampArg = MTL::ArgumentDescriptor::alloc()->init();
    sampArg->setDataType(MTL::DataTypeSampler);
    sampArg->setIndex(1);
    sampArg->setAccess(MTL::ArgumentAccessReadOnly);
    NS::Array* args = NS::Array::array((NS::Object*[]){texArg, sampArg}, 2);
    MTL::ArgumentEncoder* enc = plat.device_->newArgumentEncoder(args);

    MTL::Buffer* buf = plat.device_->newBuffer(enc->encodedLength(),
                                          MTL::ResourceStorageModeShared);
    enc->setArgumentBuffer(buf, 0);
    if (!desc.textures.empty()) {
        enc->setTexture(textures.GetHot(desc.textures[0].texture)->api_view, 0);
    }
    if (!desc.samplers.empty()) {
        enc->setSamplerState(samplers.GetHot(desc.samplers[0].sampler)->api_sampler, 1);
    }
    enc->release();
    texArg->release();
    sampArg->release();

    Handle<BindGroup> h = bind_groups.Acquire();
    BindGroup::Hot* hot = bind_groups.GetHot(h);
    hot->api_descriptor_set = buf;
    hot->arg_buf_offset = 0;
    bind_groups.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Handle<BindGroup> Resources::CreateSkinGroupA(Allocator& /*alloc*/,
                                                Frames& /*frames*/,
                                                const BindGroupDesc& /*desc*/) {
    // Metal: compute path binds buffers directly per batch via
    // setBuffer:offset:atIndex: in DispatchSkinBatches; no descriptor sets.
    return Handle<BindGroup>::Null;
}

Handle<DynamicBuffers> Resources::CreateDynamicBuffers(
    const DynamicBuffersDesc& desc) {
    Handle<DynamicBuffers> h = dynamic_buffers.Acquire();
    DynamicBuffers::Hot* hot = dynamic_buffers.GetHot(h);
    DynamicBuffers::Cold* cold = dynamic_buffers.GetCold(h);
    hot->binding_count = static_cast<uint8_t>(desc.bindings.size());
    cold->layout.assign(desc.bindings.begin(), desc.bindings.end());
    cold->debug_name = desc.debug_name;
    return h;
}

// #222 Phase D.2 (metal): same as the minimal path; no descriptor objects
// to build. The Allocator& + Frames& params are unused but kept so the
// caller can be backend-agnostic.
Handle<DynamicBuffers> Resources::CreateDynamicBuffers(
    Allocator& /*alloc*/, Frames& /*frames*/, const DynamicBuffersDesc& desc) {
    return CreateDynamicBuffers(desc);
}

uint32_t Resources::BufferBaseOffset(Allocator& alloc, Handle<Buffer> h) {
    uint32_t off = 0;
    plat.GetMtlBuffer(alloc, h, &off);
    return off;
}

MTL::Buffer* ResourcesPlat::GetMtlBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t* out_offset) {
    if (h.generation == 0) {  // bump-master sentinel from BumpMasterBuffer()
        if (out_offset) {
            *out_offset = 0;
        }
        return alloc.plat.memory_.HeapMasterBuffer(h.index);
    }
    Buffer::Hot* hot = resources_->buffers.GetHot(h);
    assert(hot && "GetMtlBuffer: invalid buffer handle");
    if (!hot) {
        if (out_offset) {
            *out_offset = 0;
        }
        return nullptr;
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
    uint8_t* base = static_cast<uint8_t*>(
        alloc.plat.memory_.HeapMappedPtr(hot->heap_buffer_index));
    if (!base) {
        return nullptr;
    }
    return base + hot->offset_in_heap;
}

MTL::Buffer* ResourcesPlat::GetBumpMasterBuffer(Allocator& alloc, Memory mem) const {
    uint32_t hi = alloc.plat.memory_.BumpMasterHeapIndex(mem);
    if (hi == metal::kInvalidBlock) {
        return nullptr;
    }
    return alloc.plat.memory_.HeapMasterBuffer(hi);
}

// Blit texture into a Shared MTL::Buffer, wait, swizzle BGRA→RGBA into |out|.
// Apple origin is top-left so no Y-flip (matches the windowed dump contract).
bool Resources::ReadBackTextureRgba(Handle<Texture> h,
                                      std::vector<uint8_t>& out_rgba,
                                      uint32_t& out_w, uint32_t& out_h) {
    Texture::Hot* hot = textures.GetHot(h);
    if (!hot || !hot->api_view) {
        return false;
    }
    MTL::Texture* tex = hot->api_view;
    const NS::UInteger w = tex->width();
    const NS::UInteger hgt = tex->height();
    const NS::UInteger bpr = w * 4;
    const NS::UInteger buf_size = bpr * hgt;
    MTL::Buffer* readback = plat.device_->newBuffer(
        buf_size, MTL::ResourceStorageModeShared);
    if (!readback) {
        return false;
    }
    MTL::CommandBuffer* cb = plat.queue_->commandBuffer();
    MTL::BlitCommandEncoder* blit = cb->blitCommandEncoder();
    blit->copyFromTexture(tex, 0, 0, MTL::Origin{0, 0, 0},
                          MTL::Size{w, hgt, 1}, readback, 0, bpr, 0);
    blit->endEncoding();
    cb->commit();
    cb->waitUntilCompleted();
    out_rgba.resize(buf_size);
    const uint8_t* bgra = static_cast<const uint8_t*>(readback->contents());
    for (NS::UInteger i = 0; i < w * hgt; ++i) {
        out_rgba[i * 4 + 0] = bgra[i * 4 + 2];
        out_rgba[i * 4 + 1] = bgra[i * 4 + 1];
        out_rgba[i * 4 + 2] = bgra[i * 4 + 0];
        out_rgba[i * 4 + 3] = bgra[i * 4 + 3];
    }
    out_w = static_cast<uint32_t>(w);
    out_h = static_cast<uint32_t>(hgt);
    readback->release();
    return true;
}

// #207 single-texel R32U readback for pick. Apple GPUs (especially the
// M-series) reject small-region blits with under-aligned destination row
// pitches; copy the whole row that contains (y) and index in. R32U row
// = width * 4 bytes, naturally aligned. Caller drains in-flight work
// before calling.
bool Resources::ReadBackTextureR32UTexel(Handle<Texture> h, uint32_t x,
                                         uint32_t y, uint32_t& out_value) {
    Texture::Hot* hot = textures.GetHot(h);
    if (!hot || !hot->api_view) {
        return false;
    }
    MTL::Texture* tex = hot->api_view;
    const NS::UInteger w = tex->width();
    const NS::UInteger hgt = tex->height();
    if (x >= w || y >= hgt) {
        return false;
    }
    const NS::UInteger bpr = w * 4;  // R32U row.
    MTL::Buffer* readback = plat.device_->newBuffer(
        bpr, MTL::ResourceStorageModeShared);
    if (!readback) {
        return false;
    }
    MTL::CommandBuffer* cb = plat.queue_->commandBuffer();
    MTL::BlitCommandEncoder* blit = cb->blitCommandEncoder();
    blit->copyFromTexture(tex, 0, 0, MTL::Origin{0, y, 0},
                          MTL::Size{w, 1, 1}, readback, 0, bpr, 0);
    blit->endEncoding();
    cb->commit();
    cb->waitUntilCompleted();
    const uint32_t* p = static_cast<const uint32_t*>(readback->contents());
    out_value = p[x];
    readback->release();
    return true;
}

// One-shot clear via a render-pass with loadActionClear / storeActionStore.
bool Resources::ClearColorTexture(Handle<Texture> h, const float color[4]) {
    Texture::Hot* hot = textures.GetHot(h);
    if (!hot || !hot->api_view) {
        return false;
    }
    MTL::RenderPassDescriptor* rpd =
        MTL::RenderPassDescriptor::renderPassDescriptor();
    MTL::RenderPassColorAttachmentDescriptor* att = rpd->colorAttachments()->object(0);
    att->setTexture(hot->api_view);
    att->setLoadAction(MTL::LoadActionClear);
    att->setStoreAction(MTL::StoreActionStore);
    att->setClearColor(MTL::ClearColor(color[0], color[1], color[2], color[3]));
    MTL::CommandBuffer* cb = plat.queue_->commandBuffer();
    MTL::RenderCommandEncoder* enc = cb->renderCommandEncoder(rpd);
    enc->endEncoding();
    cb->commit();
    cb->waitUntilCompleted();
    return true;
}

SwapResolveTarget Resources::MakeSurfacelessSwapResolveTarget(
    Handle<Texture> h, uint32_t w, uint32_t h_px) {
    Texture::Hot* hot = textures.GetHot(h);
    if (!hot || !hot->api_view) {
        return SwapResolveTarget{};
    }
    return MakeSwapResolveTargetFromTexture(hot->api_view, w, h_px);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
