// rhi/metal/resource_manager.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi/resource_manager.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <dispatch/dispatch.h>
#include <vector>

#include <Metal/Metal.hpp>
#include <stb_image_write.h>

#include "rhi/metal/memory_allocator.hpp"
#include "rhi/command_recorder.hpp"
#include "rhi/metal/command_recorder_impl.hpp"
#include "rhi/device.hpp"
#include "rhi/metal/internal/device_impl.hpp"
#include "rhi/allocator.hpp"
#include "rhi/metal/internal/allocator_impl.hpp"
#include "rhi/swap_chain.hpp"
#include "gpu_scene_registry.hpp"

namespace cairns::rhi {

struct ResourceManager::Impl {
    BackendInitParams params;
    Allocator* alloc = nullptr;  // borrowed; owns the MemoryAllocator
    std::filesystem::path dump_path;

    // rhi-owned per-frame render state (was app-registered via MtlRegisterFrame).
    void* frame_semaphore = nullptr;  // dispatch_semaphore_t
    MTL::RenderPassDescriptor* render_pass_desc = nullptr;
    MTL::DepthStencilState* depth_stencil = nullptr;
    Handle<Texture> msaa_handle = Handle<Texture>::Null;
    Handle<Texture> depth_handle = Handle<Texture>::Null;

    Pool<Buffer> buffers;
    Pool<Texture> textures;
    Pool<Sampler> samplers;
    Pool<BindGroup> bind_groups;
    Pool<DynamicBuffers> dynamic_buffers;
    Pool<Shader> shaders;
    Pool<Kernel> kernels;

    uint32_t frame_index = 1;

    // Bindless registry builder (one in-flight at a time).
    MTL::ArgumentEncoder* bindless_encoder = nullptr;
    uint32_t bindless_tex_base = 0;
    uint32_t bindless_attr_base = 0;
    uint32_t bindless_samp_base = 0;
    uint32_t bindless_num_tex = 0;
    uint32_t bindless_num_attr = 0;
    uint32_t bindless_num_samp = 0;
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
        case Format::kRg32F:      return MTL::PixelFormatRG32Float;
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
    if (!impl_) {
        return;
    }
    if (impl_->depth_stencil) {
        impl_->depth_stencil->release();
    }
    if (impl_->render_pass_desc) {
        impl_->render_pass_desc->release();
    }
    // delete impl_ runs the memory allocator dtor, freeing device heaps. The
    // device/queue release is owned by Device::Deinit, which the engine calls
    // AFTER this (so heaps free against a live device).
    delete impl_;
    impl_ = nullptr;
}


bool ResourceManager::InitDevice(Device& dev, Allocator& alloc) {
    impl_ = new Impl();
    // Mirror the device/queue owned by Device; borrow the Allocator (already
    // Init'd, owns the MemoryAllocator). Device owns device/queue teardown.
    impl_->params.device = dev.impl_->device;
    impl_->params.queue = dev.impl_->queue;
    impl_->alloc = &alloc;
    impl_->frame_semaphore = dispatch_semaphore_create(kFramesInFlight);
    {
        MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
        dsd->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        dsd->setDepthWriteEnabled(true);
        impl_->depth_stencil = impl_->params.device->newDepthStencilState(dsd);
        dsd->release();
    }
    return true;
}

bool ResourceManager::InitSwapChain(SwapChain& sc, SDL_Window* window) {
    return sc.Init(impl_->params.device, window);
}

// Metal MSAA/depth targets share the texture Pool index space with scene
// textures, and the bindless registry asserts scene-texture handle.index ==
// bindless slot. So these MUST be created AFTER scene textures (unlike Vulkan,
// whose depth/MSAA are raw images created in sc.Init). Call post scene load.
bool ResourceManager::InitFrameTargets(SwapChain& sc) {
    constexpr uint32_t kSampleCount = 4;
    const int32_t w = static_cast<int32_t>(sc.Width());
    const int32_t h = static_cast<int32_t>(sc.Height());
    {
        TextureDesc d;
        d.dimensions = {w, h, 1};
        d.format = Format::kBgra8Unorm;
        d.sample_count = kSampleCount;
        d.usage = kTexUsageColorTarget;
        d.memory = Memory::kDefault;
        impl_->msaa_handle = CreateTexture(d);
        if (impl_->msaa_handle.IsNull()) {
            return false;
        }
    }
    {
        TextureDesc d;
        d.dimensions = {w, h, 1};
        d.format = Format::kD32F;
        d.sample_count = kSampleCount;
        d.usage = kTexUsageDepthTarget;
        d.memory = Memory::kDefault;
        impl_->depth_handle = CreateTexture(d);
        if (impl_->depth_handle.IsNull()) {
            return false;
        }
    }
    MTL::Texture* msaa = GetHot(impl_->msaa_handle)->api_view;
    MTL::Texture* depth = GetHot(impl_->depth_handle)->api_view;
    return InitRenderPassDescriptor(impl_->render_pass_desc, msaa, depth, sc);
}


Handle<Buffer> ResourceManager::CreateBuffer(const BufferDesc& d) {
    metal::AllocResult r =
        impl_->alloc->impl_->memory.AllocBuffer(d.byte_size, d.usage, d.memory, 16);
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
            void* staging = impl_->alloc->impl_->memory.BumpAllocate(
                static_cast<uint32_t>(d.initial_data.size()), 16,
                Memory::kUpload);
            if (staging) {
                std::memcpy(staging, d.initial_data.data(),
                            d.initial_data.size());
                uint32_t src_off = impl_->alloc->impl_->memory.BumpOffset(staging);
                uint32_t src_hi =
                    impl_->alloc->impl_->memory.BumpMasterHeapIndex(Memory::kUpload);
                MTL::Buffer* src = impl_->alloc->impl_->memory.HeapMasterBuffer(src_hi);
                MTL::Buffer* dst_buf =
                    impl_->alloc->impl_->memory.HeapMasterBuffer(r.heap_index);
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

    metal::AllocResult r = impl_->alloc->impl_->memory.AllocImage(
        static_cast<uint32_t>(sa.size),
        static_cast<uint32_t>(sa.align),
        d.memory);
    if (!r.ok) {
        td->release();
        return Handle<Texture>::Null;
    }

    MTL::Heap* heap = impl_->alloc->impl_->memory.HeapHandle(r.heap_index);
    MTL::Texture* tex = heap->newTexture(td, r.offset);
    td->release();
    if (!tex) {
        impl_->alloc->impl_->memory.FreeImage(r.heap_index, r.alloc, nullptr,
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
    impl_->alloc->impl_->memory.FreeBuffer(hot->heap_buffer_index, cold->alloc,
                             impl_->frame_index + kFramesInFlight);
    impl_->buffers.Release(h);
}

void ResourceManager::Destroy(Handle<Texture> h) {
    Texture::Hot* hot = impl_->textures.GetHot(h);
    Texture::Cold* cold = impl_->textures.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    impl_->alloc->impl_->memory.FreeImage(cold->heap_buffer_index, cold->alloc,
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

namespace {

std::string read_text_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        return "";
    }
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

MTL::Library* compile_metal_library(MTL::Device* device, const std::string& path) {
    std::string src = read_text_file(path);
    if (src.empty()) {
        std::cerr << "rhi/metal: failed to load shader source: " << path << std::endl;
        return nullptr;
    }
    NS::String* ns_src =
        NS::String::string(src.c_str(), NS::StringEncoding::UTF8StringEncoding);
    MTL::CompileOptions* opts = MTL::CompileOptions::alloc()->init();
    opts->setLanguageVersion(MTL::LanguageVersion::LanguageVersion2_4);
    NS::Error* err = nullptr;
    MTL::Library* lib = device->newLibrary(ns_src, opts, &err);
    if (err) {
        std::cerr << "rhi/metal: shader compile error: "
                  << err->localizedDescription()->utf8String() << std::endl;
    }
    opts->release();
    return lib;
}

MTL::VertexFormat to_mtl_vertex_format(Format f) {
    switch (f) {
        case Format::kR32F:    return MTL::VertexFormatFloat;
        case Format::kRg32F:   return MTL::VertexFormatFloat2;
        case Format::kRgba32F: return MTL::VertexFormatFloat4;
        default:               return MTL::VertexFormatFloat4;
    }
}

MTL::BlendFactor to_mtl_blend_factor(BlendFactor b) {
    switch (b) {
        case BlendFactor::kZero:             return MTL::BlendFactorZero;
        case BlendFactor::kOne:              return MTL::BlendFactorOne;
        case BlendFactor::kSrcAlpha:         return MTL::BlendFactorSourceAlpha;
        case BlendFactor::kOneMinusSrcAlpha: return MTL::BlendFactorOneMinusSourceAlpha;
        default:                             return MTL::BlendFactorOne;
    }
}

// Resolve a logical shader name to its .metal file + entry function names.
struct MetalShaderInfo {
    const char* file;
    const char* vs;
    const char* fs;
    const char* cs;
};
MetalShaderInfo resolve_metal_shader(const char* logical) {
    if (std::strcmp(logical, "unlit") == 0) {
        return {"unlit.metal", "cube::vertexShader", "cube::fragmentShader", nullptr};
    }
    // "particle"
    return {"particle.metal", "particle_vertex", "particle_fragment", "particle_compute"};
}

}  // namespace

Handle<Shader> ResourceManager::CreateGraphicsPipeline(
    const GraphicsPipelineDesc& desc) {
    MTL::Device* device = impl_->params.device;
    const MetalShaderInfo info = resolve_metal_shader(desc.logical_shader);
    const std::filesystem::path dir = desc.shader_dir ? desc.shader_dir : "";
    MTL::Library* lib = compile_metal_library(device, (dir / info.file).string());
    if (!lib) {
        return Handle<Shader>::Null;
    }

    MTL::Function* vfn =
        lib->newFunction(NS::String::string(info.vs, NS::ASCIIStringEncoding));
    MTL::Function* ffn =
        lib->newFunction(NS::String::string(info.fs, NS::ASCIIStringEncoding));
    if (!vfn || !ffn) {
        lib->release();
        return Handle<Shader>::Null;
    }

    MTL::RenderPipelineDescriptor* rpd =
        MTL::RenderPipelineDescriptor::alloc()->init();
    rpd->setVertexFunction(vfn);
    rpd->setFragmentFunction(ffn);

    MTL::RenderPipelineColorAttachmentDescriptor* color =
        rpd->colorAttachments()->object(0);
    color->setPixelFormat(to_mtl_pixel_format(desc.color_format));
    if (desc.blend.enable) {
        color->setBlendingEnabled(true);
        color->setSourceRGBBlendFactor(to_mtl_blend_factor(desc.blend.src_color));
        color->setDestinationRGBBlendFactor(to_mtl_blend_factor(desc.blend.dst_color));
        color->setSourceAlphaBlendFactor(to_mtl_blend_factor(desc.blend.src_alpha));
        color->setDestinationAlphaBlendFactor(to_mtl_blend_factor(desc.blend.dst_alpha));
    }
    rpd->setSampleCount(desc.sample_count);
    rpd->setDepthAttachmentPixelFormat(to_mtl_pixel_format(desc.depth_format));
    if (desc.topology == PrimitiveTopology::kPointList) {
        rpd->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassPoint);
    }

    if (!desc.vertex_attributes.empty()) {
        MTL::VertexDescriptor* vd = MTL::VertexDescriptor::alloc()->init();
        for (size_t i = 0; i < desc.vertex_attributes.size(); ++i) {
            const VertexInputAttribute& a = desc.vertex_attributes[i];
            MTL::VertexAttributeDescriptor* ad = vd->attributes()->object(a.location);
            ad->setFormat(to_mtl_vertex_format(a.format));
            ad->setOffset(a.offset);
            ad->setBufferIndex(a.buffer_slot);
        }
        for (size_t i = 0; i < desc.vertex_buffers.size(); ++i) {
            const VertexBufferLayout& b = desc.vertex_buffers[i];
            MTL::VertexBufferLayoutDescriptor* ld = vd->layouts()->object(b.buffer_slot);
            ld->setStride(b.stride);
            ld->setStepFunction(MTL::VertexStepFunctionPerVertex);
            ld->setStepRate(1);
        }
        rpd->setVertexDescriptor(vd);
        vd->release();
    }

    NS::Error* err = nullptr;
    MTL::RenderPipelineState* pso = device->newRenderPipelineState(rpd, &err);
    rpd->release();
    vfn->release();
    ffn->release();
    lib->release();
    if (!pso) {
        std::cerr << "rhi/metal: newRenderPipelineState failed" << std::endl;
        return Handle<Shader>::Null;
    }

    Handle<Shader> h = impl_->shaders.Acquire();
    impl_->shaders.GetHot(h)->api_pso = pso;
    impl_->shaders.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Handle<Kernel> ResourceManager::CreateComputePipeline(
    const ComputePipelineDesc& desc) {
    MTL::Device* device = impl_->params.device;
    const MetalShaderInfo info = resolve_metal_shader(desc.logical_shader);
    const std::filesystem::path dir = desc.shader_dir ? desc.shader_dir : "";
    MTL::Library* lib = compile_metal_library(device, (dir / info.file).string());
    if (!lib) {
        return Handle<Kernel>::Null;
    }
    MTL::Function* fn =
        lib->newFunction(NS::String::string(info.cs, NS::ASCIIStringEncoding));
    if (!fn) {
        lib->release();
        return Handle<Kernel>::Null;
    }
    NS::Error* err = nullptr;
    MTL::ComputePipelineState* cps = device->newComputePipelineState(fn, &err);
    fn->release();
    lib->release();
    if (!cps) {
        std::cerr << "rhi/metal: newComputePipelineState failed" << std::endl;
        return Handle<Kernel>::Null;
    }
    Handle<Kernel> h = impl_->kernels.Acquire();
    impl_->kernels.GetHot(h)->api_pso = cps;
    impl_->kernels.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

void ResourceManager::BeginFrame() {
    impl_->frame_index++;
    impl_->alloc->AdvanceFrame(impl_->frame_index);
}


uint32_t ResourceManager::GetBufferByteSize(Handle<Buffer> h) const {
    Buffer::Cold* cold = impl_->buffers.GetCold(h);
    if (!cold) {
        return 0;
    }
    return cold->size_bytes;
}

uint32_t ResourceManager::BufferBaseOffset(Handle<Buffer> h) {
    uint32_t off = 0;
    GetMtlBuffer(h, &off);
    return off;
}

MTL::Buffer* ResourceManager::GetMtlBuffer(Handle<Buffer> h,
                                            uint32_t* out_offset) {
    if (h.generation == 0) {  // bump-master sentinel from BumpMasterBuffer()
        if (out_offset) {
            *out_offset = 0;
        }
        return impl_->alloc->impl_->memory.HeapMasterBuffer(h.index);
    }
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
    return impl_->alloc->impl_->memory.HeapMasterBuffer(hot->heap_buffer_index);
}

uint8_t* ResourceManager::MappedPtr(Handle<Buffer> h) {
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    if (!hot) {
        return nullptr;
    }
    uint8_t* base = static_cast<uint8_t*>(
        impl_->alloc->impl_->memory.HeapMappedPtr(hot->heap_buffer_index));
    if (!base) {
        return nullptr;
    }
    return base + hot->offset_in_heap;
}

MTL::Buffer* ResourceManager::GetBumpMasterBuffer(Memory mem) const {
    uint32_t hi = impl_->alloc->impl_->memory.BumpMasterHeapIndex(mem);
    if (hi == metal::kInvalidBlock) {
        return nullptr;
    }
    return impl_->alloc->impl_->memory.HeapMasterBuffer(hi);
}

Handle<BindGroup> ResourceManager::CreateBindlessRegistry(
    const BindlessRegistryDesc& desc) {
    MTL::Device* device = impl_->params.device;

    auto* texArg = MTL::ArgumentDescriptor::alloc()->init();
    texArg->setDataType(MTL::DataTypeTexture);
    texArg->setIndex(desc.texture_slot);
    texArg->setArrayLength(desc.max_textures);
    texArg->setAccess(MTL::ArgumentAccessReadOnly);

    auto* attrArg = MTL::ArgumentDescriptor::alloc()->init();
    attrArg->setDataType(MTL::DataTypePointer);
    attrArg->setIndex(desc.attr_buffer_slot);
    attrArg->setArrayLength(desc.max_attr_buffers);
    attrArg->setAccess(MTL::ArgumentAccessReadOnly);

    auto* sampArg = MTL::ArgumentDescriptor::alloc()->init();
    sampArg->setDataType(MTL::DataTypeSampler);
    sampArg->setIndex(desc.sampler_slot);
    sampArg->setArrayLength(desc.max_samplers);
    sampArg->setAccess(MTL::ArgumentAccessReadOnly);

    NS::Array* args = NS::Array::array((NS::Object*[]){texArg, attrArg, sampArg}, 3);
    MTL::ArgumentEncoder* arg_encoder = device->newArgumentEncoder(args);

    BufferDesc bd;
    bd.byte_size = static_cast<uint32_t>(arg_encoder->encodedLength());
    bd.usage = kUsageUniform | kUsageStorage;
    bd.memory = Memory::kUpload;
    Handle<Buffer> arg_buf_h = CreateBuffer(bd);
    uint32_t arg_off = 0;
    MTL::Buffer* arg_buf = GetMtlBuffer(arg_buf_h, &arg_off);
    arg_encoder->setArgumentBuffer(arg_buf, arg_off);

    impl_->bindless_encoder = arg_encoder;
    impl_->bindless_tex_base = desc.texture_slot;
    impl_->bindless_attr_base = desc.attr_buffer_slot;
    impl_->bindless_samp_base = desc.sampler_slot;
    impl_->bindless_num_tex = 0;
    impl_->bindless_num_attr = 0;
    impl_->bindless_num_samp = 0;

    Handle<BindGroup> h = impl_->bind_groups.Acquire();
    BindGroup::Hot* hot = impl_->bind_groups.GetHot(h);
    hot->api_descriptor_set = arg_buf;
    hot->arg_buf_offset = arg_off;
    impl_->bind_groups.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

uint32_t ResourceManager::BindlessAddTexture(Handle<BindGroup>, Handle<Texture> tex) {
    MTL::Texture* t = impl_->textures.GetHot(tex)->api_view;
    const uint32_t slot = impl_->bindless_num_tex;
    impl_->bindless_encoder->setTexture(t, impl_->bindless_tex_base + slot);
    impl_->bindless_num_tex = slot + 1;
    return slot;
}

uint32_t ResourceManager::BindlessAddAttrBuffer(Handle<BindGroup>, Handle<Buffer> buf) {
    uint32_t off = 0;
    MTL::Buffer* b = GetMtlBuffer(buf, &off);
    const uint32_t slot = impl_->bindless_num_attr;
    impl_->bindless_encoder->setBuffer(b, off, impl_->bindless_attr_base + slot);
    impl_->bindless_num_attr = slot + 1;
    return slot;
}

uint32_t ResourceManager::BindlessAddSampler(Handle<BindGroup>, Handle<Sampler> samp) {
    MTL::SamplerState* s = impl_->samplers.GetHot(samp)->api_sampler;
    const uint32_t slot = impl_->bindless_num_samp;
    impl_->bindless_encoder->setSamplerState(s, impl_->bindless_samp_base + slot);
    impl_->bindless_num_samp = slot + 1;
    return slot;
}

void ResourceManager::BindlessFinalize(Handle<BindGroup>) {
    if (impl_->bindless_encoder) {
        impl_->bindless_encoder->release();
        impl_->bindless_encoder = nullptr;
    }
}

void ResourceManager::SetDumpPath(const std::filesystem::path& path) {
    impl_->dump_path = path;
}

FrameContext ResourceManager::BeginFrame(SwapChain& sc) {
    dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(impl_->frame_semaphore),
                            DISPATCH_TIME_FOREVER);
    BeginFrame();  // bump ring reset

    sc.NextDrawable();
    MTL::Texture* msaa = GetHot(impl_->msaa_handle)->api_view;
    MTL::Texture* depth = GetHot(impl_->depth_handle)->api_view;
    UpdateRenderPassDescriptor(impl_->render_pass_desc, msaa, depth, sc);

    MTL::CommandBuffer* cmd = impl_->params.queue->commandBuffer();
    dispatch_semaphore_t sem = static_cast<dispatch_semaphore_t>(impl_->frame_semaphore);
    cmd->addCompletedHandler([sem](MTL::CommandBuffer*) { dispatch_semaphore_signal(sem); });

    FrameContext fc;
    fc.frame_index = 0;
    fc.swapchain_image_index = 0;
    fc.cmd.impl_ = new CommandRecorder::Impl{this,           &sc,  cmd, nullptr,
                                             impl_->render_pass_desc,
                                             impl_->depth_stencil};
    return fc;
}

void ResourceManager::EndFrame(FrameContext& fc) {
    CommandRecorder::Impl* ri = fc.cmd.impl_;
    MTL::CommandBuffer* cmd = ri->cmd;

    if (!impl_->dump_path.empty()) {
        MTL::Texture* drawableTex = ri->sc->GetDrawable()->texture();
        const NS::UInteger w = drawableTex->width();
        const NS::UInteger h = drawableTex->height();
        const NS::UInteger bytesPerRow = w * 4;
        const NS::UInteger bufSize = bytesPerRow * h;
        MTL::Buffer* readback = impl_->params.device->newBuffer(bufSize, MTL::ResourceStorageModeShared);
        MTL::BlitCommandEncoder* blitEnc = cmd->blitCommandEncoder();
        blitEnc->copyFromTexture(drawableTex, 0, 0, MTL::Origin{0, 0, 0}, MTL::Size{w, h, 1},
                                 readback, 0, bytesPerRow, 0);
        blitEnc->endEncoding();
        cmd->presentDrawable(ri->sc->GetDrawable());
        cmd->commit();
        cmd->waitUntilCompleted();
        std::vector<uint8_t> rgba(bufSize);
        const uint8_t* bgra = static_cast<const uint8_t*>(readback->contents());
        for (NS::UInteger i = 0; i < w * h; ++i) {
            rgba[i * 4 + 0] = bgra[i * 4 + 2];
            rgba[i * 4 + 1] = bgra[i * 4 + 1];
            rgba[i * 4 + 2] = bgra[i * 4 + 0];
            rgba[i * 4 + 3] = bgra[i * 4 + 3];
        }
        stbi_write_png(impl_->dump_path.string().c_str(), static_cast<int>(w),
                       static_cast<int>(h), 4, rgba.data(), static_cast<int>(bytesPerRow));
        readback->release();
        impl_->dump_path.clear();
    } else {
        cmd->presentDrawable(ri->sc->GetDrawable());
        cmd->commit();
    }

    delete ri;
    fc.cmd.impl_ = nullptr;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
