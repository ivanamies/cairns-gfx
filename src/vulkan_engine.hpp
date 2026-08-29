#pragma once

#include "util/define.hpp"

#if CAIRNS_VULKAN

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>

// stb_image.h (decls + impl) comes from util/gltf_loader.hpp, included by
// main.cpp before this header. The implementations live in util/stb_impl.cpp.
// Do not define their implementations again here.
#include <stb_image_write.h>

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/util.hpp>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include <array>
#include <string>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <optional>
#include <set>
#include <fstream>
#include <chrono>
#include <random>
#include <filesystem>
#include <unordered_map>

#include "gpu_scene_registry.hpp"
#include "rhi/resource_manager.hpp"
#include "util/debug_asset.hpp"
#include "util/draw.hpp"
#include "util/draw_key.hpp"
#include "util/gltf_loader.hpp"
#include "util/material_gpu.hpp"
#include "util/misc.hpp"
#include "util/render_pass_globals.hpp"
#include "util/scene_gpu.hpp"
#include "util/std_allocator.hpp"
#include "util/frame_transient_cache.hpp"

#include <algorithm>
#include <numbers>

namespace cairns {

namespace vk_debug {
    inline constexpr uint64_t kNone          = 0;
    inline constexpr uint64_t kDumpSwapchain = 1ull << 0; // add TRANSFER_SRC to swapchain + dump a frame to PNG
}

// Default: no debug instrumentation. Flip to vk_debug::kDumpSwapchain to enable
// the swapchain readback (which also adds TRANSFER_SRC to the swapchain images).
inline constexpr uint64_t kVkDebugFlags = vk_debug::kNone;

constexpr bool vk_debug_has(uint64_t bit) {
    return (kVkDebugFlags & bit) != 0;
}

struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 3> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);
        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, texCoord);
        return attributeDescriptions;
    }

    bool operator==(const Vertex& other) const {
        return pos == other.pos && color == other.color && texCoord == other.texCoord;
    }
};

} // namespace cairns

namespace std {
template<> struct hash<cairns::Vertex> {
    size_t operator()(const cairns::Vertex& vertex) const {
        return ((hash<glm::vec3>()(vertex.pos) ^
                 (hash<glm::vec3>()(vertex.color) << 1)) >>1) ^
        (hash<glm::vec2>()(vertex.texCoord) << 1);
    }
};
}

namespace cairns {

struct UniformBufferObject {
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};

struct ParameterUBO {
    alignas(sizeof(float)) float deltaTime;
};

struct Particle {
    alignas(8) glm::vec2 position;
    alignas(8) glm::vec2 velocity;
    alignas(16) glm::vec4 color;

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Particle);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 2> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Particle, position);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Particle, color);

        return attributeDescriptions;
    }
};

const int MAX_FRAMES_IN_FLIGHT = 2;
const int PARTICLE_COUNT = 512;

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
                                                    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                                    VkDebugUtilsMessageTypeFlagsEXT messageType,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                    void* pUserData
                                                    ) {
    std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;
    return VK_FALSE;
}

static VkResult CreateDebugUtilsMessengerEXT(VkInstance instance,
                                             const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                             const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if ( func != nullptr ) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    }
    else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if ( func != nullptr ) {
        func(instance, debugMessenger, pAllocator);
    }
}

static bool readFile(const std::string& filename, std::vector<char>& buffer) {
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    size_t fileSize = (size_t)file.tellg();
    buffer.resize(fileSize);
    file.seekg(0);
    file.read(buffer.data(),fileSize);
    file.close();
    return true;
}

class Engine2 {
public:
    Engine2() :
        hot_arena_mem_(malloc(kHotArenaMemorySize)),
        hot_arena_(hot_arena_mem_, kHotArenaMemorySize),
        scenes_(Allocator<Scene>(hot_arena_))
    {}

    bool GreaterInit(SDL_Window* window) {
        window_ = window;
        return initVulkan();
    }

    bool Draw() {
        return drawFrame();
    }

    void Deinit() {
        vkDeviceWaitIdle(device);
        cleanup();
    }

    void RequestResizeFrameBuffer(int width, int height) {
        framebufferResized = true;
    }

private:

    bool loadScenes() {
        std::vector<std::filesystem::path> glb_paths;
        if (const char* env_glb = std::getenv("CAIRNS_GLB")) {
            std::string spec(env_glb);
            size_t start = 0;
            while (start <= spec.size()) {
                size_t comma = spec.find(',', start);
                std::string tok = spec.substr(
                    start, comma == std::string::npos ? std::string::npos
                                                      : comma - start);
                if (!tok.empty()) {
                    std::filesystem::path p(tok);
                    if (p.is_absolute()) {
                        glb_paths.push_back(p);
                    } else {
                        std::filesystem::path resolved;
                        if (!cairns::GetStaticResourceFilepath(tok, resolved)) {
                            return false;
                        }
                        glb_paths.push_back(resolved);
                    }
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else {
            for (size_t glb_idx = cairns::kDebugGlbsToParseStart;
                 glb_idx < cairns::kDebugGlbsToParseStart + cairns::kDebugGlbsToParse;
                 ++glb_idx) {
                std::filesystem::path filepath;
                if (!cairns::GetStaticResourceFilepath(cairns::kDebugGlbs[glb_idx],
                                                       filepath)) {
                    return false;
                }
                glb_paths.push_back(filepath);
            }
        }

        const int instance_count =
            std::getenv("CAIRNS_N") ? std::atoi(std::getenv("CAIRNS_N"))
                                    : static_cast<int>(glb_paths.size());
        const float target_size =
            std::getenv("CAIRNS_SCALE")
                ? static_cast<float>(std::atof(std::getenv("CAIRNS_SCALE")))
                : 0.8f;
        debugSceneXforms_ = cairns::GenerateDebugGridTransforms(
            glm::vec3(-1, -1, -3), 3, 1, 1, 1, 1.0f, instance_count);

        scene_norm_scales_.clear();
        for (const std::filesystem::path& filepath : glb_paths) {
            scenes_.push_back(cairns::Scene(hot_arena_));
            cairns::Scene& scene = scenes_.back();
            if (!cairns::LoadSceneFromGltf(filepath, scene)) {
                return false;
            }
            cairns::PrepareSceneResources(scene, rm_, materials_);
            if (!cairns::rhi::LoadSceneGpu(scene, rm_)) {
                return false;
            }
            glm::vec3 lo(1e9f);
            glm::vec3 hi(-1e9f);
            for (const auto& m : scene.meshes) {
                for (const auto& p : m.cpuPositions) {
                    lo = glm::min(lo, glm::vec3(p));
                    hi = glm::max(hi, glm::vec3(p));
                }
            }
            const glm::vec3 ext = hi - lo;
            const float max_ext = std::max({ext.x, ext.y, ext.z, 1e-6f});
            scene_norm_scales_.push_back(target_size / max_ext);
            scene.CleanupTmps();
        }
        return true;
    }

    bool BuildMeshOpaqueDraws() {
        drawList_.clear();
        drawListSorted_.clear();
        draw_material_offsets_.clear();
        draw_drawtmp_offsets_.clear();

        const char* freeze_rot = std::getenv("CAIRNS_FREEZE_ROT");
        const float angle_degs = freeze_rot
                                     ? static_cast<float>(std::atof(freeze_rot))
                                     : (SDL_GetTicks() / 1000.0 / 2.0 * 45);
        const float angle_rads = angle_degs * std::numbers::pi / 180.0f;
        const glm::mat4 rot_matrix = glm::rotate(glm::mat4(1.0f), angle_rads, glm::vec3(0, 1.0, 0));

        const glm::vec3 camera_pos(0, 0, 0);
        const glm::vec3 camera_dir(0, 0, -1);
        const glm::vec3 world_up(0, 1, 0);
        const glm::mat4 view_matrix = glm::lookAtRH(camera_pos, camera_pos + camera_dir, world_up);

        const float aspect_ratio = static_cast<float>(swapChainExtent.width) /
                                   static_cast<float>(swapChainExtent.height);
        const float fov = 90.0f * (std::numbers::pi / 180.0f);
        const float near_z = 0.1f;
        const float far_z = 100.0f;

        glm::mat4 proj_matrix = glm::perspectiveRH_ZO(fov, aspect_ratio, near_z, far_z);
        proj_matrix[1][1] *= -1;

        const glm::mat4 view_proj = proj_matrix * view_matrix;
        cairns::rhi::RenderPassGlobals render_pass_globals{
            .view_proj = view_proj,
            .inv_view_proj = glm::inverse(view_proj),
            .camera_pos = glm::vec4(camera_pos, 1.0f),
            .camera_dir = glm::vec4(camera_dir, near_z),
            .screen_params = glm::vec4(
                static_cast<float>(swapChainExtent.width),
                static_cast<float>(swapChainExtent.height),
                1.0f / static_cast<float>(swapChainExtent.width),
                1.0f / static_cast<float>(swapChainExtent.height))
        };
        void* gptr = rm_.BumpAllocate(
            sizeof(cairns::rhi::RenderPassGlobals), ubo_align_, rhi::Memory::kDynamic);
        memcpy(gptr, &render_pass_globals, sizeof(render_pass_globals));
        globals_offset_ = rm_.BumpOffset(gptr);

        for (size_t scene_xform_idx = 0; scene_xform_idx < debugSceneXforms_.size(); ++scene_xform_idx) {
            const size_t scene_idx = scene_xform_idx % scenes_.size();
            const glm::mat4& scene_xform = debugSceneXforms_[scene_xform_idx];
            cairns::Scene& scene = scenes_[scene_idx];

            std::vector<int32_t> node_stack;
            for (size_t j = 0; j < scene.rootNodes.size(); ++j) {
                node_stack.push_back(scene.rootNodes[j]);
            }
            while (!node_stack.empty()) {
                const int32_t node_idx = node_stack.back();
                node_stack.pop_back();
                const auto& node = scene.nodes[node_idx];
                if (node.meshIndex < 0) {
                    for (int32_t c : node.children) {
                        node_stack.push_back(c);
                    }
                    continue;
                }

                const auto& mesh = scene.meshes[node.meshIndex];
                const rhi::Handle<rhi::Buffer> pos = mesh.posHandle;
                const rhi::Handle<rhi::Buffer> index = mesh.indexHandle;

                for (const auto& prim : mesh.primitives) {
                    const uint32_t scene_mat_idx = prim.materialIndex;
                    const uint32_t mat_id = scene.materialIds[scene_mat_idx];
                    const rhi::Handle<rhi::Texture> tex_handle = materials_[mat_id].color;
                    const rhi::Handle<rhi::Sampler> sampler_handle = materials_[mat_id].sampler;

                    const uint32_t gpu_tex_id = texture_id_map_[tex_handle.index];
                    const uint32_t gpu_sampler_id = sampler_id_map_[sampler_handle.index];
                    const uint32_t gpu_attr_idx = mesh_attr_id_map_[mesh.attrHandle.index];

                    const cairns::rhi::MaterialGpu material_gpu{
                        .tex_color_id = gpu_tex_id,
                        .sampler_id = gpu_sampler_id,
                    };
                    void* mptr = rm_.BumpAllocate(
                        sizeof(cairns::rhi::MaterialGpu), ubo_align_, rhi::Memory::kDynamic);
                    memcpy(mptr, &material_gpu, sizeof(material_gpu));
                    const uint32_t material_offset = rm_.BumpOffset(mptr);

                    const glm::mat4 norm_scale = glm::scale(
                        glm::mat4(1.0f), glm::vec3(scene_norm_scales_[scene_idx]));
                    const glm::mat4 model_matrix = scene_xform * norm_scale * rot_matrix;
                    const cairns::rhi::DrawTmp draw_tmp{
                        .model_matrix = node.globalTransform * model_matrix,
                        .mesh_id = gpu_attr_idx,
                        .tex_id = gpu_tex_id,
                        .sampler_id = gpu_sampler_id
                    };
                    void* tptr = rm_.BumpAllocate(
                        sizeof(cairns::rhi::DrawTmp), ubo_align_, rhi::Memory::kDynamic);
                    memcpy(tptr, &draw_tmp, sizeof(draw_tmp));
                    const uint32_t drawtmp_offset = rm_.BumpOffset(tptr);

                    cairns::Draw draw{};
                    draw.index_buffer = index;
                    uint32_t index_base_off = 0;
                    rm_.GetVkBuffer(index, &index_base_off);
                    draw.index_offset = index_base_off + (prim.firstIndex * sizeof(uint32_t));
                    draw.vertex_offset = prim.vertexOffset;
                    draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot] = pos;
                    draw.instance_count = 1;
                    draw.instance_offset = 0;
                    assert(prim.indexCount % 3 == 0);
                    draw.triangle_count = prim.indexCount / 3;

                    draw_material_offsets_.push_back(material_offset);
                    draw_drawtmp_offsets_.push_back(drawtmp_offset);
                    drawListSorted_.emplace_back(cairns::BuildDrawKey(draw), static_cast<uint32_t>(drawList_.size()));
                    drawList_.push_back(draw);
                }

                for (int32_t c : node.children) {
                    node_stack.push_back(c);
                }
            }
        }

        return true;
    }

    bool initVulkan() {
        if (!createInstance()) return false;
        if (!setupDebugMessenger()) return false;
        if (!createSurface()) return false;
        if (!pickPhysicalDevice()) return false;
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(physicalDevice, &props);
            ubo_align_ = std::max(1u, static_cast<uint32_t>(
                props.limits.minUniformBufferOffsetAlignment));
        }
        if (!createLogicalDevice()) return false;
        if (!createSwapChain()) return false;
        if (!createImageViews()) return false;
        if (!createRenderPass()) return false;
        if (!createCommandPool()) return false;
        if (!initResourceManager()) return false;
        if (!loadScenes()) return false;
        if (!createBindlessRegistry()) return false;
        if (!createDescriptorSetLayout()) return false;
        if (!createComputePipeline()) return false;
        if (!createGraphicsPipeline()) return false;
        if (!createColorResources()) return false;
        if (!createDepthResources()) return false;
        if (!createFramebuffers()) return false;
        if (!createUniformBuffers()) return false;
        if (!createShaderStorageBuffers()) return false;
        if (!createDescriptorPool()) return false;
        if (!createDescriptorSets()) return false;
        if (!createCommandBuffers()) return false;
        if (!createComputeCommandBuffers()) return false;
        if (!createSyncObjects()) return false;
        return true;
    }

    bool loadModel() {
        std::filesystem::path path(MODEL_PATH);
        fastgltf::Parser parser;
        auto data = fastgltf::GltfDataBuffer::FromPath(path);
        if (data.error() != fastgltf::Error::None) {
            return false;
        }
        auto assetRes = parser.loadGltfBinary(data.get(), path.parent_path(), fastgltf::Options::None);
        if (assetRes.error() != fastgltf::Error::None) {
            return false;
        }
        const fastgltf::Asset& asset = assetRes.get();

        for (const auto& mesh : asset.meshes) {
            for (const auto& primitive : mesh.primitives) {
                const uint32_t baseVertex = static_cast<uint32_t>(vertices.size());
                size_t vertexCount = 0;

                const auto* posIt = primitive.findAttribute("POSITION");
                if (posIt != primitive.attributes.end()) {
                    auto& accessor = asset.accessors[posIt->accessorIndex];
                    vertexCount = accessor.count;
                    vertices.resize(baseVertex + vertexCount);
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, accessor, [&](glm::vec3 v, size_t i) {
                        vertices[baseVertex + i].pos = v;
                        vertices[baseVertex + i].color = glm::vec3(1.0f);
                        vertices[baseVertex + i].texCoord = glm::vec2(0.0f);
                    });
                }

                const auto* uvIt = primitive.findAttribute("TEXCOORD_0");
                if (uvIt != primitive.attributes.end()) {
                    auto& accessor = asset.accessors[uvIt->accessorIndex];
                    fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, accessor, [&](glm::vec2 v, size_t i) {
                        vertices[baseVertex + i].texCoord = v;
                    });
                }

                if (primitive.indicesAccessor.has_value()) {
                    auto& accessor = asset.accessors[*primitive.indicesAccessor];
                    fastgltf::iterateAccessorWithIndex<uint32_t>(asset, accessor, [&](uint32_t idx, size_t) {
                        indices.push_back(baseVertex + idx);
                    });
                }
            }
        }
        return true;
    }

    bool findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features, VkFormat& out) {
        for ( VkFormat format : candidates) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);
            if ( tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features ) {
                out = format;
                return true;
            }
            else if ( tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features ) {
                out = format;
                return true;
            }
        }
        return false;
    }

    bool createColorResources() {
        VkFormat colorFormat = swapChainImageFormat;
        if (!createImage(swapChainExtent.width, swapChainExtent.height, 1, msaaSamples, colorFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, colorImage, colorImageMemory)) return false;
        if (!createImageView(colorImage, colorFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1, colorImageView)) return false;
        return true;
    }

    bool findDepthFormat(VkFormat& out) {
        return findSupportedFormat({VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},VK_IMAGE_TILING_OPTIMAL,VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, out);
    }

    bool hasStencilComponent(VkFormat format) {
        return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
    }

    bool createDepthResources() {
        VkFormat depthFormat;
        if (!findDepthFormat(depthFormat)) return false;
        if (!createImage(swapChainExtent.width, swapChainExtent.height, 1, msaaSamples, depthFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage, depthImageMemory)) return false;
        if (!createImageView(depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT, 1, depthImageView)) return false;
        if (!transitionImageLayout(depthImage, depthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 1)) return false;
        return true;
    }

    bool createTextureSampler() {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);

        rhi::SamplerDesc desc{};
        desc.debug_name = "texture_sampler";
        desc.mag_filter = rhi::Filter::kLinear;
        desc.min_filter = rhi::Filter::kLinear;
        desc.mip_filter = rhi::Filter::kLinear;
        desc.address_mode = rhi::AddressMode::kRepeat;
        desc.max_anisotropy = properties.limits.maxSamplerAnisotropy;
        desc.max_lod = static_cast<float>(mipLevels);
        sampler_ = rm_.CreateSampler(desc);
        return !sampler_.IsNull();
    }

    bool createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels, VkImageView& out) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspectFlags;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        if ( vkCreateImageView(device, &viewInfo, nullptr, &out) != VK_SUCCESS ) {
            return false;
        }
        return true;
    }

    bool createTextureImage() {
        int texWidth, texHeight, texChannels;
        stbi_uc* pixels = stbi_load(TEXTURE_PATH.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);
        if ( !pixels ) {
            return false;
        }
        VkDeviceSize imageSize = texWidth * texHeight * 4;
        mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(texWidth, texHeight)))) + 1;

        rhi::TextureDesc desc{};
        desc.debug_name = "viking_room";
        desc.dimensions = { texWidth, texHeight, 1 };
        desc.mip_levels = mipLevels;
        desc.array_layers = 1;
        desc.format = rhi::Format::kRgba8Srgb;
        desc.usage = rhi::kTexUsageSampled;
        desc.memory = rhi::Memory::kDefault;
        desc.initial_data = rhi::Span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(pixels),
            static_cast<size_t>(imageSize));
        texture_ = rm_.CreateTexture(desc);
        stbi_image_free(pixels);
        return !texture_.IsNull();
    }

    bool createImage(uint32_t width, uint32_t height, uint32_t mipLevels, VkSampleCountFlagBits numSamples, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = tiling;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = numSamples;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.flags = 0;

        if ( vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS ) {
            return false;
        }

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device, image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        if (!findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, allocInfo.memoryTypeIndex)) return false;

        if ( vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
            return false;
        }
        vkBindImageMemory(device, image, imageMemory, 0);
        return true;
    }

    bool createBindlessRegistry() {
        using R = cairns::rhi::GpuSceneRegistry;

        VkDescriptorBindingFlags binding_flags[3] = {
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
        flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flags_info.bindingCount = 3;
        flags_info.pBindingFlags = binding_flags;

        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bindings[0].descriptorCount = R::kMaxTextures;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = R::kMaxMeshes;
        bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bindings[2].descriptorCount = R::kMaxSamplers;
        bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout_info.bindingCount = 3;
        layout_info.pBindings = bindings;
        layout_info.pNext = &flags_info;

        if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &bindlessLayout_) != VK_SUCCESS) {
            return false;
        }

        VkDescriptorPoolSize pool_sizes[3]{};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        pool_sizes[0].descriptorCount = R::kMaxTextures;
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_sizes[1].descriptorCount = R::kMaxMeshes;
        pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
        pool_sizes[2].descriptorCount = R::kMaxSamplers;

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 3;
        pool_info.pPoolSizes = pool_sizes;

        if (vkCreateDescriptorPool(device, &pool_info, nullptr, &bindlessPool_) != VK_SUCCESS) {
            return false;
        }

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = bindlessPool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &bindlessLayout_;

        if (vkAllocateDescriptorSets(device, &alloc_info, &bindlessSet_) != VK_SUCCESS) {
            return false;
        }

        texture_id_map_.clear();
        mesh_attr_id_map_.clear();
        sampler_id_map_.clear();

        std::vector<VkDescriptorImageInfo> tex_infos;
        std::vector<VkDescriptorBufferInfo> attr_infos;
        std::vector<VkDescriptorImageInfo> sampler_infos;

        for (size_t i = 0; i < scenes_.size(); ++i) {
            cairns::Scene& scene = scenes_[i];
            for (size_t j = 0; j < scene.textureHandles.size(); ++j) {
                auto h = scene.textureHandles[j];
                rhi::Texture::Hot* hot = rm_.GetHot(h);
                if (hot && hot->api_view) {
                    VkDescriptorImageInfo img{};
                    img.imageView = static_cast<VkImageView>(hot->api_view);
                    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    tex_infos.push_back(img);
                    texture_id_map_[h.index] =
                        static_cast<uint32_t>(tex_infos.size()) - 1;
                }
            }
            for (size_t j = 0; j < scene.meshes.size(); ++j) {
                auto h = scene.meshes[j].attrHandle;
                if (!h.IsNull()) {
                    uint32_t off = 0;
                    VkBuffer buf = rm_.GetVkBuffer(h, &off);
                    if (buf != VK_NULL_HANDLE) {
                        VkDescriptorBufferInfo buf_info{};
                        buf_info.buffer = buf;
                        buf_info.offset = off;
                        buf_info.range = rm_.GetBufferByteSize(h);
                        attr_infos.push_back(buf_info);
                        mesh_attr_id_map_[h.index] = static_cast<uint32_t>(attr_infos.size()) - 1;
                    }
                }
            }
            for (size_t j = 0; j < scene.samplerHandles.size(); ++j) {
                auto h = scene.samplerHandles[j];
                rhi::Sampler::Hot* hot = rm_.GetHot(h);
                if (hot && hot->api_sampler) {
                    VkDescriptorImageInfo samp{};
                    samp.sampler = static_cast<VkSampler>(hot->api_sampler);
                    sampler_infos.push_back(samp);
                    sampler_id_map_[h.index] = static_cast<uint32_t>(sampler_infos.size()) - 1;
                }
            }
        }

        std::vector<VkWriteDescriptorSet> writes;
        if (!tex_infos.empty()) {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = bindlessSet_;
            w.dstBinding = 0;
            w.dstArrayElement = 0;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.descriptorCount = static_cast<uint32_t>(tex_infos.size());
            w.pImageInfo = tex_infos.data();
            writes.push_back(w);
        }
        if (!attr_infos.empty()) {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = bindlessSet_;
            w.dstBinding = 1;
            w.dstArrayElement = 0;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.descriptorCount = static_cast<uint32_t>(attr_infos.size());
            w.pBufferInfo = attr_infos.data();
            writes.push_back(w);
        }
        if (!sampler_infos.empty()) {
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = bindlessSet_;
            w.dstBinding = 2;
            w.dstArrayElement = 0;
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            w.descriptorCount = static_cast<uint32_t>(sampler_infos.size());
            w.pImageInfo = sampler_infos.data();
            writes.push_back(w);
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(device,
                static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        return true;
    }

    bool createDescriptorPool() {
        std::array<VkDescriptorPoolSize, 3> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = 2 * static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        poolSizes[2].descriptorCount = 3 * static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = 3 * static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        if ( vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    bool createDescriptorSets() {
        {
            std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, descriptorSetLayout2);
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = descriptorPool;
            allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
            allocInfo.pSetLayouts = layouts.data();

            descriptorSets2.resize(MAX_FRAMES_IN_FLIGHT);
            if ( vkAllocateDescriptorSets(device, &allocInfo, descriptorSets2.data())) {
                return false;
            }

            // all data is in the attributes
        }
        {
            std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, computeDescriptorSetLayout);
            VkDescriptorSetAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = descriptorPool;
            allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
            allocInfo.pSetLayouts = layouts.data();

            computeDescriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
            if ( vkAllocateDescriptorSets(device, &allocInfo, computeDescriptorSets.data())) {
                return false;
            }

            for ( size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
                uint32_t computeUboOffset = 0;
                VkDescriptorBufferInfo uniformBufferInfo{};
                uniformBufferInfo.buffer = rm_.GetVkBuffer(compute_uniform_buffers_[i], &computeUboOffset);
                uniformBufferInfo.offset = computeUboOffset;
                uniformBufferInfo.range = sizeof(ParameterUBO);

                std::array<VkWriteDescriptorSet, 3> descriptorWrites{};
                descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrites[0].dstSet = computeDescriptorSets[i];
                descriptorWrites[0].dstBinding = 0;
                descriptorWrites[0].dstArrayElement = 0;
                descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                descriptorWrites[0].descriptorCount = 1;
                descriptorWrites[0].pBufferInfo = &uniformBufferInfo;

                uint32_t lastFrameOffset = 0;
                VkDescriptorBufferInfo storageBufferInfoLastFrame{};
                storageBufferInfoLastFrame.buffer = rm_.GetVkBuffer(ssbo_[(i-1) % MAX_FRAMES_IN_FLIGHT], &lastFrameOffset);
                storageBufferInfoLastFrame.offset = lastFrameOffset;
                storageBufferInfoLastFrame.range = sizeof(Particle) * PARTICLE_COUNT;

                descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrites[1].dstSet = computeDescriptorSets[i];
                descriptorWrites[1].dstBinding = 1;
                descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorWrites[1].descriptorCount = 1;
                descriptorWrites[1].pBufferInfo = &storageBufferInfoLastFrame;

                uint32_t currentFrameOffset = 0;
                VkDescriptorBufferInfo storageBufferInfoCurrentFrame{};
                storageBufferInfoCurrentFrame.buffer = rm_.GetVkBuffer(ssbo_[i], &currentFrameOffset);
                storageBufferInfoCurrentFrame.offset = currentFrameOffset;
                storageBufferInfoCurrentFrame.range = sizeof(Particle) * PARTICLE_COUNT;

                descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                descriptorWrites[2].dstSet = computeDescriptorSets[i];
                descriptorWrites[2].dstBinding = 2;
                descriptorWrites[2].dstArrayElement = 0;
                descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                descriptorWrites[2].descriptorCount = 1;
                descriptorWrites[2].pBufferInfo = &storageBufferInfoCurrentFrame;

                vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
            }
        }
        {
            std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, dynamicUboLayout_);
            dynUboSets_.resize(MAX_FRAMES_IN_FLIGHT);
            VkDescriptorSetAllocateInfo alloc_info{};
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = descriptorPool;
            alloc_info.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
            alloc_info.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(device, &alloc_info, dynUboSets_.data()) != VK_SUCCESS) {
                return false;
            }
        }
        return true;
    }

    bool createShaderStorageBuffers() {
        std::default_random_engine rndEngine(42);
        std::uniform_real_distribution<float> rndDist(0.0f, 1.0f);

        std::vector<Particle> particles(PARTICLE_COUNT);
        for ( auto& particle : particles ) {
            float r = 0.25f * std::sqrt(rndDist(rndEngine));
            float theta = rndDist(rndEngine) * 2 * M_PI;
            float x = r * cos(theta) * HEIGHT / WIDTH;
            float y = r * sin(theta);
            particle.position = glm::vec2(x,y);
            particle.velocity = glm::normalize(glm::vec2(x,y)) * 0.25f;
            particle.color = glm::vec4(rndDist(rndEngine), rndDist(rndEngine), rndDist(rndEngine), 1.0f);
        }

        VkDeviceSize bufferSize = sizeof(Particle) * PARTICLE_COUNT;
        rhi::Span<const uint8_t> init(
            reinterpret_cast<const uint8_t*>(particles.data()),
            static_cast<size_t>(bufferSize));

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            rhi::BufferDesc desc{};
            desc.debug_name = "ssbo";
            desc.byte_size = static_cast<uint32_t>(bufferSize);
            desc.usage = rhi::kUsageStorage | rhi::kUsageVertex |
                         rhi::kUsageTransferDst;
            desc.memory = rhi::Memory::kDefault;
            desc.initial_data = init;
            ssbo_[i] = rm_.CreateBuffer(desc);
            if (ssbo_[i].IsNull()) {
                return false;
            }
        }
        return true;
    }

    bool createUniformBuffers() {
        {
            VkDeviceSize bufferSize = sizeof(UniformBufferObject);
            for ( size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
                rhi::BufferDesc desc{};
                desc.debug_name = "ubo";
                desc.byte_size = static_cast<uint32_t>(bufferSize);
                desc.usage = rhi::kUsageUniform;
                desc.memory = rhi::Memory::kUpload;
                uniform_buffers_[i] = rm_.CreateBuffer(desc);
                if (uniform_buffers_[i].IsNull()) {
                    return false;
                }
                uniformBuffersMapped[i] = rm_.MappedPtr(uniform_buffers_[i]);
            }
        }
        {
            VkDeviceSize bufferSize = sizeof(ParameterUBO);
            for ( size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
                rhi::BufferDesc desc{};
                desc.debug_name = "compute_ubo";
                desc.byte_size = static_cast<uint32_t>(bufferSize);
                desc.usage = rhi::kUsageUniform;
                desc.memory = rhi::Memory::kUpload;
                compute_uniform_buffers_[i] = rm_.CreateBuffer(desc);
                if (compute_uniform_buffers_[i].IsNull()) {
                    return false;
                }
                computeUniformBuffersMapped[i] = rm_.MappedPtr(compute_uniform_buffers_[i]);
            }
        }
        return true;
    }

    bool createDescriptorSetLayout() {
        {
            VkDescriptorSetLayoutBinding uboLayoutBinding{};
            uboLayoutBinding.binding = 0;
            uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uboLayoutBinding.descriptorCount = 1;
            uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            uboLayoutBinding.pImmutableSamplers = nullptr;

            VkDescriptorSetLayoutBinding samplerLayoutBinding{};
            samplerLayoutBinding.binding = 1;
            samplerLayoutBinding.descriptorCount = 1;
            samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            samplerLayoutBinding.pImmutableSamplers = nullptr;
            samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            std::array<VkDescriptorSetLayoutBinding, 2> bindings = { uboLayoutBinding,
                samplerLayoutBinding};

            VkDescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();

            if ( vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS ) {
                return false;
            }
        }
        {
            std::array<VkDescriptorSetLayoutBinding, 0> bindings = { };

            VkDescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();

            if ( vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout2) != VK_SUCCESS ) {
                return false;
            }

        }

        {
            std::array<VkDescriptorSetLayoutBinding, 3> layoutBindings{};
            layoutBindings[0].binding = 0;
            layoutBindings[0].descriptorCount = 1;
            layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            layoutBindings[0].pImmutableSamplers = nullptr;
            layoutBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            layoutBindings[1].binding = 1;
            layoutBindings[1].descriptorCount = 1;
            layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            layoutBindings[1].pImmutableSamplers = nullptr;
            layoutBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            layoutBindings[2].binding = 2;
            layoutBindings[2].descriptorCount = 1;
            layoutBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            layoutBindings[2].pImmutableSamplers = nullptr;
            layoutBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutInfo{};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = 3;
            layoutInfo.pBindings = layoutBindings.data();

            if ( vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &computeDescriptorSetLayout) != VK_SUCCESS ) {
                return false;
            }
        }

        {
            VkDescriptorSetLayoutBinding dyn_bindings[3]{};
            dyn_bindings[0].binding = 0;
            dyn_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            dyn_bindings[0].descriptorCount = 1;
            dyn_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            dyn_bindings[1].binding = 1;
            dyn_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            dyn_bindings[1].descriptorCount = 1;
            dyn_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            dyn_bindings[2].binding = 2;
            dyn_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            dyn_bindings[2].descriptorCount = 1;
            dyn_bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo dyn_layout_info{};
            dyn_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dyn_layout_info.bindingCount = 3;
            dyn_layout_info.pBindings = dyn_bindings;
            if (vkCreateDescriptorSetLayout(device, &dyn_layout_info, nullptr, &dynamicUboLayout_) != VK_SUCCESS) {
                return false;
            }
        }

        return true;
    }

    bool createIndexBuffer() {
        VkDeviceSize bufferSize = sizeof(indices[0]) * indices.size();
        rhi::BufferDesc desc{};
        desc.debug_name = "index";
        desc.byte_size = static_cast<uint32_t>(bufferSize);
        desc.usage = rhi::kUsageIndex | rhi::kUsageTransferDst;
        desc.memory = rhi::Memory::kDefault;
        desc.initial_data = rhi::Span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(indices.data()),
            static_cast<size_t>(bufferSize));
        index_buffer_ = rm_.CreateBuffer(desc);
        return !index_buffer_.IsNull();
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if ( vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS ) {
            return false;
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        if (!findMemoryType(memRequirements.memoryTypeBits, properties, allocInfo.memoryTypeIndex)) return false;

        if ( vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS ) {
            return false;
        }

        vkBindBufferMemory(device, buffer, bufferMemory, 0);

        return true;
    }

    bool generateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels) {

        VkFormatProperties formatProperties;
        vkGetPhysicalDeviceFormatProperties(physicalDevice, imageFormat, &formatProperties);
        if ( !(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
            return false;
        }

        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = image;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.subresourceRange.levelCount = 1;

        int32_t mipWidth = texWidth;
        int32_t mipHeight = texHeight;
        for ( uint32_t i = 1; i < mipLevels; ++i ) {
            barrier.subresourceRange.baseMipLevel = i - 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr,
                                 0, nullptr,
                                 1, &barrier);
            VkImageBlit blit{};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = { mipWidth, mipHeight, 1 };
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = 1;
            blit.dstOffsets[0] = { 0, 0, 0 };
            blit.dstOffsets[1] = { mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = 1;
            vkCmdBlitImage(commandBuffer,
                           image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit,
                           VK_FILTER_LINEAR);
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, nullptr,
                                 0, nullptr,
                                 1, &barrier);
            if (mipWidth > 1) mipWidth /= 2;
            if (mipHeight > 1) mipHeight /= 2;
        }

        barrier.subresourceRange.baseMipLevel = mipLevels - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);
        endSingleTimeCommands(commandBuffer);
        return true;
    }

    bool transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        if ( newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ) {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if( hasStencilComponent(format)) {
                barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        }
        else {
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        }
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = mipLevels;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0; // TODO
        barrier.dstAccessMask = 0; // TODO

        VkPipelineStageFlags sourceStage;
        VkPipelineStageFlags destinationStage;

        if ( oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if ( oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else if ( oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        }
        else {
            return false;
        }

        vkCmdPipelineBarrier(
                             commandBuffer,
                             sourceStage, destinationStage,
                             0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier
                             );

        endSingleTimeCommands(commandBuffer);
        return true;
    }

    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {
            width,
            height,
            1
        };
        vkCmdCopyBufferToImage(
                               commandBuffer,
                               buffer,
                               image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1,
                               &region
                               );
        endSingleTimeCommands(commandBuffer);
    }

    VkCommandBuffer beginSingleTimeCommands() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = commandPool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        return commandBuffer;
    }

    void endSingleTimeCommands(VkCommandBuffer commandBuffer) {
        vkEndCommandBuffer(commandBuffer);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(graphicsQueue);

        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    }

    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {

        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = 0;
        copyRegion.size = size;

        vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

        endSingleTimeCommands(commandBuffer);
    }

    bool createVertexBuffer() {
        VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();
        rhi::BufferDesc desc{};
        desc.debug_name = "vertex";
        desc.byte_size = static_cast<uint32_t>(bufferSize);
        desc.usage = rhi::kUsageVertex | rhi::kUsageTransferDst;
        desc.memory = rhi::Memory::kDefault;
        desc.initial_data = rhi::Span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(vertices.data()),
            static_cast<size_t>(bufferSize));
        vertex_buffer_ = rm_.CreateBuffer(desc);
        return !vertex_buffer_.IsNull();
    }

    bool findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties, uint32_t& out) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

        for ( uint32_t i = 0; i < memProperties.memoryTypeCount; ++i ) {
            if ( (typeFilter & (1<<i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                out = i;
                return true;
            }
        }
        return false;
    }

    void recreateSwapChain() {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        while (width == 0 || height == 0) {
            SDL_GetWindowSizeInPixels(window_, &width, &height);
            SDL_Delay(10);
        }

        vkDeviceWaitIdle(device);

        cleanupSwapChain();

        createSwapChain();
        createColorResources();
        createImageViews();
        createDepthResources();
        createFramebuffers();
    }

    bool createSyncObjects() {
        {
            imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
            renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
            inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
                if ( vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS || vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
                    vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS ) {
                    return false;
                }
            }
        }
        {
            computeInFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
            computeFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);

            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            for ( size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
                if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &computeFinishedSemaphores[i]) != VK_SUCCESS || vkCreateFence(device, &fenceInfo, nullptr, &computeInFlightFences[i]) != VK_SUCCESS ) {
                    return false;
                }
            }
        }
        return true;
    }

    bool createCommandBuffers() {
        commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)commandBuffers.size();
        if ( vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS ) {
            return false;
        }
        return true;
    }

    bool createCommandBuffers2() {
        commandBuffers2.resize(MAX_FRAMES_IN_FLIGHT);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)commandBuffers2.size();
        if ( vkAllocateCommandBuffers(device, &allocInfo, commandBuffers2.data()) != VK_SUCCESS ) {
            return false;
        }
        return true;
    }

    bool createComputeCommandBuffers() {
        computeCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t)computeCommandBuffers.size();
        if ( vkAllocateCommandBuffers(device, &allocInfo, computeCommandBuffers.data()) != VK_SUCCESS ) {
            return false;
        }
        return true;
    }

    bool recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = 0;
        beginInfo.pInheritanceInfo = nullptr;

        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS ) {
            return false;
        }
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass;
        renderPassInfo.framebuffer = swapChainFramebuffers[imageIndex];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = swapChainExtent;
        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};
        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        {
            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = static_cast<float>(swapChainExtent.width);
            viewport.height = static_cast<float>(swapChainExtent.height);
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.offset = { 0, 0 };
            scissor.extent = swapChainExtent;
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        }
        {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipelineLayout, 0, 1, &bindlessSet_, 0, nullptr);

            for (const auto& [key, draw_idx] : drawListSorted_) {
                const cairns::Draw& draw = drawList_[draw_idx];
                uint32_t pos_off = 0;
                VkBuffer pos_buf = rm_.GetVkBuffer(
                    draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot], &pos_off);
                VkDeviceSize pos_off_dev = pos_off +
                    static_cast<VkDeviceSize>(draw.vertex_offset) * sizeof(glm::vec4);
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, &pos_buf, &pos_off_dev);

                uint32_t idx_base = 0;
                VkBuffer idx_buf = rm_.GetVkBuffer(draw.index_buffer, &idx_base);
                vkCmdBindIndexBuffer(commandBuffer, idx_buf, idx_base, VK_INDEX_TYPE_UINT32);
                const uint32_t first_index = (draw.index_offset - idx_base) / sizeof(uint32_t);

                std::array<uint32_t, 3> dyn_offsets = {
                    globals_offset_,
                    draw_material_offsets_[draw_idx],
                    draw_drawtmp_offsets_[draw_idx]
                };
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    pipelineLayout, 1, 1, &dynUboSets_[currentFrame],
                    static_cast<uint32_t>(dyn_offsets.size()), dyn_offsets.data());

                const uint32_t base_vertex = draw.vertex_offset;
                vkCmdPushConstants(commandBuffer, pipelineLayout,
                    VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &base_vertex);

                vkCmdDrawIndexed(commandBuffer,
                    draw.triangle_count * 3,
                    draw.instance_count,
                    first_index,
                    0,
                    draw.instance_offset);
            }
        }
        {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline2);

            uint32_t ssboOffset = 0;
            VkBuffer ssboBuf = rm_.GetVkBuffer(ssbo_[currentFrame], &ssboOffset);
            std::array<VkBuffer,1> vertexBuffers = {ssboBuf};
            VkDeviceSize offsets[] = { ssboOffset };
            vkCmdBindVertexBuffers(commandBuffer, 0, vertexBuffers.size(), vertexBuffers.data(), offsets);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout2, 0, 1, &descriptorSets2[currentFrame], 0, nullptr);

            vkCmdDraw(commandBuffer, PARTICLE_COUNT, 1, 0, 0);
        }

        vkCmdEndRenderPass(commandBuffer);

        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS ) {
            return false;
        }
        return true;
    }

    bool recordComputeCommandBuffer(VkCommandBuffer commandBuffer) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if ( vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS ) {
            return false;
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipelineLayout, 0, 1, &computeDescriptorSets[currentFrame], 0, 0);
        vkCmdDispatch(commandBuffer, PARTICLE_COUNT / 256, 1, 1);

        if ( vkEndCommandBuffer(commandBuffer) != VK_SUCCESS ) {
            return false;
        }
        return true;
    }

    bool createCommandPool() {
        QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsAndComputeFamily.value();
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS ) {
            return false;
        }
        return true;
    }

    bool initResourceManager() {
        QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);
        rhi::BackendInitParams params{};
        params.instance = instance;
        params.physical = physicalDevice;
        params.device = device;
        params.queue = graphicsQueue;
        params.queue_family_index = queueFamilyIndices.graphicsAndComputeFamily.value();
        params.command_pool = commandPool;
        params.enable_bda = false;
        return rm_.Init(params);
    }

    bool createFramebuffers() {
        swapChainFramebuffers.resize(swapChainImageViews.size());
        for ( size_t i = 0; i < swapChainImageViews.size(); ++i ) {
            std::array<VkImageView,3> attachments = {
                colorImageView,
                depthImageView,
                swapChainImageViews[i],
            };
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            framebufferInfo.pAttachments = attachments.data();
            framebufferInfo.width = swapChainExtent.width;
            framebufferInfo.height = swapChainExtent.height;
            framebufferInfo.layers = 1;

            if ( vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[i])) {
                return false;
            }
        }
        return true;
    }

    bool createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat;
        colorAttachment.samples = msaaSamples;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription colorAttachmentResolve{};
        colorAttachmentResolve.format = swapChainImageFormat;
        colorAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorAttachmentResolveRef{};
        colorAttachmentResolveRef.attachment = 2;
        colorAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAttachment{};
        VkFormat depthFormat;
        if (!findDepthFormat(depthFormat)) return false;
        depthAttachment.format = depthFormat;
        depthAttachment.samples = msaaSamples;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pResolveAttachments = &colorAttachmentResolveRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        std::array<VkAttachmentDescription, 3> attachments = { colorAttachment, depthAttachment, colorAttachmentResolve };
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        if(vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    bool createGraphicsPipeline() {
        { // create graphics pipeline1
            const char* sdl_base = SDL_GetBasePath();
            const std::filesystem::path base_path = sdl_base ? sdl_base : "";
            std::vector<char> vertShaderCode;
            if (!readFile((base_path / "unlit.vert.spv").string(), vertShaderCode)) return false;
            std::vector<char> fragShaderCode;
            if (!readFile((base_path / "unlit.frag.spv").string(), fragShaderCode)) return false;

            VkShaderModule vertShaderModule;
            if (!createShaderModule(vertShaderCode, vertShaderModule)) return false;
            VkShaderModule fragShaderModule;
            if (!createShaderModule(fragShaderCode, fragShaderModule)) return false;

            VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
            vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vertShaderStageInfo.module = vertShaderModule;
            vertShaderStageInfo.pName = "main";

            VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
            fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fragShaderStageInfo.module = fragShaderModule;
            fragShaderStageInfo.pName = "main";

            std::array<VkPipelineShaderStageCreateInfo,2> shaderStages = { vertShaderStageInfo, fragShaderStageInfo
            };

            std::vector<VkDynamicState> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR
            };
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamicState.pDynamicStates = dynamicStates.data();

            VkVertexInputBindingDescription pos_binding{};
            pos_binding.binding = 0;
            pos_binding.stride = sizeof(glm::vec4);
            pos_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            VkVertexInputAttributeDescription pos_attr{};
            pos_attr.binding = 0;
            pos_attr.location = 0;
            pos_attr.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            pos_attr.offset = 0;
            VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
            vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInputInfo.vertexBindingDescriptionCount = 1;
            vertexInputInfo.pVertexBindingDescriptions = &pos_binding;
            vertexInputInfo.vertexAttributeDescriptionCount = 1;
            vertexInputInfo.pVertexAttributeDescriptions = &pos_attr;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            inputAssembly.primitiveRestartEnable = VK_FALSE;

            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = (float)swapChainExtent.width;
            viewport.height = (float)swapChainExtent.height;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            VkRect2D scissor{};
            scissor.offset = { 0, 0 };
            scissor.extent = swapChainExtent;

            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.pViewports = &viewport;
            viewportState.scissorCount = 1;
            viewportState.pScissors = &scissor;

            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.depthClampEnable = VK_FALSE;
            rasterizer.rasterizerDiscardEnable = VK_FALSE;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.lineWidth = 1.0f;
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizer.depthBiasEnable = VK_FALSE;
            rasterizer.depthBiasConstantFactor = 0.0f;
            rasterizer.depthBiasClamp = 0.0f;
            rasterizer.depthBiasSlopeFactor = 0.0f;

            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.sampleShadingEnable = VK_FALSE;
            multisampling.rasterizationSamples = msaaSamples;
            multisampling.minSampleShading = 1.0f;
            multisampling.pSampleMask = nullptr;
            multisampling.alphaToCoverageEnable = VK_FALSE;
            multisampling.alphaToOneEnable = VK_FALSE;

            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            colorBlendAttachment.blendEnable = VK_FALSE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

            VkPipelineColorBlendStateCreateInfo colorBlending{};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.logicOpEnable = VK_FALSE;
            colorBlending.logicOp = VK_LOGIC_OP_COPY;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &colorBlendAttachment;
            colorBlending.blendConstants[0] = 0.0f;
            colorBlending.blendConstants[1] = 0.0f;
            colorBlending.blendConstants[2] = 0.0f;
            colorBlending.blendConstants[3] = 0.0f;

            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
            depthStencil.depthBoundsTestEnable = VK_FALSE;
            depthStencil.minDepthBounds = 0.0f;
            depthStencil.maxDepthBounds = 1.0f;
            depthStencil.stencilTestEnable = VK_FALSE;
            depthStencil.front = {};
            depthStencil.back = {};

            VkDescriptorSetLayout unlit_layouts[2] = {bindlessLayout_, dynamicUboLayout_};
            VkPushConstantRange pc_range{};
            pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            pc_range.offset = 0;
            pc_range.size = sizeof(uint32_t);
            VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
            pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipelineLayoutInfo.setLayoutCount = 2;
            pipelineLayoutInfo.pSetLayouts = unlit_layouts;
            pipelineLayoutInfo.pushConstantRangeCount = 1;
            pipelineLayoutInfo.pPushConstantRanges = &pc_range;

            if ( vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS ) {
                return false;
            }

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
            pipelineInfo.pStages = shaderStages.data();
            pipelineInfo.pVertexInputState = &vertexInputInfo;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.layout = pipelineLayout;
            pipelineInfo.renderPass = renderPass;
            pipelineInfo.subpass = 0;
            pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
            pipelineInfo.basePipelineIndex = -1;

            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
                return false;
            }
            vkDestroyShaderModule(device, fragShaderModule, nullptr);
            vkDestroyShaderModule(device, vertShaderModule, nullptr);
        }
        { // create graphics pipeline 2
            std::vector<char> vertShaderCode;
            if (!readFile("/Users/ivanamies/dev/gfx/Vulkan/vulkan-tutorial-dot-com/src/VulkanTesting/VulkanTesting/vert2.spv", vertShaderCode)) return false;
            std::vector<char> fragShaderCode;
            if (!readFile("/Users/ivanamies/dev/gfx/Vulkan/vulkan-tutorial-dot-com/src/VulkanTesting/VulkanTesting/frag2.spv", fragShaderCode)) return false;

            VkShaderModule vertShaderModule;
            if (!createShaderModule(vertShaderCode, vertShaderModule)) return false;
            VkShaderModule fragShaderModule;
            if (!createShaderModule(fragShaderCode, fragShaderModule)) return false;

            VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
            vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vertShaderStageInfo.module = vertShaderModule;
            vertShaderStageInfo.pName = "main";

            VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
            fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fragShaderStageInfo.module = fragShaderModule;
            fragShaderStageInfo.pName = "main";

            std::array<VkPipelineShaderStageCreateInfo,2> shaderStages = { vertShaderStageInfo, fragShaderStageInfo
            };

            std::vector<VkDynamicState> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR
            };
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamicState.pDynamicStates = dynamicStates.data();

            VkPipelineVertexInputStateCreateInfo vertexInputInfo{};

            auto bindingDescription = Particle::getBindingDescription();
            auto attributeDescriptions = Particle::getAttributeDescriptions();

            vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInputInfo.vertexBindingDescriptionCount = 1;
            vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
            vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
            vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            inputAssembly.primitiveRestartEnable = VK_FALSE;

            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = 0.0f;
            viewport.width = (float)swapChainExtent.width;
            viewport.height = (float)swapChainExtent.height;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;

            VkRect2D scissor{};
            scissor.offset = { 0, 0 };
            scissor.extent = swapChainExtent;

            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.pViewports = &viewport;
            viewportState.scissorCount = 1;
            viewportState.pScissors = &scissor;

            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.depthClampEnable = VK_FALSE;
            rasterizer.rasterizerDiscardEnable = VK_FALSE;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.lineWidth = 1.0f;
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterizer.depthBiasEnable = VK_FALSE;
            rasterizer.depthBiasConstantFactor = 0.0f;
            rasterizer.depthBiasClamp = 0.0f;
            rasterizer.depthBiasSlopeFactor = 0.0f;

            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.sampleShadingEnable = VK_FALSE;
            multisampling.rasterizationSamples = msaaSamples;
            multisampling.minSampleShading = 1.0f;
            multisampling.pSampleMask = nullptr;
            multisampling.alphaToCoverageEnable = VK_FALSE;
            multisampling.alphaToOneEnable = VK_FALSE;

            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
            VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            colorBlendAttachment.blendEnable = VK_FALSE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

            VkPipelineColorBlendStateCreateInfo colorBlending{};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.logicOpEnable = VK_FALSE;
            colorBlending.logicOp = VK_LOGIC_OP_COPY;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &colorBlendAttachment;
            colorBlending.blendConstants[0] = 0.0f;
            colorBlending.blendConstants[1] = 0.0f;
            colorBlending.blendConstants[2] = 0.0f;
            colorBlending.blendConstants[3] = 0.0f;

            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
            depthStencil.depthBoundsTestEnable = VK_FALSE;
            depthStencil.minDepthBounds = 0.0f;
            depthStencil.maxDepthBounds = 1.0f;
            depthStencil.stencilTestEnable = VK_FALSE;
            depthStencil.front = {};
            depthStencil.back = {};

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
            pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipelineLayoutInfo.setLayoutCount = 1;
            pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout2;
            pipelineLayoutInfo.pushConstantRangeCount = 0;
            pipelineLayoutInfo.pPushConstantRanges = nullptr;

            if ( vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout2) != VK_SUCCESS ) {
                return false;
            }

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
            pipelineInfo.pStages = shaderStages.data();
            pipelineInfo.pVertexInputState = &vertexInputInfo;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.layout = pipelineLayout2;
            pipelineInfo.renderPass = renderPass;
            pipelineInfo.subpass = 0;
            pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
            pipelineInfo.basePipelineIndex = -1;

            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline2) != VK_SUCCESS) {
                return false;
            }
            vkDestroyShaderModule(device, fragShaderModule, nullptr);
            vkDestroyShaderModule(device, vertShaderModule, nullptr);
        }
        { // create graphics pipeline 3
            std::vector<char> vertShaderCode;
            if (!readFile("/Users/ivanamies/dev/gfx/Vulkan/vulkan-tutorial-dot-com/src/VulkanTesting/VulkanTesting/vert3.spv", vertShaderCode)) return false;
            std::vector<char> fragShaderCode;
            if (!readFile("/Users/ivanamies/dev/gfx/Vulkan/vulkan-tutorial-dot-com/src/VulkanTesting/VulkanTesting/frag3.spv", fragShaderCode)) return false;

            VkShaderModule vertShaderModule;
            if (!createShaderModule(vertShaderCode, vertShaderModule)) return false;
            VkShaderModule fragShaderModule;
            if (!createShaderModule(fragShaderCode, fragShaderModule)) return false;

            VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
            vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vertShaderStageInfo.module = vertShaderModule;
            vertShaderStageInfo.pName = "main";

            VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
            fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fragShaderStageInfo.module = fragShaderModule;
            fragShaderStageInfo.pName = "main";

            VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

            VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
            vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInputInfo.vertexBindingDescriptionCount = 0;
            vertexInputInfo.vertexAttributeDescriptionCount = 0;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            inputAssembly.primitiveRestartEnable = VK_FALSE;

            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.depthClampEnable = VK_FALSE;
            rasterizer.rasterizerDiscardEnable = VK_FALSE;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.lineWidth = 1.0f;
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
            rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
            rasterizer.depthBiasEnable = VK_FALSE;

            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.sampleShadingEnable = VK_FALSE;
            multisampling.rasterizationSamples = msaaSamples;
            multisampling.minSampleShading = 1.0f;
            multisampling.pSampleMask = nullptr;
            multisampling.alphaToCoverageEnable = VK_FALSE;
            multisampling.alphaToOneEnable = VK_FALSE;

            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            colorBlendAttachment.blendEnable = VK_FALSE;

            VkPipelineColorBlendStateCreateInfo colorBlending{};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.logicOpEnable = VK_FALSE;
            colorBlending.logicOp = VK_LOGIC_OP_COPY;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &colorBlendAttachment;
            colorBlending.blendConstants[0] = 0.0f;
            colorBlending.blendConstants[1] = 0.0f;
            colorBlending.blendConstants[2] = 0.0f;
            colorBlending.blendConstants[3] = 0.0f;

            VkPipelineDepthStencilStateCreateInfo depthStencil{};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
            depthStencil.depthBoundsTestEnable = VK_FALSE;
            depthStencil.minDepthBounds = 0.0f;
            depthStencil.maxDepthBounds = 1.0f;
            depthStencil.stencilTestEnable = VK_FALSE;
            depthStencil.front = {};
            depthStencil.back = {};

            std::vector<VkDynamicState> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR
            };
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamicState.pDynamicStates = dynamicStates.data();

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
            pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipelineLayoutInfo.setLayoutCount = 0;
            pipelineLayoutInfo.pushConstantRangeCount = 0;

            if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout3) != VK_SUCCESS) {
                return false;
            }

            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = 2;
            pipelineInfo.pStages = shaderStages;
            pipelineInfo.pVertexInputState = &vertexInputInfo;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.layout = pipelineLayout3;
            pipelineInfo.renderPass = renderPass;
            pipelineInfo.subpass = 0;
            pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline3) != VK_SUCCESS) {
                return false;
            }

            vkDestroyShaderModule(device, fragShaderModule, nullptr);
            vkDestroyShaderModule(device, vertShaderModule, nullptr);
        }
        return true;
    }

    bool createComputePipeline() {
        {
            std::vector<char> computeShaderCode;
            if (!readFile("/Users/ivanamies/dev/gfx/Vulkan/vulkan-tutorial-dot-com/src/VulkanTesting/VulkanTesting/comp.spv", computeShaderCode)) return false;
            VkShaderModule computeShaderModule;
            if (!createShaderModule(computeShaderCode, computeShaderModule)) return false;

            VkPipelineShaderStageCreateInfo computeShaderStageInfo{};
            computeShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            computeShaderStageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            computeShaderStageInfo.module = computeShaderModule;
            computeShaderStageInfo.pName = "main";

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
            pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipelineLayoutInfo.setLayoutCount = 1;
            pipelineLayoutInfo.pSetLayouts = &computeDescriptorSetLayout;

            if ( vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &computePipelineLayout) != VK_SUCCESS ) {
                return false;
            }

            VkComputePipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pipelineInfo.layout = computePipelineLayout;
            pipelineInfo.stage = computeShaderStageInfo;

            if ( vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &computePipeline) != VK_SUCCESS ) {
                return false;
            }

            vkDestroyShaderModule(device, computeShaderModule, nullptr);
        }
        return true;
    }

    bool createShaderModule(const std::vector<char>& code, VkShaderModule& out) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        if ( vkCreateShaderModule(device, &createInfo, nullptr, &out) != VK_SUCCESS) {
            return false;
        }
        return true;
    }

    bool createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());
        for ( size_t i = 0; i < swapChainImages.size(); ++i ) {
            if (!createImageView(swapChainImages[i], swapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT, 1, swapChainImageViews[i])) return false;
        }
        return true;
    }

    bool createSwapChain() {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice);
        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
        VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
        VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);
        uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
        if ( swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount ) {
            imageCount = swapChainSupport.capabilities.maxImageCount;
        }
        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if constexpr (vk_debug_has(vk_debug::kDumpSwapchain)) {
            createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }

        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
        uint32_t queueFamilyIndices[] = {indices.graphicsAndComputeFamily.value(),
            indices.presentFamily.value()};
        if ( indices.graphicsAndComputeFamily != indices.presentFamily ) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        }
        else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0;
            createInfo.pQueueFamilyIndices = nullptr;
        }

        createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;

        if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
            return false;
        }

        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr);
        swapChainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages.data());
        swapChainImageFormat = surfaceFormat.format;
        swapChainExtent = extent;
        return true;
    }

    bool createSurface() {
        if (!SDL_Vulkan_CreateSurface(window_, instance, nullptr, &surface)) {
            return false;
        }
        return true;
    }

    bool createLogicalDevice() {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsAndComputeFamily.value(), indices.presentFamily.value()};
        constexpr float queuePriority = 1;
        for ( uint32_t queueFamily : uniqueQueueFamilies ) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        VkPhysicalDeviceVulkan12Features vk12features{};
        vk12features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vk12features.runtimeDescriptorArray = VK_TRUE;
        vk12features.descriptorBindingPartiallyBound = VK_TRUE;
        vk12features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        vk12features.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
        vk12features.descriptorBindingVariableDescriptorCount = VK_TRUE;
        vk12features.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
        vk12features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
        vk12features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;

        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.features.samplerAnisotropy = VK_TRUE;
        features2.pNext = &vk12features;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = nullptr;
        createInfo.pNext = &features2;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();
        if ( enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
        }
        else {
            createInfo.enabledLayerCount = 0;
        }

        if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS ) {
            return false;
        }
        vkGetDeviceQueue(device, indices.graphicsAndComputeFamily.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
        vkGetDeviceQueue(device, indices.graphicsAndComputeFamily.value(), 0, &computeQueue);
        return true;
    }

    struct QueueFamilyIndices {
        std::optional<uint32_t> graphicsAndComputeFamily;
        std::optional<uint32_t> presentFamily;

        bool isComplete() {
            return graphicsAndComputeFamily.has_value() && presentFamily.has_value();
        }
    };

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) {
        QueueFamilyIndices indices;
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
        int i = 0;
        for ( const auto& queueFamily : queueFamilies ) {
            if ((queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) && (queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT) ) {
                indices.graphicsAndComputeFamily = i;
            }
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if ( presentSupport ) {
                indices.presentFamily = i;
            }
            if ( indices.isComplete() ) {
                break;
            }
            ++i;
        }
        return indices;
    }

    struct SwapChainSupportDetails {
        VkSurfaceCapabilitiesKHR capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) {
        SwapChainSupportDetails details;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
        if ( formatCount != 0 ) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
        }
        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
        if ( presentModeCount != 0 ) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
        }
        return details;
    }

    bool isDeviceSuitable(VkPhysicalDevice device) {
        QueueFamilyIndices indices = findQueueFamilies(device);
        bool swapChainAdequate = false;
        bool extensionsSupported = checkDeviceExtensionSupport(device);
        if ( extensionsSupported ) {
            SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
            swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
        }
        VkPhysicalDeviceFeatures supportedFeatures;
        vkGetPhysicalDeviceFeatures(device, &supportedFeatures);
        return indices.isComplete() && extensionsSupported && swapChainAdequate && supportedFeatures.samplerAnisotropy;
    }

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        for ( const auto& availableFormat : availableFormats ) {
            if ( availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR ) {
                return availableFormat;
            }
        }
        // hey man I just copy the tutorial, but this function clearly
        // breaks when availableFormats is empty
        return availableFormats[0];
    }

    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        for (const auto& availablePresentMode : availablePresentModes) {
            if ( availablePresentMode == VK_PRESENT_MODE_FIFO_KHR ) {
                return availablePresentMode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }
        else {
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(window_, &width, &height);
            VkExtent2D actualExtent = {
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height)
            };
            actualExtent.width = std::clamp(actualExtent.width,
                                            capabilities.minImageExtent.width,
                                            capabilities.maxImageExtent.width);
            actualExtent.height = std::clamp(actualExtent.height,
                                             capabilities.minImageExtent.height,
                                             capabilities.maxImageExtent.height);
            return actualExtent;
        }
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());
        std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
        for ( const auto& extension : availableExtensions ) {
            requiredExtensions.erase(extension.extensionName);
        }
        return requiredExtensions.empty();
    }

    VkSampleCountFlagBits getMaxUsableSampleCount() {
        VkPhysicalDeviceProperties physicalDeviceProperties;
        vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);
        VkSampleCountFlags counts = physicalDeviceProperties.limits.framebufferColorSampleCounts & physicalDeviceProperties.limits.framebufferDepthSampleCounts;

        if ( counts & VK_SAMPLE_COUNT_64_BIT ) { return VK_SAMPLE_COUNT_64_BIT; };
        if ( counts & VK_SAMPLE_COUNT_32_BIT ) { return VK_SAMPLE_COUNT_32_BIT; };
        if ( counts & VK_SAMPLE_COUNT_16_BIT ) { return VK_SAMPLE_COUNT_16_BIT; };
        if ( counts & VK_SAMPLE_COUNT_8_BIT ) { return VK_SAMPLE_COUNT_8_BIT; };
        if ( counts & VK_SAMPLE_COUNT_4_BIT ) { return VK_SAMPLE_COUNT_4_BIT; };
        if ( counts & VK_SAMPLE_COUNT_2_BIT ) { return VK_SAMPLE_COUNT_2_BIT; };

        return VK_SAMPLE_COUNT_1_BIT;
    }

    bool pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if ( deviceCount == 0 ) {
            return false;
        }
        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for ( const auto& device : devices ) {
            if (isDeviceSuitable(device)) {
                physicalDevice = device;
                msaaSamples = getMaxUsableSampleCount();
                break;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            return false;
        }
        return true;
    }

    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
        createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
    }

    bool setupDebugMessenger() {
        if (!enableValidationLayers) {
            return true;
        }
        VkDebugUtilsMessengerCreateInfoEXT createInfo{};
        populateDebugMessengerCreateInfo(createInfo);
        if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger)) {
            return false;
        }
        return true;
    }

    void dumpSwapchainToPng(uint32_t imageIndex, const char* path) {
        const uint32_t w = swapChainExtent.width;
        const uint32_t h = swapChainExtent.height;
        const VkDeviceSize bufSize = static_cast<VkDeviceSize>(w) * h * 4;

        VkBuffer buf;
        VkDeviceMemory bufMem;
        createBuffer(bufSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     buf, bufMem);

        VkCommandBuffer cmd = beginSingleTimeCommands();

        VkImageMemoryBarrier toSrc{};
        toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = swapChainImages[imageIndex];
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toSrc.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toSrc);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, swapChainImages[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

        VkImageMemoryBarrier toPresent = toSrc;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &toPresent);

        endSingleTimeCommands(cmd);

        void* mapped = nullptr;
        vkMapMemory(device, bufMem, 0, bufSize, 0, &mapped);
        const uint8_t* src = static_cast<const uint8_t*>(mapped);
        const bool bgra = (swapChainImageFormat == VK_FORMAT_B8G8R8A8_SRGB ||
                           swapChainImageFormat == VK_FORMAT_B8G8R8A8_UNORM);
        std::vector<uint8_t> rgba(static_cast<size_t>(bufSize));
        for (uint32_t i = 0; i < w * h; ++i) {
            if (bgra) {
                rgba[i * 4 + 0] = src[i * 4 + 2];
                rgba[i * 4 + 1] = src[i * 4 + 1];
                rgba[i * 4 + 2] = src[i * 4 + 0];
                rgba[i * 4 + 3] = src[i * 4 + 3];
            } else {
                rgba[i * 4 + 0] = src[i * 4 + 0];
                rgba[i * 4 + 1] = src[i * 4 + 1];
                rgba[i * 4 + 2] = src[i * 4 + 2];
                rgba[i * 4 + 3] = src[i * 4 + 3];
            }
        }
        vkUnmapMemory(device, bufMem);

        const int ok = stbi_write_png(path, static_cast<int>(w), static_cast<int>(h),
                                      4, rgba.data(), static_cast<int>(w * 4));
        std::cerr << "dumpSwapchainToPng -> " << path << " ok=" << ok
                  << " fmt=" << swapChainImageFormat << " " << w << "x" << h << std::endl;

        vkDestroyBuffer(device, buf, nullptr);
        vkFreeMemory(device, bufMem, nullptr);
    }

    bool drawFrame() {
        {
            vkWaitForFences(device, 1, &computeInFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
            updateComputeUniformBuffer(currentFrame);
            vkResetFences(device, 1, &computeInFlightFences[currentFrame]);
            vkResetCommandBuffer(computeCommandBuffers[currentFrame], /* VkCommandBufferResetFlagBits */ 0);
            recordComputeCommandBuffer(computeCommandBuffers[currentFrame]);

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &computeCommandBuffers[currentFrame];
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &computeFinishedSemaphores[currentFrame];

            if ( vkQueueSubmit(computeQueue, 1, &submitInfo, computeInFlightFences[currentFrame]) != VK_SUCCESS ) {
                return false;
            }

        }
        {
            vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);

            rm_.BeginFrame();

            {
                VkBuffer bump_buf = rm_.GetVkBumpMasterBuffer(rhi::Memory::kDynamic);
                std::array<VkWriteDescriptorSet, 3> writes{};
                std::array<VkDescriptorBufferInfo, 3> buf_infos{};

                buf_infos[0].buffer = bump_buf;
                buf_infos[0].offset = 0;
                buf_infos[0].range = sizeof(cairns::rhi::RenderPassGlobals);
                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = dynUboSets_[currentFrame];
                writes[0].dstBinding = 0;
                writes[0].dstArrayElement = 0;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                writes[0].descriptorCount = 1;
                writes[0].pBufferInfo = &buf_infos[0];

                buf_infos[1].buffer = bump_buf;
                buf_infos[1].offset = 0;
                buf_infos[1].range = sizeof(cairns::rhi::MaterialGpu);
                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = dynUboSets_[currentFrame];
                writes[1].dstBinding = 1;
                writes[1].dstArrayElement = 0;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                writes[1].descriptorCount = 1;
                writes[1].pBufferInfo = &buf_infos[1];

                buf_infos[2].buffer = bump_buf;
                buf_infos[2].offset = 0;
                buf_infos[2].range = sizeof(cairns::rhi::DrawTmp);
                writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[2].dstSet = dynUboSets_[currentFrame];
                writes[2].dstBinding = 2;
                writes[2].dstArrayElement = 0;
                writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                writes[2].descriptorCount = 1;
                writes[2].pBufferInfo = &buf_infos[2];

                vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            }

            if (!BuildMeshOpaqueDraws()) {
                return false;
            }
            std::sort(drawListSorted_.begin(), drawListSorted_.end());

            uint32_t imageIndex;
            VkResult result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);

            if ( result == VK_ERROR_OUT_OF_DATE_KHR ) {
                recreateSwapChain();
                return true;
            }
            else if ( result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR ) {
                return false;
            }

            updateUniformBuffer(currentFrame);

            vkResetFences(device, 1, &inFlightFences[currentFrame]);

            {
                vkResetCommandBuffer(commandBuffers[currentFrame], 0);
                recordCommandBuffer(commandBuffers[currentFrame], imageIndex);
            }

            VkSubmitInfo submitInfo{};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

            std::array<VkSemaphore,2> waitSemaphores = {computeFinishedSemaphores[currentFrame], imageAvailableSemaphores[currentFrame]};
            VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
            submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
            submitInfo.pWaitSemaphores = waitSemaphores.data();
            submitInfo.pWaitDstStageMask = waitStages;

            std::vector<VkCommandBuffer> drawCommandBuffers;
            {
                drawCommandBuffers.push_back(commandBuffers[currentFrame]);
            }
            submitInfo.commandBufferCount = static_cast<uint32_t>(drawCommandBuffers.size());
            submitInfo.pCommandBuffers = drawCommandBuffers.data();

            std::array<VkSemaphore,1> signalSemaphores = {renderFinishedSemaphores[currentFrame]};
            submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
            submitInfo.pSignalSemaphores = signalSemaphores.data();

            if ( vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS ) {
                return false;
            }

            VkPresentInfoKHR presentInfo{};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
            presentInfo.pWaitSemaphores = signalSemaphores.data();

            VkSwapchainKHR swapChains[] = {swapChain};
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = swapChains;

            presentInfo.pImageIndices = &imageIndex;
            presentInfo.pResults = nullptr;

            result = vkQueuePresentKHR(presentQueue, &presentInfo);

            if ( result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
                framebufferResized = false;
                recreateSwapChain();
            }
            else if ( result != VK_SUCCESS ) {
                return false;
            }

            if constexpr (vk_debug_has(vk_debug::kDumpSwapchain)) {
                if ( dumpFrameCounter == 60 ) {
                    vkQueueWaitIdle(presentQueue);
                    dumpSwapchainToPng(imageIndex, "/tmp/tut_dump.png");
                }
                ++dumpFrameCounter;
            }
        }

        currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
        return true;
    }

    uint32_t dumpFrameCounter = 0;

    void updateUniformBuffer(uint32_t currentImage) {
        static auto startTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

        UniformBufferObject ubo{};
        // glTF is Y-up; the tutorial's camera is Z-up. Convert Y-up -> Z-up, then spin about world Z.
        glm::mat4 yUpToZUp = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        ubo.model = glm::rotate(glm::mat4(1.0f), time*glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f)) * yUpToZUp;
        ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        ubo.proj = glm::perspective(glm::radians(45.0f), swapChainExtent.width / (float)swapChainExtent.height, 0.1f, 10.0f);
        ubo.proj[1][1] *= -1;
        memcpy(uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
    }

    void updateComputeUniformBuffer(uint32_t currentImage) {
        static auto previousTime = std::chrono::high_resolution_clock::now();
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - previousTime).count();
        previousTime = currentTime;

        ParameterUBO ubo{};
        ubo.deltaTime = deltaTime;
        memcpy(computeUniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
    }

    void cleanupSwapChain() {
        vkDestroyImageView(device, colorImageView, nullptr);
        vkDestroyImage(device, colorImage, nullptr);
        vkFreeMemory(device, colorImageMemory, nullptr);
        vkDestroyImageView(device, depthImageView, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        vkFreeMemory(device, depthImageMemory, nullptr);
        for ( size_t i = 0; i < swapChainFramebuffers.size(); ++i ) {
            vkDestroyFramebuffer(device, swapChainFramebuffers[i], nullptr);
        }
        for ( size_t i = 0; i < swapChainImageViews.size(); ++i ) {
            vkDestroyImageView(device, swapChainImageViews[i], nullptr);
        }
        vkDestroySwapchainKHR(device, swapChain, nullptr);
    }

    void cleanup() {
        // do not clean up resources that are still being used
        vkDeviceWaitIdle(device);

        cleanupSwapChain();

        vkDestroyDescriptorPool(device, bindlessPool_, nullptr);
        vkDestroyDescriptorSetLayout(device, bindlessLayout_, nullptr);

        vkDestroyDescriptorPool(device, descriptorPool, nullptr);

        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout2, nullptr);
        vkDestroyDescriptorSetLayout(device, computeDescriptorSetLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, dynamicUboLayout_, nullptr);

        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);

        vkDestroyPipeline(device, graphicsPipeline2, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout2, nullptr);

        vkDestroyPipeline(device, graphicsPipeline3, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout3, nullptr);

        vkDestroyPipeline(device, computePipeline, nullptr);
        vkDestroyPipelineLayout(device, computePipelineLayout, nullptr);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
            vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
            vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
            vkDestroyFence(device, inFlightFences[i], nullptr);
        }

        for ( size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i ) {
            vkDestroySemaphore(device, computeFinishedSemaphores[i], nullptr);
            vkDestroyFence(device, computeInFlightFences[i], nullptr);
        }
        rm_.Deinit();

        vkDestroyCommandPool(device, commandPool, nullptr);

        vkDestroyDevice(device, nullptr);

        if (enableValidationLayers) {
            DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        }

        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    bool createInstance() {
        if (enableValidationLayers && !checkValidationLayerSupport()) {
            enableValidationLayers = false;
        }
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Hello Triangle";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        auto extensions = getRequiredExtensions();

        { // for getting Vulkan to work on macosx
            extensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

            extensions.emplace_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }

        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();

            populateDebugMessengerCreateInfo(debugCreateInfo);
            createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*) &debugCreateInfo;
        }
        else {
            createInfo.enabledLayerCount = 0;
            createInfo.pNext = nullptr;
        }

        VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
        if ( result != VK_SUCCESS ) {
            return false;
        }
        return true;
    }

    bool checkValidationLayerSupport() {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        for ( const char* layerName : validationLayers ) {
            bool layerFound = false;
            for ( const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    layerFound = true;
                    break;
                }
            }
            if ( !layerFound ) {
                return false;
            }
        }

        return true;
    }

    std::vector<const char*> getRequiredExtensions() {
        uint32_t sdlExtensionCount = 0;
        const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
        std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);
        if ( enableValidationLayers ) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        return extensions;
    }

    const uint32_t WIDTH = 800;
    const uint32_t HEIGHT = 600;

    static constexpr uint32_t kHotArenaMemorySize = 1 << 29;
    void* hot_arena_mem_;
    cairns::Arena hot_arena_;
    std::vector<cairns::Scene, cairns::Allocator<cairns::Scene>> scenes_;
    std::vector<cairns::LoadedMaterial> materials_;
    std::vector<glm::mat4> debugSceneXforms_;
    std::vector<float> scene_norm_scales_;
    std::unordered_map<uint32_t, uint32_t> texture_id_map_;
    std::unordered_map<uint32_t, uint32_t> mesh_attr_id_map_;
    std::unordered_map<uint32_t, uint32_t> sampler_id_map_;

    const std::string MODEL_PATH = "/Users/ivanamies/dev/gfx/assets/debug/viking_room.glb";
    const std::string TEXTURE_PATH = "/Users/ivanamies/dev/gfx/Vulkan/vulkan-tutorial-dot-com/src/VulkanTesting/VulkanTesting/viking_room.png";

    const std::vector<const char*> validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };

    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        // validation layer will complain without this.
        // fixed by following https://www.reddit.com/r/vulkan/comments/17ecxkg/beginner_having_trouble_enabling_vk_khr/
        VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    };

#ifdef NDEBUG
    bool enableValidationLayers = false;
#else
    bool enableValidationLayers = true;
#endif // NDEBUG

    SDL_Window* window_ = nullptr;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device;
    VkQueue graphicsQueue;
    VkQueue presentQueue;
    VkQueue computeQueue;

    VkSwapchainKHR swapChain;
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    std::vector<VkImageView> swapChainImageViews;

    VkRenderPass renderPass;
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;

    VkDescriptorSetLayout descriptorSetLayout2;
    VkPipelineLayout pipelineLayout2;
    VkPipeline graphicsPipeline2;

    VkPipelineLayout pipelineLayout3;
    VkPipeline graphicsPipeline3;

    VkDescriptorSetLayout computeDescriptorSetLayout;
    VkPipelineLayout computePipelineLayout;
    VkPipeline computePipeline;

    std::vector<VkFramebuffer> swapChainFramebuffers;

    VkCommandPool commandPool;
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<VkCommandBuffer> commandBuffers2;
    std::vector<VkCommandBuffer> computeCommandBuffers;

    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;

    std::vector<VkFence> computeInFlightFences;
    std::vector<VkSemaphore> computeFinishedSemaphores;

    bool framebufferResized = false;
    uint32_t currentFrame = 0;

    rhi::ResourceManager rm_;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    rhi::Handle<rhi::Buffer> vertex_buffer_;
    rhi::Handle<rhi::Buffer> index_buffer_;

    std::array<rhi::Handle<rhi::Buffer>, MAX_FRAMES_IN_FLIGHT> uniform_buffers_;
    std::array<void*, MAX_FRAMES_IN_FLIGHT> uniformBuffersMapped{};

    std::array<rhi::Handle<rhi::Buffer>, MAX_FRAMES_IN_FLIGHT> compute_uniform_buffers_;
    std::array<void*, MAX_FRAMES_IN_FLIGHT> computeUniformBuffersMapped{};

    std::array<rhi::Handle<rhi::Buffer>, MAX_FRAMES_IN_FLIGHT> ssbo_;

    VkDescriptorSetLayout bindlessLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool bindlessPool_ = VK_NULL_HANDLE;
    VkDescriptorSet bindlessSet_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dynamicUboLayout_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> dynUboSets_;

    uint32_t ubo_align_ = 256;
    uint32_t globals_offset_ = 0;
    std::vector<cairns::Draw> drawList_;
    std::vector<std::pair<cairns::DrawKey, uint32_t>> drawListSorted_;
    std::vector<uint32_t> draw_material_offsets_;
    std::vector<uint32_t> draw_drawtmp_offsets_;

    VkDescriptorPool descriptorPool;
    std::vector<VkDescriptorSet> descriptorSets;
    std::vector<VkDescriptorSet> descriptorSets2;
    std::vector<VkDescriptorSet> computeDescriptorSets;

    uint32_t mipLevels = 0;
    rhi::Handle<rhi::Texture> texture_;
    rhi::Handle<rhi::Sampler> sampler_;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;

    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    VkImage colorImage;
    VkDeviceMemory colorImageMemory;
    VkImageView colorImageView;
};

} // namespace cairns

#pragma clang diagnostic pop

#endif // CAIRNS_VULKAN
