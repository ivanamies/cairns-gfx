// rhi/webgpu/pipelines.cpp -- WebGPU backend.
//
// W4: real WGSL render pipelines for the fullscreen passes (red_triangle,
// composite_pip). Each logical_shader maps to assets/<name>.wgsl (vs_main +
// fs_main). The forward/material pipelines (unlit, particle, imgui, ...) are
// still stubbed -- DrawMeshes/DrawPoints/DrawImGui no-op until W5, so their
// null api_pso is never bound. NEVER return Handle::Null here: a null handle
// hangs Engine::GreaterInit.
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/pipelines.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/frames.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <webgpu/webgpu.h>

namespace cairns::rhi {

namespace {

WGPUStringView Sv(const char* s) { return WGPUStringView{s, WGPU_STRLEN}; }

WGPUTextureFormat PipeFormat(Format f) {
    switch (f) {
        case Format::kBgra8Unorm: return WGPUTextureFormat_BGRA8Unorm;
        case Format::kBgra8Srgb: return WGPUTextureFormat_BGRA8UnormSrgb;
        case Format::kRgba8Unorm: return WGPUTextureFormat_RGBA8Unorm;
        case Format::kRgba8Srgb: return WGPUTextureFormat_RGBA8UnormSrgb;
        case Format::kR32Uint: return WGPUTextureFormat_R32Uint;
        case Format::kD32F: return WGPUTextureFormat_Depth32Float;
        default: return WGPUTextureFormat_BGRA8Unorm;
    }
}

WGPUCompareFunction ToWgpuCompare(CompareOp op) {
    switch (op) {
        case CompareOp::kNever: return WGPUCompareFunction_Never;
        case CompareOp::kLess: return WGPUCompareFunction_Less;
        case CompareOp::kEqual: return WGPUCompareFunction_Equal;
        case CompareOp::kLessEqual: return WGPUCompareFunction_LessEqual;
        case CompareOp::kGreater: return WGPUCompareFunction_Greater;
        case CompareOp::kNotEqual: return WGPUCompareFunction_NotEqual;
        case CompareOp::kGreaterEqual: return WGPUCompareFunction_GreaterEqual;
        case CompareOp::kAlways: return WGPUCompareFunction_Always;
        default: return WGPUCompareFunction_Less;
    }
}

WGPUCullMode ToWgpuCull(CullMode c) {
    switch (c) {
        case CullMode::kNone: return WGPUCullMode_None;
        case CullMode::kBack: return WGPUCullMode_Back;
        case CullMode::kFront: return WGPUCullMode_Front;
        default: return WGPUCullMode_None;
    }
}

// Fullscreen WGSL pipelines we know how to build, and their sampled-texture
// count (the bind group is N textures at bindings 0..N-1 + one sampler at N,
// matching CommandRecorder::DrawFullscreen). -1 => not a WGSL fullscreen pass
// (stub it). Forward/material shaders land in W5.
int FullscreenTexCount(const char* logical) {
    if (!logical) { return -1; }
    if (std::strcmp(logical, "red_triangle") == 0) { return 0; }
    if (std::strcmp(logical, "composite_pip") == 0) { return 1; }
    return -1;
}

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return !out.empty();
}

}  // namespace

bool Pipelines::Init(Device& device) { plat.device_ = device.plat.device; inited_ = true; return true; }
void Pipelines::Deinit(Resources& resources) { (void)resources; }

Handle<Shader> Pipelines::CreateGraphicsPipeline(Resources& resources, Frames& frames,
                                                 const GraphicsPipelineDesc& desc) {
    (void)frames;
    Handle<Shader> h = resources.shaders.Acquire();
    Shader::Hot* hot = resources.shaders.GetHot(h);
    if (hot) { hot->api_pso = nullptr; }

    const int tex_count = FullscreenTexCount(desc.logical_shader);
    if (tex_count < 0 || !desc.shader_dir || !hot) {
        return h;  // not yet ported -- real handle, null pso (never drawn)
    }

    std::string wgsl;
    const std::string path =
        std::string(desc.shader_dir) + "/" + desc.logical_shader + ".wgsl";
    if (!ReadFile(path, wgsl)) {
        std::fprintf(stderr, "[webgpu] missing wgsl: %s\n", path.c_str());
        return h;
    }

    WGPUShaderSourceWGSL src = {};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = Sv(wgsl.c_str());
    WGPUShaderModuleDescriptor smd = {};
    smd.nextInChain = &src.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(plat.device_, &smd);
    if (!module) {
        std::fprintf(stderr, "[webgpu] shader module failed: %s\n", path.c_str());
        return h;
    }

    // Bind group 0: tex_count sampled textures (Float/2D) at 0..N-1, one
    // filtering sampler at N. red_triangle (N==0) has no bind group.
    WGPUBindGroupLayout bgl = nullptr;
    if (tex_count > 0) {
        WGPUBindGroupLayoutEntry entries[8] = {};
        for (int i = 0; i < tex_count; ++i) {
            entries[i].binding = static_cast<uint32_t>(i);
            entries[i].visibility = WGPUShaderStage_Fragment;
            entries[i].texture.sampleType = WGPUTextureSampleType_Float;
            entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
            entries[i].texture.multisampled = 0;
        }
        entries[tex_count].binding = static_cast<uint32_t>(tex_count);
        entries[tex_count].visibility = WGPUShaderStage_Fragment;
        entries[tex_count].sampler.type = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor bgld = {};
        bgld.entryCount = static_cast<size_t>(tex_count) + 1;
        bgld.entries = entries;
        bgl = wgpuDeviceCreateBindGroupLayout(plat.device_, &bgld);
    }

    WGPUPipelineLayoutDescriptor pld = {};
    if (bgl) {
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &bgl;
    }
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(plat.device_, &pld);

    const Format color_fmt =
        desc.color_count > 0 ? desc.color_formats[0] : desc.color_format;
    WGPUColorTargetState color = {};
    color.format = PipeFormat(color_fmt);
    color.blend = nullptr;  // opaque; alpha-blending passes (imgui) land in W5
    color.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState frag = {};
    frag.module = module;
    frag.entryPoint = Sv("fs_main");
    frag.targetCount = 1;
    frag.targets = &color;

    WGPUDepthStencilState ds = {};
    const bool has_depth = desc.depth_format != Format::kUndefined;
    if (has_depth) {
        ds.format = PipeFormat(desc.depth_format);
        ds.depthWriteEnabled =
            desc.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        // depth_test off => Always so the rung-1 triangle never self-discards.
        ds.depthCompare = desc.depth_test ? ToWgpuCompare(desc.depth_compare)
                                          : WGPUCompareFunction_Always;
        ds.stencilFront.compare = WGPUCompareFunction_Always;
        ds.stencilBack.compare = WGPUCompareFunction_Always;
    }

    WGPURenderPipelineDescriptor rpd = {};
    rpd.label = Sv(desc.debug_name ? desc.debug_name : desc.logical_shader);
    rpd.layout = pl;
    rpd.vertex.module = module;
    rpd.vertex.entryPoint = Sv("vs_main");
    rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpd.primitive.frontFace = desc.front_face == FrontFace::kClockwise
                                  ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    rpd.primitive.cullMode = ToWgpuCull(desc.cull);
    rpd.depthStencil = has_depth ? &ds : nullptr;
    // Headless graph targets are 1-sample (GraphTextureDesc.samples=1); the
    // MSAA sample_count only applies to the windowed swapchain (W6).
    rpd.multisample.count = desc.swap_chain ? desc.sample_count : 1u;
    rpd.multisample.mask = 0xFFFFFFFFu;
    rpd.fragment = &frag;

    WGPURenderPipeline pso = wgpuDeviceCreateRenderPipeline(plat.device_, &rpd);
    wgpuShaderModuleRelease(module);
    if (!pso) {
        std::fprintf(stderr, "[webgpu] render pipeline failed: %s\n",
                     desc.logical_shader);
        if (pl) { wgpuPipelineLayoutRelease(pl); }
        if (bgl) { wgpuBindGroupLayoutRelease(bgl); }
        return h;
    }
    hot->api_pso = static_cast<void*>(pso);
    hot->plat.layout = pl;
    hot->plat.bind_group_layouts[0] = bgl;  // kept alive for DrawFullscreen
    return h;
}

Handle<Kernel> Pipelines::CreateComputePipeline(Resources& resources, Frames& frames,
                                                const ComputePipelineDesc& desc) {
    (void)frames; (void)desc;
    Handle<Kernel> h = resources.kernels.Acquire();
    if (Kernel::Hot* hot = resources.kernels.GetHot(h)) { hot->api_pso = nullptr; }
    return h;
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
