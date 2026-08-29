// rhi/vulkan/resource_manager.cpp
//
// Vulkan implementation of cairns::rhi::ResourceManager.

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include "rhi/resource_manager.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan_beta.h>
#include <stb_image_write.h>

#include "rhi/vulkan/memory_allocator.hpp"
#include "rhi/command_recorder.hpp"
#include "rhi/vulkan/command_recorder_impl.hpp"
#include "rhi/device.hpp"
#include "rhi/vulkan/internal/device_impl.hpp"
#include "rhi/allocator.hpp"
#include "rhi/vulkan/internal/allocator_impl.hpp"
#include "rhi/resources.hpp"
#include "rhi/bindless.hpp"
#include "rhi/vulkan/internal/bindless_impl.hpp"
#include "rhi/frames.hpp"
#include "rhi/vulkan/internal/frames_impl.hpp"
#include "rhi/swap_chain.hpp"
#include "util/render_pass_globals.hpp"
#include "util/material_gpu.hpp"

namespace cairns::rhi {

struct ResourceManager::Impl {
    BackendInitParams params;
    Allocator* alloc = nullptr;  // borrowed; owns the MemoryAllocator + aligns

    // Device handles mirrored from Device; used by InitSwapChain + pipelines.
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaa_samples = VK_SAMPLE_COUNT_1_BIT;

    Resources* res = nullptr;       // borrowed; owns the 7 pools + frame counter
    Bindless* bindless = nullptr;   // borrowed; CreateGraphicsPipeline reads its layout
    Frames* frames = nullptr;       // borrowed; pipeline reads its set layouts
};

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
        case Format::kD32F: return VK_FORMAT_D32_SFLOAT;
        case Format::kD24S8: return VK_FORMAT_D24_UNORM_S8_UINT;
        default: return VK_FORMAT_R8G8B8A8_UNORM;
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
    VkDevice dev = impl_->params.device;
    impl_->res->textures.ForEachLive([dev](Texture::Hot& hot, Texture::Cold& cold) {
        if (hot.api_view) {
            vkDestroyImageView(dev, static_cast<VkImageView>(hot.api_view), nullptr);
            hot.api_view = nullptr;
        }
        if (cold.api_image) {
            vkDestroyImage(dev, static_cast<VkImage>(cold.api_image), nullptr);
            cold.api_image = nullptr;
        }
    });
    impl_->res->samplers.ForEachLive([dev](Sampler::Hot& hot, Sampler::Cold&) {
        if (hot.api_sampler) {
            vkDestroySampler(dev, static_cast<VkSampler>(hot.api_sampler), nullptr);
            hot.api_sampler = nullptr;
        }
    });
    impl_->res->shaders.ForEachLive([dev](Shader::Hot& hot, Shader::Cold&) {
        if (hot.vk_pipeline) {
            vkDestroyPipeline(dev, hot.vk_pipeline, nullptr);
            hot.vk_pipeline = VK_NULL_HANDLE;
        }
        if (hot.vk_layout) {
            vkDestroyPipelineLayout(dev, hot.vk_layout, nullptr);
            hot.vk_layout = VK_NULL_HANDLE;
        }
    });
    impl_->res->kernels.ForEachLive([dev](Kernel::Hot& hot, Kernel::Cold&) {
        if (hot.vk_pipeline) {
            vkDestroyPipeline(dev, hot.vk_pipeline, nullptr);
            hot.vk_pipeline = VK_NULL_HANDLE;
        }
        if (hot.vk_layout) {
            vkDestroyPipelineLayout(dev, hot.vk_layout, nullptr);
            hot.vk_layout = VK_NULL_HANDLE;
        }
    });
    // memory allocator dtor frees device memory; the device/instance/surface/
    // command-pool teardown is owned by Device::Deinit, which the engine calls
    // AFTER this (so heaps free against a live device).
    delete impl_;
    impl_ = nullptr;
}

bool ResourceManager::InitDevice(Device& dev, Allocator& alloc, Resources& res,
                                 Bindless& bindless, Frames& frames) {
    impl_ = new Impl();
    impl_->res = &res;            // borrowed; owns the 7 pools + frame counter
    impl_->bindless = &bindless;  // borrowed; CreateGraphicsPipeline reads its layout
    impl_->frames = &frames;      // borrowed; pipeline reads its set layouts

    // Mirror the subset of device handles still used here (InitSwapChain +
    // pipeline creation). Device owns creation + teardown; RM only borrows.
    impl_->surface = dev.impl_->surface;
    impl_->params.physical = dev.impl_->physical;
    impl_->params.device = dev.impl_->device;
    impl_->graphics_queue = dev.impl_->graphics_queue;
    impl_->params.command_pool = dev.impl_->command_pool;
    impl_->params.queue = dev.impl_->graphics_queue;
    impl_->params.queue_family_index = dev.impl_->queue_family_index;
    impl_->msaa_samples = dev.impl_->msaa_samples;
    impl_->alloc = &alloc;  // Allocator owns the MemoryAllocator + aligns (Init'd already)
    return true;
}


bool ResourceManager::InitSwapChain(SwapChain& sc, SDL_Window* window) {
    return sc.Init(impl_->params.device, impl_->params.physical, impl_->surface,
                   window, impl_->params.command_pool, impl_->graphics_queue,
                   impl_->msaa_samples, true);
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

void ResourceManager::Destroy(Handle<Shader> h) {
    impl_->res->shaders.Release(h);
}

Shader::Hot* ResourceManager::GetHot(Handle<Shader> h) {
    return impl_->res->shaders.GetHot(h);
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
        vkDestroySampler(impl_->params.device,
                         static_cast<VkSampler>(hot->api_sampler), nullptr);
    }
    impl_->res->samplers.Release(h);
}

void ResourceManager::Destroy(Handle<BindGroup> h) {
    impl_->res->bind_groups.Release(h);
}

void ResourceManager::Destroy(Handle<DynamicBuffers> h) {
    impl_->res->dynamic_buffers.Release(h);
}

void ResourceManager::Destroy(Handle<Kernel> h) {
    impl_->res->kernels.Release(h);
}

namespace {

bool read_spv_file(const std::string& path, std::vector<char>* out) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "rhi/vk: failed to open shader: " << path << std::endl;
        return false;
    }
    const size_t size = static_cast<size_t>(file.tellg());
    out->resize(size);
    file.seekg(0);
    file.read(out->data(), size);
    file.close();
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
    if (std::strcmp(logical, "unlit") == 0) {
        return {"unlit.vert.spv", "unlit.frag.spv", nullptr};
    }
    // "particle"
    return {"particle.vert.spv", "particle.frag.spv", "particle.comp.spv"};
}

}  // namespace

Handle<Shader> ResourceManager::CreateGraphicsPipeline(
    const GraphicsPipelineDesc& desc) {
    VkDevice device = impl_->params.device;
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

    VkPipelineColorBlendStateCreateInfo color_blending{};
    color_blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blending.logicOpEnable = VK_FALSE;
    color_blending.logicOp = VK_LOGIC_OP_COPY;
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &blend_attachment;

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
    std::vector<VkDescriptorSetLayout> set_layouts;
    if (desc.logical_shader && std::string(desc.logical_shader) == "unlit") {
        set_layouts = {impl_->bindless->impl_->bindless_layout,
                       impl_->frames->impl_->dyn_ubo_layout};
    } else {
        set_layouts = {impl_->frames->impl_->point_layout};
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
    pi.renderPass = desc.swap_chain->renderPass;
    pi.subpass = 0;
    pi.basePipelineHandle = VK_NULL_HANDLE;
    pi.basePipelineIndex = -1;

    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
    vkDestroyShaderModule(device, vert_mod, nullptr);
    vkDestroyShaderModule(device, frag_mod, nullptr);
    if (res != VK_SUCCESS) {
        vkDestroyPipelineLayout(device, layout, nullptr);
        return Handle<Shader>::Null;
    }

    Handle<Shader> h = impl_->res->shaders.Acquire();
    Shader::Hot* hot = impl_->res->shaders.GetHot(h);
    hot->vk_pipeline = pipeline;
    hot->vk_layout = layout;
    impl_->res->shaders.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Handle<Kernel> ResourceManager::CreateComputePipeline(
    const ComputePipelineDesc& desc) {
    VkDevice device = impl_->params.device;
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

    const VkDescriptorSetLayout compute_layouts[1] = {impl_->frames->impl_->compute_layout};
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
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

    Handle<Kernel> h = impl_->res->kernels.Acquire();
    Kernel::Hot* hot = impl_->res->kernels.GetHot(h);
    hot->vk_pipeline = pipeline;
    hot->vk_layout = layout;
    impl_->res->kernels.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

Kernel::Hot* ResourceManager::GetHot(Handle<Kernel> h) {
    return impl_->res->kernels.GetHot(h);
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

uint32_t ResourceManager::GetBufferByteSize(Handle<Buffer> h) const {
    Buffer::Cold* cold = impl_->res->buffers.GetCold(h);
    if (!cold) {
        return 0;
    }
    return cold->size_bytes;
}

VkBuffer ResourceManager::GetVkBumpMasterBuffer(Memory mem) {
    return impl_->res->GetVkBumpMasterBuffer(mem);
}

uint32_t ResourceManager::BufferBaseOffset(Handle<Buffer> h) {
    return impl_->res->BufferBaseOffset(h);
}

VkBuffer ResourceManager::GetVkBuffer(Handle<Buffer> h, uint32_t* out_offset) {
    return impl_->res->GetVkBuffer(h, out_offset);
}

uint8_t* ResourceManager::MappedPtr(Handle<Buffer> h) {
    return impl_->res->MappedPtr(h);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
