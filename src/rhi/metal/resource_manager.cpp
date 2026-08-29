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
#include "rhi/resources.hpp"
#include "rhi/bindless.hpp"
#include "rhi/frames.hpp"
#include "rhi/swap_chain.hpp"
#include "gpu_scene_registry.hpp"

namespace cairns::rhi {

struct ResourceManager::Impl {
    BackendInitParams params;
    Allocator* alloc = nullptr;     // borrowed; owns the MemoryAllocator
    Resources* res = nullptr;       // borrowed; owns the 7 pools + frame counter
    Bindless* bindless = nullptr;   // borrowed
    Frames* frames = nullptr;       // borrowed
};

namespace {

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

}  // namespace

ResourceManager::~ResourceManager() {
    Deinit();
}

void ResourceManager::Deinit() {
    if (!impl_) {
        return;
    }
    // delete impl_ runs the memory allocator dtor, freeing device heaps. The
    // device/queue release is owned by Device::Deinit, which the engine calls
    // AFTER this (so heaps free against a live device).
    delete impl_;
    impl_ = nullptr;
}


bool ResourceManager::InitDevice(Device& dev, Allocator& alloc, Resources& res,
                                 Bindless& bindless, Frames& frames) {
    impl_ = new Impl();
    // Mirror the device/queue owned by Device; borrow the Allocator + Resources
    // (already Init'd). Device owns device/queue teardown.
    impl_->params.device = dev.impl_->device;
    impl_->params.queue = dev.impl_->queue;
    impl_->alloc = &alloc;
    impl_->res = &res;
    impl_->bindless = &bindless;
    impl_->frames = &frames;
    return true;
}

bool ResourceManager::InitSwapChain(SwapChain& sc, SDL_Window* window) {
    return sc.Init(impl_->params.device, window);
}

Handle<Buffer> ResourceManager::CreateBuffer(const BufferDesc& d) {
    return impl_->res->CreateBuffer(d);
}

Handle<Texture> ResourceManager::CreateTexture(const TextureDesc& d) {
    return impl_->res->CreateTexture(d);
}

Handle<Sampler> ResourceManager::CreateSampler(const SamplerDesc& d) {
    return impl_->res->CreateSampler(d);
}

Handle<BindGroup> ResourceManager::CreateBindGroup(const BindGroupDesc& d) {
    return impl_->res->CreateBindGroup(d);
}

Handle<DynamicBuffers> ResourceManager::CreateDynamicBuffers(
    const DynamicBuffersDesc& d) {
    return impl_->res->CreateDynamicBuffers(d);
}

void ResourceManager::Destroy(Handle<Buffer> h) { impl_->res->Destroy(h); }

void ResourceManager::Destroy(Handle<Texture> h) { impl_->res->Destroy(h); }

void ResourceManager::Destroy(Handle<Sampler> h) {
    Sampler::Hot* hot = impl_->res->samplers.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_sampler) {
        hot->api_sampler->release();
        hot->api_sampler = nullptr;
    }
    impl_->res->samplers.Release(h);
}

void ResourceManager::Destroy(Handle<BindGroup> h) {
    impl_->res->bind_groups.Release(h);
}

void ResourceManager::Destroy(Handle<DynamicBuffers> h) {
    impl_->res->dynamic_buffers.Release(h);
}

void ResourceManager::Destroy(Handle<Shader> h) {
    Shader::Hot* hot = impl_->res->shaders.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_pso) {
        hot->api_pso->release();
        hot->api_pso = nullptr;
    }
    impl_->res->shaders.Release(h);
}

void ResourceManager::Destroy(Handle<Kernel> h) {
    Kernel::Hot* hot = impl_->res->kernels.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_pso) {
        hot->api_pso->release();
        hot->api_pso = nullptr;
    }
    impl_->res->kernels.Release(h);
}

Buffer::Hot* ResourceManager::GetHot(Handle<Buffer> h) {
    return impl_->res->buffers.GetHot(h);
}

Texture::Hot* ResourceManager::GetHot(Handle<Texture> h) {
    return impl_->res->textures.GetHot(h);
}

Sampler::Hot* ResourceManager::GetHot(Handle<Sampler> h) {
    return impl_->res->samplers.GetHot(h);
}

BindGroup::Hot* ResourceManager::GetHot(Handle<BindGroup> h) {
    return impl_->res->bind_groups.GetHot(h);
}

DynamicBuffers::Hot* ResourceManager::GetHot(Handle<DynamicBuffers> h) {
    return impl_->res->dynamic_buffers.GetHot(h);
}

Shader::Hot* ResourceManager::GetHot(Handle<Shader> h) {
    return impl_->res->shaders.GetHot(h);
}

Kernel::Hot* ResourceManager::GetHot(Handle<Kernel> h) {
    return impl_->res->kernels.GetHot(h);
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

    Handle<Shader> h = impl_->res->shaders.Acquire();
    impl_->res->shaders.GetHot(h)->api_pso = pso;
    impl_->res->shaders.GetCold(h)->debug_name = desc.debug_name;
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
    Handle<Kernel> h = impl_->res->kernels.Acquire();
    impl_->res->kernels.GetHot(h)->api_pso = cps;
    impl_->res->kernels.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

uint32_t ResourceManager::GetBufferByteSize(Handle<Buffer> h) const {
    Buffer::Cold* cold = impl_->res->buffers.GetCold(h);
    if (!cold) {
        return 0;
    }
    return cold->size_bytes;
}

uint32_t ResourceManager::BufferBaseOffset(Handle<Buffer> h) {
    return impl_->res->BufferBaseOffset(h);
}

MTL::Buffer* ResourceManager::GetMtlBuffer(Handle<Buffer> h,
                                            uint32_t* out_offset) {
    return impl_->res->GetMtlBuffer(h, out_offset);
}

uint8_t* ResourceManager::MappedPtr(Handle<Buffer> h) {
    return impl_->res->MappedPtr(h);
}

MTL::Buffer* ResourceManager::GetBumpMasterBuffer(Memory mem) const {
    return impl_->res->GetBumpMasterBuffer(mem);
}


}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
