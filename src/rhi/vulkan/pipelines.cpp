// rhi/vulkan/pipelines.cpp
//
// Vulkan implementation of cairns::rhi::Pipelines.

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include "rhi/pipelines.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "rhi/resource_manager.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/frames.hpp"
#include "rhi/swap_chain.hpp"
#include "util/log.hpp"

namespace cairns::rhi {

namespace {

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

}  // namespace

bool Pipelines::Init(Device& device) {
    if (inited_) {
        return true;
    }
    plat.device_ = device.plat.device_;
    VkDevice dev = plat.device_;
    // #222 Phase F.4: descriptor set layouts moved from Frames to here
    // (Pipelines is the canonical consumer for VkPipelineLayout creation).

    // #222 Phase E.2: point_layout_ retired -- particle render PSO has
    // zero descriptor sets in its pipeline layout (vert reads only
    // vertex-input attributes); the recorder skips the empty-set bind.
    {  // skin Group B (frame-global skin kernel set 0): dynUBO Params
       // @0, dynSSBO palettes @1, dynSSBO InstanceMeta @2, SSBO output
       // pool whole @3.
        VkDescriptorSetLayoutBinding b[4]{};
        b[0].binding = 0;
        b[0].descriptorCount = 1;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[1].binding = 1;
        b[1].descriptorCount = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[2].binding = 2;
        b[2].descriptorCount = 1;
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[3].binding = 3;
        b[3].descriptorCount = 1;
        b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr,
                                        &plat.skin_group_b_layout_) !=
            VK_SUCCESS) {
            return false;
        }
    }
    {  // #231 anim_eval set layout (7 bindings; DYNAMIC_UBO records @0,
       // 6 packed SSBO i32/vec4/word16 + headers + scratch + palette @1..6).
        VkDescriptorSetLayoutBinding b[7]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        for (uint32_t i = 1; i < 7; ++i) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        for (uint32_t i = 0; i < 7; ++i) {
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 7;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr,
                                        &plat.anim_eval_layout_) !=
            VK_SUCCESS) {
            return false;
        }
    }
    {  // #221 Skinning Phase 4 -- Group A (per-mesh). Set 1 of the
       // skin kernel: SSBO positions slice @ 0, SSBO skin-attrs slice
       // @ 1. Built at load via Resources::CreateBindGroup; one set
       // per skinned mesh.
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0;
        b[0].descriptorCount = 1;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[1].binding = 1;
        b[1].descriptorCount = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr,
                                        &plat.skin_group_a_layout_) !=
            VK_SUCCESS) {
            return false;
        }
    }
    // Per-frequency single dynamic-UBO layouts (Aaltonen split): set 0 globals
    // (once/frame), set 2 drawtmp (per draw). Structurally identical but kept as
    // distinct named layouts.
    auto make_dyn_ubo_layout = [&](VkDescriptorSetLayout* out) -> bool {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings = &b;
        return vkCreateDescriptorSetLayout(dev, &li, nullptr, out) == VK_SUCCESS;
    };
    if (!make_dyn_ubo_layout(&plat.globals_set_layout_) ||
        !make_dyn_ubo_layout(&plat.drawtmp_set_layout_)) {
        return false;
    }
    // Composite descriptor set layout: 3 COMBINED_IMAGE_SAMPLER frag.
    {
        VkDescriptorSetLayoutBinding b[3]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1].binding = 1;
        b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[1].descriptorCount = 1;
        b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[2].binding = 2;
        b[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[2].descriptorCount = 1;
        b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 3;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, nullptr,
                                        &plat.composite_set_layout_) !=
            VK_SUCCESS) {
            return false;
        }
    }
    inited_ = true;
    return true;
}

void Pipelines::Deinit(Resources& resources) {
    if (!inited_) {
        return;
    }
    VkDevice dev = plat.device_;
    resources.shaders.ForEachLive([dev](Shader::Hot& hot, Shader::Cold&) {
        if (hot.plat.vk_pipeline) {
            vkDestroyPipeline(dev, hot.plat.vk_pipeline, nullptr);
            hot.plat.vk_pipeline = VK_NULL_HANDLE;
        }
        if (hot.plat.vk_layout) {
            vkDestroyPipelineLayout(dev, hot.plat.vk_layout, nullptr);
            hot.plat.vk_layout = VK_NULL_HANDLE;
        }
        if (hot.plat.vk_imgui_pool) {
            vkDestroyDescriptorPool(dev, hot.plat.vk_imgui_pool, nullptr);
            hot.plat.vk_imgui_pool = VK_NULL_HANDLE;
            hot.plat.vk_imgui_set = VK_NULL_HANDLE;
        }
        if (hot.plat.vk_imgui_set_layout) {
            vkDestroyDescriptorSetLayout(dev, hot.plat.vk_imgui_set_layout, nullptr);
            hot.plat.vk_imgui_set_layout = VK_NULL_HANDLE;
        }
    });
    resources.kernels.ForEachLive([dev](Kernel::Hot& hot, Kernel::Cold&) {
        if (hot.plat.vk_pipeline) {
            vkDestroyPipeline(dev, hot.plat.vk_pipeline, nullptr);
            hot.plat.vk_pipeline = VK_NULL_HANDLE;
        }
        if (hot.plat.vk_layout) {
            vkDestroyPipelineLayout(dev, hot.plat.vk_layout, nullptr);
            hot.plat.vk_layout = VK_NULL_HANDLE;
        }
    });
    // #222 Phase F.4: descriptor set layouts owned here -- teardown
    // after the live pipelines so consumers are gone first.
    auto destroy_layout = [dev](VkDescriptorSetLayout& l) {
        if (l) {
            vkDestroyDescriptorSetLayout(dev, l, nullptr);
            l = VK_NULL_HANDLE;
        }
    };
    destroy_layout(plat.globals_set_layout_);
    destroy_layout(plat.drawtmp_set_layout_);
    destroy_layout(plat.composite_set_layout_);
    destroy_layout(plat.skin_group_a_layout_);
    destroy_layout(plat.skin_group_b_layout_);
    destroy_layout(plat.anim_eval_layout_);
    inited_ = false;
}

namespace {

bool read_spv_file(const std::string& path, std::vector<char>* out) {
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) {
        CAIRNS_PRINT("rhi/vk: failed to open shader: %s (%s)\n", path.c_str(),
                     SDL_GetError());
        return false;
    }
    out->resize(size);
    std::memcpy(out->data(), data, size);
    SDL_free(data);
    return true;
}

VkShaderModule make_shader_module(VkDevice device, const std::vector<char>& code) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return m;
}

VkPrimitiveTopology to_vk_topology(PrimitiveTopology t) {
    return t == PrimitiveTopology::kPointList ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                                              : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkCullModeFlags to_vk_cull(CullMode c) {
    switch (c) {
        case CullMode::kBack:  return VK_CULL_MODE_BACK_BIT;
        case CullMode::kFront: return VK_CULL_MODE_FRONT_BIT;
        default:               return VK_CULL_MODE_NONE;
    }
}

VkFrontFace to_vk_front_face(FrontFace f) {
    return f == FrontFace::kClockwise ? VK_FRONT_FACE_CLOCKWISE
                                      : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkCompareOp to_vk_compare_op(CompareOp c) {
    switch (c) {
        case CompareOp::kNever:        return VK_COMPARE_OP_NEVER;
        case CompareOp::kLess:         return VK_COMPARE_OP_LESS;
        case CompareOp::kEqual:        return VK_COMPARE_OP_EQUAL;
        case CompareOp::kLessEqual:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::kGreater:      return VK_COMPARE_OP_GREATER;
        case CompareOp::kNotEqual:     return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::kGreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::kAlways:       return VK_COMPARE_OP_ALWAYS;
        default:                       return VK_COMPARE_OP_LESS;
    }
}

VkBlendFactor to_vk_blend_factor(BlendFactor b) {
    switch (b) {
        case BlendFactor::kZero:             return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::kOne:              return VK_BLEND_FACTOR_ONE;
        case BlendFactor::kSrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::kOneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        default:                             return VK_BLEND_FACTOR_ONE;
    }
}

VkSampleCountFlagBits to_vk_samples(uint32_t n) {
    switch (n) {
        case 1:  return VK_SAMPLE_COUNT_1_BIT;
        case 2:  return VK_SAMPLE_COUNT_2_BIT;
        case 4:  return VK_SAMPLE_COUNT_4_BIT;
        case 8:  return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        case 32: return VK_SAMPLE_COUNT_32_BIT;
        case 64: return VK_SAMPLE_COUNT_64_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
    }
}

struct VkShaderFiles {
    const char* vert;
    const char* frag;
    const char* comp;
};
VkShaderFiles resolve_vk_shader(const char* logical) {
    if (std::strcmp(logical, "unlit") == 0 ||
        std::strcmp(logical, "unlit_offscreen") == 0) {
        return {"unlit.vert.spv", "unlit.frag.spv", nullptr};
    }
    // #222 Phase A.1: id-less offscreen variant (no R32U MRT output).
    if (std::strcmp(logical, "unlit_offscreen_noid") == 0) {
        return {"unlit.vert.spv", "unlit_noid.frag.spv", nullptr};
    }
    if (std::strcmp(logical, "imgui") == 0) {
        return {"imgui.vert.spv", "imgui.frag.spv", nullptr};
    }
    if (std::strcmp(logical, "composite_pip") == 0) {
        return {"composite_pip.vert.spv", "composite_pip.frag.spv", nullptr};
    }
    if (std::strcmp(logical, "depthviz") == 0) {
        // depthviz reuses the composite full-screen tri vert.
        return {"composite_pip.vert.spv", "depthviz.frag.spv", nullptr};
    }
    if (std::strcmp(logical, "outline") == 0) {
        // #207 outline post-process fullscreen tri.
        return {"outline.vert.spv", "outline.frag.spv", nullptr};
    }
    if (std::strcmp(logical, "skin") == 0) {
        // #221 Phase 4: skin compute kernel (no vert/frag).
        return {nullptr, nullptr, "skin.comp.spv"};
    }
    if (std::strcmp(logical, "anim_eval") == 0) {
        return {nullptr, nullptr, "anim_eval.comp.spv"};
    }
    return {"particle.vert.spv", "particle.frag.spv", "particle.comp.spv"};
}

// Render-pass compatibility object built once per pipeline create, then
// destroyed. Used when desc.swap_chain is null (offscreen target compat).
// #206 multi-color: color_count attachments at indices [0..color_count),
// depth (if has_depth) follows at color_count. kMaxColorFormats hard cap.
VkRenderPass build_offscreen_compat_rp(VkDevice dev,
                                       const VkFormat* colors, uint32_t color_count,
                                       VkFormat depth, bool has_depth,
                                       VkSampleCountFlagBits samples) {
    constexpr uint32_t kMaxAtts = GraphicsPipelineDesc::kMaxColorFormats + 1;
    VkAttachmentDescription atts[kMaxAtts]{};
    VkAttachmentReference color_refs[GraphicsPipelineDesc::kMaxColorFormats]{};
    VkAttachmentReference depth_ref{};
    uint32_t n = 0;
    for (uint32_t i = 0; i < color_count; ++i) {
        atts[n].format = colors[i];
        atts[n].samples = samples;
        atts[n].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        atts[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_refs[i].attachment = n;
        color_refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ++n;
    }
    int depth_idx = -1;
    if (has_depth) {
        depth_idx = static_cast<int>(n);
        atts[n].format = depth;
        atts[n].samples = samples;
        atts[n].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        atts[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        atts[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        atts[n].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        atts[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ++n;
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = color_count;
    sub.pColorAttachments = color_count > 0 ? color_refs : nullptr;
    if (depth_idx >= 0) {
        depth_ref.attachment = static_cast<uint32_t>(depth_idx);
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        sub.pDepthStencilAttachment = &depth_ref;
    }
    VkRenderPassCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ci.attachmentCount = n;
    ci.pAttachments = atts;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    VkRenderPass rp = VK_NULL_HANDLE;
    vkCreateRenderPass(dev, &ci, nullptr, &rp);
    return rp;
}

}  // namespace

Handle<Shader> Pipelines::CreateGraphicsPipeline(
    Resources& resources, Frames& /*frames*/,
    const GraphicsPipelineDesc& desc) {
    VkDevice device = plat.device_;
    const VkShaderFiles files = resolve_vk_shader(desc.logical_shader);
    const std::filesystem::path dir = desc.shader_dir ? desc.shader_dir : "";

    std::vector<char> vert_code;
    std::vector<char> frag_code;
    if (!read_spv_file((dir / files.vert).string(), &vert_code) ||
        !read_spv_file((dir / files.frag).string(), &frag_code)) {
        return Handle<Shader>::Null;
    }
    VkShaderModule vert_mod = make_shader_module(device, vert_code);
    VkShaderModule frag_mod = make_shader_module(device, frag_code);
    if (!vert_mod || !frag_mod) {
        return Handle<Shader>::Null;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_mod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_mod;
    stages[1].pName = "main";

    std::vector<VkVertexInputBindingDescription> bindings;
    for (size_t i = 0; i < desc.vertex_buffers.size(); ++i) {
        VkVertexInputBindingDescription b{};
        b.binding = desc.vertex_buffers[i].buffer_slot;
        b.stride = desc.vertex_buffers[i].stride;
        b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(b);
    }
    std::vector<VkVertexInputAttributeDescription> attrs;
    for (size_t i = 0; i < desc.vertex_attributes.size(); ++i) {
        const VertexInputAttribute& a = desc.vertex_attributes[i];
        VkVertexInputAttributeDescription va{};
        va.location = a.location;
        va.binding = a.buffer_slot;
        va.format = to_vk_format(a.format);
        va.offset = a.offset;
        attrs.push_back(va);
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertex_input.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = to_vk_topology(desc.topology);
    input_assembly.primitiveRestartEnable = VK_FALSE;

    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dyn_states;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = to_vk_cull(desc.cull);
    rasterizer.frontFace = to_vk_front_face(desc.front_face);
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = to_vk_samples(desc.sample_count);
    multisampling.minSampleShading = 1.0f;

    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_attachment.blendEnable = desc.blend.enable ? VK_TRUE : VK_FALSE;
    blend_attachment.srcColorBlendFactor = to_vk_blend_factor(desc.blend.src_color);
    blend_attachment.dstColorBlendFactor = to_vk_blend_factor(desc.blend.dst_color);
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = to_vk_blend_factor(desc.blend.src_alpha);
    blend_attachment.dstAlphaBlendFactor = to_vk_blend_factor(desc.blend.dst_alpha);
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;

    // #206 per-attachment blend state. The shared BlendState applies to
    // attachment 0 (color); secondary attachments (e.g. R32U ID buffer)
    // get a blend-disabled state with the same color write mask.
    const uint32_t n_color_atts =
        desc.color_count > 0
            ? static_cast<uint32_t>(desc.color_count)
            : (desc.color_format != Format::kUndefined ? 1u : 0u);
    VkPipelineColorBlendAttachmentState blend_atts[GraphicsPipelineDesc::kMaxColorFormats]{};
    // #242: shader may write fewer outputs than n_color_atts. Attachments
    // [frag_outs, n_color_atts) get colorWriteMask=0 so validation knows
    // we're intentionally not writing them (avoids
    // Undefined-Value-ShaderInputNotProduced).
    const uint32_t frag_outs = desc.frag_color_output_count > 0
        ? static_cast<uint32_t>(desc.frag_color_output_count)
        : n_color_atts;
    for (uint32_t i = 0; i < n_color_atts; ++i) {
        blend_atts[i] = blend_attachment;
        if (i > 0) {
            // Secondary attachments: blend off (UINT formats can't blend).
            blend_atts[i].blendEnable = VK_FALSE;
        }
        if (i >= frag_outs) {
            blend_atts[i].colorWriteMask = 0;
        }
    }
    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.logicOp = VK_LOGIC_OP_COPY;
    color_blending.attachmentCount = n_color_atts;
    color_blending.pAttachments = n_color_atts ? blend_atts : nullptr;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = desc.depth_write ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = to_vk_compare_op(desc.depth_compare);
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.minDepthBounds = 0.0f;
    depth_stencil.maxDepthBounds = 1.0f;
    depth_stencil.stencilTestEnable = VK_FALSE;

    VkPushConstantRange pc_range{};
    pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc_range.offset = 0;
    pc_range.size = desc.push_constant_bytes;
    const std::string ls = desc.logical_shader ? desc.logical_shader : "";
    std::vector<VkDescriptorSetLayout> set_layouts;
    VkDescriptorSetLayout imgui_set_layout = VK_NULL_HANDLE;
    if (ls == "unlit" || ls == "unlit_offscreen" ||
        ls == "unlit_offscreen_noid") {
        set_layouts = {plat.globals_set_layout_,      // set 0: globals (once/frame)
                       resources.plat.MaterialSetLayout(),   // set 1: per-material
                       plat.drawtmp_set_layout_};     // set 2: drawtmp (per draw)
    } else if (ls == "composite_pip" || ls == "depthviz" || ls == "outline") {
        // 2 COMBINED_IMAGE_SAMPLER frag (binding 0 = primary color, binding
        // 1 = id; only outline statically accesses binding 1). Composite and
        // outline share this layout.
        set_layouts = {plat.composite_set_layout_};
    } else if (ls == "imgui") {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dl{};
        dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dl.bindingCount = 1;
        dl.pBindings = &b;
        vkCreateDescriptorSetLayout(device, &dl, nullptr, &imgui_set_layout);
        set_layouts = {imgui_set_layout};
    } else {
        // #222 Phase E.2: particle vert shader reads no descriptors --
        // pipeline layout has zero sets, recorder skips bind.
    }
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    layout_info.pSetLayouts = set_layouts.data();
    layout_info.pushConstantRangeCount = desc.push_constant_bytes ? 1 : 0;
    layout_info.pPushConstantRanges = desc.push_constant_bytes ? &pc_range : nullptr;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, vert_mod, nullptr);
        vkDestroyShaderModule(device, frag_mod, nullptr);
        return Handle<Shader>::Null;
    }

    VkGraphicsPipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vertex_input;
    pi.pInputAssemblyState = &input_assembly;
    pi.pViewportState = &viewport_state;
    pi.pRasterizationState = &rasterizer;
    pi.pMultisampleState = &multisampling;
    pi.pColorBlendState = &color_blending;
    pi.pDynamicState = &dynamic_state;
    pi.pDepthStencilState = &depth_stencil;
    pi.layout = layout;
    // Swapchain pipelines bind the swapchain's (MSAA) render pass; offscreen
    // pipelines build a single-sample compat render pass, destroyed below.
    VkRenderPass compat_rp = VK_NULL_HANDLE;
    if (desc.swap_chain && desc.swap_chain->plat.renderPass != VK_NULL_HANDLE) {
        pi.renderPass = desc.swap_chain->plat.renderPass;
    } else {
        // #206 resolve color formats: prefer color_formats[] when
        // color_count > 0; else fall back to single color_format.
        VkFormat color_fmts[GraphicsPipelineDesc::kMaxColorFormats]{};
        uint32_t cc = 0;
        if (desc.color_count > 0) {
            for (uint8_t i = 0; i < desc.color_count; ++i) {
                color_fmts[i] = to_vk_format(desc.color_formats[i]);
            }
            cc = desc.color_count;
        } else if (desc.color_format != Format::kUndefined) {
            color_fmts[0] = to_vk_format(desc.color_format);
            cc = 1;
        }
        const bool has_depth = desc.depth_format != Format::kUndefined;
        compat_rp = build_offscreen_compat_rp(
            device, color_fmts, cc,
            to_vk_format(desc.depth_format), has_depth,
            to_vk_samples(desc.sample_count));
        pi.renderPass = compat_rp;
    }
    pi.subpass = 0;
    pi.basePipelineHandle = VK_NULL_HANDLE;
    pi.basePipelineIndex = -1;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
    vkDestroyShaderModule(device, vert_mod, nullptr);
    vkDestroyShaderModule(device, frag_mod, nullptr);
    if (compat_rp) {
        vkDestroyRenderPass(device, compat_rp, nullptr);
    }
    if (res != VK_SUCCESS) {
        vkDestroyPipelineLayout(device, layout, nullptr);
        return Handle<Shader>::Null;
    }

    Handle<Shader> h = resources.shaders.Acquire();
    Shader::Hot* hot = resources.shaders.GetHot(h);
    hot->plat.vk_pipeline = pipeline;
    hot->plat.vk_layout = layout;
    if (imgui_set_layout) {
        hot->plat.vk_imgui_set_layout = imgui_set_layout;
        VkDescriptorPoolSize ps{};
        ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ps.descriptorCount = 1;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &ps;
        vkCreateDescriptorPool(device, &pci, nullptr, &hot->plat.vk_imgui_pool);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = hot->plat.vk_imgui_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &imgui_set_layout;
        vkAllocateDescriptorSets(device, &ai, &hot->plat.vk_imgui_set);
    }
    resources.shaders.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Handle<Kernel> Pipelines::CreateComputePipeline(
    Resources& resources, Frames& /*frames*/, const ComputePipelineDesc& desc) {
    VkDevice device = plat.device_;
    const VkShaderFiles files = resolve_vk_shader(desc.logical_shader);
    const std::filesystem::path dir = desc.shader_dir ? desc.shader_dir : "";

    std::vector<char> comp_code;
    if (!read_spv_file((dir / files.comp).string(), &comp_code)) {
        return Handle<Kernel>::Null;
    }
    VkShaderModule comp_mod = make_shader_module(device, comp_code);
    if (!comp_mod) {
        return Handle<Kernel>::Null;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = comp_mod;
    stage.pName = "main";

    // #221 Phase 4: layout selection via ComputePipelineDesc::layout. Skin
    // uses two sets (Group B frame-global + Group A per-mesh); particle
    // stays single-set. The skin path's Group A set is per-mesh and lives
    // in the bind-group pool, not in Frames.
    VkDescriptorSetLayout compute_layouts[2];
    uint32_t set_count = 1;
    // #222 Phase D.3/D.4 cleanup:
    // - kParticle: layout comes from dyn_set_0 (parity DynamicBuffers
    //   built before the kernel by initParticles).
    // - kSkin / kAnimEval: layout comes from frames.plat (DynamicBuffers
    //   for these kernels are built post-uploadAnimTablesGpu, AFTER the
    //   kernel. Sets are layout-compatible).
    VkDescriptorSetLayout dyn0 = VK_NULL_HANDLE;
    if (!desc.dyn_set_0.IsNull()) {
        DynamicBuffers::Hot* dh = resources.dynamic_buffers.GetHot(desc.dyn_set_0);
        if (dh) {
            dyn0 = dh->plat.vk_layout;
        }
    }
    if (desc.layout == ComputePipelineLayout::kSkin) {
        compute_layouts[0] = plat.skin_group_b_layout_;
        compute_layouts[1] = plat.skin_group_a_layout_;
        set_count = 2;
    } else if (desc.layout == ComputePipelineLayout::kAnimEval) {
        compute_layouts[0] = plat.anim_eval_layout_;
    } else {
        compute_layouts[0] = dyn0;
    }
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = set_count;
    layout_info.pSetLayouts = compute_layouts;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &layout) != VK_SUCCESS) {
        vkDestroyShaderModule(device, comp_mod, nullptr);
        return Handle<Kernel>::Null;
    }

    VkComputePipelineCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage = stage;
    pi.layout = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
    vkDestroyShaderModule(device, comp_mod, nullptr);
    if (res != VK_SUCCESS) {
        vkDestroyPipelineLayout(device, layout, nullptr);
        return Handle<Kernel>::Null;
    }

    Handle<Kernel> h = resources.kernels.Acquire();
    Kernel::Hot* hot = resources.kernels.GetHot(h);
    hot->plat.vk_pipeline = pipeline;
    hot->plat.vk_layout = layout;
    resources.kernels.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
