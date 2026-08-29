// rhi/webgpu/pipelines.cpp -- WebGPU backend. WGSL pipelines resolved by
// Classify; a logical shader without a port gets a REAL handle with a null
// PSO (its consumers no-op) -- NEVER return Handle::Null (hangs GreaterInit).
// Forward pass: vertex streams + 3 bind groups (globals dyn-UBO @0, material
// tex+sampler @1, drawtmp dyn-UBO @2).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/pipelines.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/frames.hpp"
#include "rhi/webgpu/layouts_plat.hpp"

#include <SDL3/SDL.h>

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

WGPUBlendFactor ToWgpuBlend(BlendFactor b) {
    switch (b) {
        case BlendFactor::kZero: return WGPUBlendFactor_Zero;
        case BlendFactor::kOne: return WGPUBlendFactor_One;
        case BlendFactor::kSrcAlpha: return WGPUBlendFactor_SrcAlpha;
        case BlendFactor::kOneMinusSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
        default: return WGPUBlendFactor_One;
    }
}

enum class Kind { kStub, kFullscreen, kUnlit, kImgui };

// Maps a logical_shader to (kind, wgsl file stem, sampled-texture count for the
// fullscreen bind group). The id MRT variants (unlit / unlit_offscreen) need a
// 2-output WGSL; deferred -> stubbed until a pick/outline scenario needs them.
struct ShaderInfo {
    Kind kind = Kind::kStub;
    const char* stem = nullptr;
    int tex_count = 0;
    bool depth_sample = false;  // depthviz samples a Depth32Float target
    bool id_textures = false;   // outline: binding 0 = color (float), 1+ = R32U
};
ShaderInfo Classify(const char* logical) {
    if (!logical) { return {}; }
    if (std::strcmp(logical, "composite_pip") == 0) {
        return {Kind::kFullscreen, "composite_pip", 1};
    }
    if (std::strcmp(logical, "depthviz") == 0) {
        return {Kind::kFullscreen, "depthviz", 1, true};
    }
    if (std::strcmp(logical, "outline") == 0) {
        // 3 textures: color (BGRA float) + id (R32U) + highlights (R32U), and a
        // non-filtering sampler (the R32U pair are read via textureLoad).
        return {Kind::kFullscreen, "outline", 3, false, true};
    }
    if (std::strcmp(logical, "unlit_offscreen") == 0) {
        // id MRT: {BGRA color, R32U id}. 2-output WGSL (unlit_offscreen.wgsl);
        // color_count=2 from the desc drives the 2 fragment targets.
        return {Kind::kUnlit, "unlit_offscreen", 0};
    }
    if (std::strcmp(logical, "unlit_offscreen_noid") == 0) {
        return {Kind::kUnlit, "unlit", 0};
    }
    if (std::strcmp(logical, "lit_offscreen") == 0) {
        return {Kind::kUnlit, "lit_offscreen", 0};
    }
    if (std::strcmp(logical, "lit_offscreen_noid") == 0) {
        return {Kind::kUnlit, "lit_offscreen_noid", 0};
    }
    // Depth-only shadow pipeline: stub until the webgpu builder grows a
    // no-fragment-target path (real handle, null PSO -- consumers no-op, so
    // webgpu renders lit without shadows until then).
    if (std::strcmp(logical, "shadow_depth") == 0) {
        return {Kind::kStub, "depth_only", 0};
    }
    if (std::strcmp(logical, "imgui") == 0) {
        return {Kind::kImgui, "imgui", 0};
    }
    return {};
}

bool ReadFile(const std::string& path, std::string& out) {
    size_t n = 0;
    void* data = SDL_LoadFile(path.c_str(), &n);
    if (!data) { return false; }
    out.assign(static_cast<const char*>(data), n);
    SDL_free(data);
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

    const ShaderInfo info = Classify(desc.logical_shader);
    if (info.kind == Kind::kStub || !desc.shader_dir || !hot) {
        return h;  // not yet ported -- real handle, null pso (never drawn)
    }

    std::string wgsl;
    const std::string path =
        std::string(desc.shader_dir) + "/" + info.stem + ".wgsl";
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

    // --- bind group + pipeline layouts ------------------------------------
    WGPUBindGroupLayout bgl0 = nullptr;  // fullscreen: tex+sampler; unlit: globals
    WGPUBindGroupLayout bgl1 = nullptr;  // unlit: material
    WGPUPipelineLayout pl = nullptr;
    if (info.kind == Kind::kUnlit) {
        bgl0 = webgpu::MakeDynUboLayout(plat.device_);   // group 0 + group 2
        bgl1 = webgpu::MakeMaterialLayout(plat.device_);  // group 1
        WGPUBindGroupLayout bgls[3] = {bgl0, bgl1, bgl0};
        WGPUPipelineLayoutDescriptor pld = {};
        pld.bindGroupLayoutCount = 3;
        pld.bindGroupLayouts = bgls;
        pl = wgpuDeviceCreatePipelineLayout(plat.device_, &pld);
    } else if (info.kind == Kind::kImgui) {
        bgl0 = webgpu::MakeDynUboLayout(plat.device_);    // group 0: pc uniform
        bgl1 = webgpu::MakeMaterialLayout(plat.device_);  // group 1: font+sampler
        WGPUBindGroupLayout bgls[2] = {bgl0, bgl1};
        WGPUPipelineLayoutDescriptor pld = {};
        pld.bindGroupLayoutCount = 2;
        pld.bindGroupLayouts = bgls;
        pl = wgpuDeviceCreatePipelineLayout(plat.device_, &pld);
    } else if (info.tex_count > 0) {
        WGPUBindGroupLayoutEntry entries[8] = {};
        for (int i = 0; i < info.tex_count; ++i) {
            entries[i].binding = static_cast<uint32_t>(i);
            entries[i].visibility = WGPUShaderStage_Fragment;
            WGPUTextureSampleType st = WGPUTextureSampleType_Float;
            if (info.depth_sample) {
                st = WGPUTextureSampleType_Depth;
            } else if (info.id_textures && i > 0) {
                st = WGPUTextureSampleType_Uint;  // outline id + highlights = R32U
            }
            entries[i].texture.sampleType = st;
            entries[i].texture.viewDimension = WGPUTextureViewDimension_2D;
        }
        entries[info.tex_count].binding = static_cast<uint32_t>(info.tex_count);
        entries[info.tex_count].visibility = WGPUShaderStage_Fragment;
        // Depth (depthviz) and R32U-paired (outline) bind groups can only use a
        // NON-filtering sampler -- Dawn rejects the pipeline otherwise (e.g.
        // "TextureSampleType::Depth used with a Filtering sampler"). The bound
        // sampler is Nearest in both cases (depthviz/outline use outline_sampler_).
        entries[info.tex_count].sampler.type =
            (info.depth_sample || info.id_textures)
                ? WGPUSamplerBindingType_NonFiltering
                : WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor bgld = {};
        bgld.entryCount = static_cast<size_t>(info.tex_count) + 1;
        bgld.entries = entries;
        bgl0 = wgpuDeviceCreateBindGroupLayout(plat.device_, &bgld);
        WGPUPipelineLayoutDescriptor pld = {};
        pld.bindGroupLayoutCount = 1;
        pld.bindGroupLayouts = &bgl0;
        pl = wgpuDeviceCreatePipelineLayout(plat.device_, &pld);
    } else {
        WGPUPipelineLayoutDescriptor pld = {};  // no bind groups
        pl = wgpuDeviceCreatePipelineLayout(plat.device_, &pld);
    }

    // --- vertex state (unlit: pos stream @0 + attr stream @1) -------------
    WGPUVertexBufferLayout vbl[8] = {};
    WGPUVertexAttribute vattr[16] = {};
    uint32_t attr_cursor = 0;
    for (size_t b = 0; b < desc.vertex_buffers.size() && b < 8; ++b) {
        const uint32_t start = attr_cursor;
        uint32_t n = 0;
        for (size_t a = 0; a < desc.vertex_attributes.size() && attr_cursor < 16; ++a) {
            if (desc.vertex_attributes[a].buffer_slot != desc.vertex_buffers[b].buffer_slot) {
                continue;
            }
            vattr[attr_cursor].format =
                webgpu::ToWgpuVertexFormat(desc.vertex_attributes[a].format);
            vattr[attr_cursor].offset = desc.vertex_attributes[a].offset;
            vattr[attr_cursor].shaderLocation = desc.vertex_attributes[a].location;
            ++attr_cursor;
            ++n;
        }
        vbl[b].arrayStride = desc.vertex_buffers[b].stride;
        vbl[b].stepMode = WGPUVertexStepMode_Vertex;
        vbl[b].attributeCount = n;
        vbl[b].attributes = &vattr[start];
    }

    // --- color / depth / multisample --------------------------------------
    // MRT: unlit_offscreen writes {BGRA color @0, R32U id @1}; only target 0
    // blends (integer id targets can't). color_count==0 means a single target
    // from the legacy color_format field.
    const uint32_t ncolor = desc.color_count > 0
                                ? static_cast<uint32_t>(desc.color_count)
                                : 1u;
    WGPUBlendState blend = {};
    if (desc.blend.enable) {
        blend.color.operation = WGPUBlendOperation_Add;
        blend.color.srcFactor = ToWgpuBlend(desc.blend.src_color);
        blend.color.dstFactor = ToWgpuBlend(desc.blend.dst_color);
        blend.alpha.operation = WGPUBlendOperation_Add;
        blend.alpha.srcFactor = ToWgpuBlend(desc.blend.src_alpha);
        blend.alpha.dstFactor = ToWgpuBlend(desc.blend.dst_alpha);
    }
    WGPUColorTargetState colors[GraphicsPipelineDesc::kMaxColorFormats] = {};
    for (uint32_t i = 0; i < ncolor &&
                         i < GraphicsPipelineDesc::kMaxColorFormats; ++i) {
        const Format f =
            desc.color_count > 0 ? desc.color_formats[i] : desc.color_format;
        colors[i].format = PipeFormat(f);
        colors[i].writeMask = WGPUColorWriteMask_All;
        if (i == 0 && desc.blend.enable) { colors[i].blend = &blend; }
    }

    WGPUFragmentState frag = {};
    frag.module = module;
    frag.entryPoint = Sv("fs_main");
    frag.targetCount = ncolor;
    frag.targets = colors;

    WGPUDepthStencilState ds = {};
    const bool has_depth = desc.depth_format != Format::kUndefined;
    if (has_depth) {
        ds.format = PipeFormat(desc.depth_format);
        ds.depthWriteEnabled =
            desc.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
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
    if (info.kind == Kind::kUnlit || info.kind == Kind::kImgui) {
        rpd.vertex.bufferCount = desc.vertex_buffers.size();
        rpd.vertex.buffers = vbl;
    }
    rpd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    rpd.primitive.frontFace = desc.front_face == FrontFace::kClockwise
                                  ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    rpd.primitive.cullMode = ToWgpuCull(desc.cull);
    rpd.depthStencil = has_depth ? &ds : nullptr;
    // Headless graph targets are 1-sample (GraphTextureDesc.samples=1); MSAA
    // sample_count only applies to a windowed swapchain pass.
    rpd.multisample.count = desc.swap_chain ? desc.sample_count : 1u;
    rpd.multisample.mask = 0xFFFFFFFFu;
    rpd.fragment = &frag;

    WGPURenderPipeline pso = wgpuDeviceCreateRenderPipeline(plat.device_, &rpd);
    wgpuShaderModuleRelease(module);
    if (!pso) {
        std::fprintf(stderr, "[webgpu] render pipeline failed: %s\n",
                     desc.logical_shader);
        return h;
    }
    hot->api_pso = static_cast<void*>(pso);
    hot->plat.layout = pl;
    hot->plat.bind_group_layouts[0] = bgl0;
    hot->plat.bind_group_layouts[1] = bgl1;
    return h;
}

Handle<Kernel> Pipelines::CreateComputePipeline(Resources& resources, Frames& frames,
                                                const ComputePipelineDesc& desc) {
    (void)frames;
    Handle<Kernel> h = resources.kernels.Acquire();
    Kernel::Hot* hot = resources.kernels.GetHot(h);
    if (hot) { hot->api_pso = nullptr; }
    if (!hot || !desc.shader_dir || !desc.logical_shader) { return h; }

    std::string wgsl;
    const std::string path =
        std::string(desc.shader_dir) + "/" + desc.logical_shader + ".wgsl";
    if (!ReadFile(path, wgsl)) {
        std::fprintf(stderr, "[webgpu] missing compute wgsl: %s\n", path.c_str());
        return h;
    }
    WGPUShaderSourceWGSL src = {};
    src.chain.sType = WGPUSType_ShaderSourceWGSL;
    src.code = Sv(wgsl.c_str());
    WGPUShaderModuleDescriptor smd = {};
    smd.nextInChain = &src.chain;
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(plat.device_, &smd);
    if (!module) {
        std::fprintf(stderr, "[webgpu] compute module failed: %s\n", path.c_str());
        return h;
    }

    // Pipeline layout = the dyn-set's prebuilt group-0 layout (CreateDynamicBuffers
    // built it from the same bindings, so they are group-equivalent). Skin is the
    // exception: vk's two sets fold into one webgpu set (7 bindings, see
    // skin.wgsl / MakeSkinSetLayout), so it gets a dedicated layout kept on the
    // kernel for the recorder's per-batch bind groups.
    WGPUBindGroupLayout bgl = nullptr;
    if (desc.layout == ComputePipelineLayout::kSkin) {
        bgl = webgpu::MakeSkinSetLayout(plat.device_);
        hot->plat.set0_bgl = bgl;  // recorder builds per-batch bind groups from it
    } else if (desc.layout == ComputePipelineLayout::kAnimEval) {
        bgl = webgpu::MakeAnimEvalSetLayout(plat.device_);
    } else if (!desc.dyn_set_0.IsNull()) {  // kParticle: dyn-set's prebuilt layout
        if (DynamicBuffers::Hot* dh =
                resources.dynamic_buffers.GetHot(desc.dyn_set_0)) {
            bgl = dh->plat.layout;
        }
    }
    if (!bgl) {
        std::fprintf(stderr, "[webgpu] compute %s: no dyn_set_0 layout\n",
                     desc.logical_shader);
        wgpuShaderModuleRelease(module);
        return h;
    }
    WGPUPipelineLayoutDescriptor pld = {};
    pld.bindGroupLayoutCount = 1;
    pld.bindGroupLayouts = &bgl;
    WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(plat.device_, &pld);

    WGPUComputePipelineDescriptor cpd = {};
    cpd.label = Sv(desc.debug_name ? desc.debug_name : desc.logical_shader);
    cpd.layout = pl;
    cpd.compute.module = module;
    cpd.compute.entryPoint = Sv("cs_main");
    WGPUComputePipeline cps = wgpuDeviceCreateComputePipeline(plat.device_, &cpd);
    wgpuShaderModuleRelease(module);
    if (!cps) {
        std::fprintf(stderr, "[webgpu] compute pipeline failed: %s\n",
                     desc.logical_shader);
        return h;
    }
    hot->api_pso = static_cast<void*>(cps);
    hot->plat.layout = pl;
    return h;
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
