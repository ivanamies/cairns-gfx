// rhi/metal/pipelines.cpp
//
// Metal implementation of cairns::rhi::Pipelines.

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi/pipelines.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <Metal/Metal.hpp>

#include "rhi/resource_manager.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"

namespace cairns::rhi {

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
        case Format::kR32Uint:    return MTL::PixelFormatR32Uint;
        case Format::kD32F:       return MTL::PixelFormatDepth32Float;
        default:                  return MTL::PixelFormatInvalid;
    }
}

}  // namespace

bool Pipelines::Init(Device& device) {
    if (inited_) {
        return true;
    }
    plat.device_ = device.plat.device_;
    inited_ = true;
    return true;
}

void Pipelines::Deinit(Resources&) {
    if (!inited_) {
        return;
    }
    inited_ = false;
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
        case Format::kRgba8Unorm: return MTL::VertexFormatUChar4Normalized;
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
    if (std::strcmp(logical, "unlit") == 0 ||
        std::strcmp(logical, "unlit_offscreen") == 0) {
        return {"unlit.metal", "cube::vertexShader", "cube::fragmentShader", nullptr};
    }
    if (std::strcmp(logical, "imgui") == 0) {
        return {"imgui.metal", "imguicairns::imgui_vertex",
                "imguicairns::imgui_fragment", nullptr};
    }
    if (std::strcmp(logical, "composite_pip") == 0) {
        return {"composite_pip.metal", "composite_pipfx::composite_pip_vertex",
                "composite_pipfx::composite_pip_fragment", nullptr};
    }
    if (std::strcmp(logical, "depthviz") == 0) {
        return {"depthviz.metal", "depthvizfx::depthviz_vertex",
                "depthvizfx::depthviz_fragment", nullptr};
    }
    return {"particle.metal", "particle_vertex", "particle_fragment", "particle_compute"};
}

}  // namespace

Handle<Shader> Pipelines::CreateGraphicsPipeline(
    Resources& resources, Frames&, const GraphicsPipelineDesc& desc) {
    MTL::Device* device = plat.device_;
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

    Handle<Shader> h = resources.shaders.Acquire();
    resources.shaders.GetHot(h)->api_pso = pso;
    resources.shaders.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Handle<Kernel> Pipelines::CreateComputePipeline(
    Resources& resources, Frames&, const ComputePipelineDesc& desc) {
    MTL::Device* device = plat.device_;
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
    Handle<Kernel> h = resources.kernels.Acquire();
    resources.kernels.GetHot(h)->api_pso = cps;
    resources.kernels.GetCold(h)->debug_name = desc.debug_name;
    return h;
}
}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
