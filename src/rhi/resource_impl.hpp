#pragma once

#include "rhi/gfx_api.hpp"
#include "rhi/resource.hpp"
#include "rhi/tag.hpp"
#include "rhi/sampler.hpp"
#include "rhi/gpu_scene_registry.hpp"

#include "util/offset_allocator.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>

namespace cairns::rhi {

template<>
struct ResourceObject<Texture> {
    MTL::Texture* texture = nullptr;
};

template<>
struct ResourceDescriptor<Texture> {
    std::string undecorated_filename;
    int64_t width = 0;
    int64_t height = 0;
    int64_t channels = 0;
    int64_t depth = 0;
    int64_t src_bytes_per_row = 0;
    int64_t format = 0; // MTL::PixelFormat on Metal
    int64_t type = 0; // MTL::TextureType on Metal
    int64_t sample_count = 0; // for MSAA
    int64_t usage = 0; // MTL::TextureUsage on Metal
    int64_t levels = 1; // mipmap levels
    int64_t storage = 0; // MTL::StorageMode on Metal
    unsigned char* src_image = nullptr;
    
    int64_t GetBytes() const noexcept {
        return src_bytes_per_row * height;
    }
};

template<>
struct ResourceObject<Buffer> {
    // this comes from the allocator which alloc'd it.
    // DO NOT FREE.
    MTL::Buffer* buffer = nullptr;
    OffsetAllocator::Allocation mem = OffsetAllocator::BadAllocation;
};

template<>
struct ResourceDescriptor<Buffer> {
    enum class Usage : uint32_t {
        kUnknown = 0,
        kPosition = 1,
        kOtherAttr = 2,
        kIndex = 3,
        kSsbo = 4,
    };
    int64_t src_size_bytes = 0;
    int64_t aligned_size_bytes = 0;
    
    int64_t num_indices = 0;
    int64_t num_vertices = 0;
    
    int64_t stride = 0;
    int64_t format = 0;
    
    Usage usage;

    int64_t GetBytes() const noexcept {
        return aligned_size_bytes;
    }
};

struct RenderPassGlobals {
    glm::mat4 view_proj;
    glm::mat4 inv_view_proj; // get world-pos form depth for like raycasting
    glm::vec4 camera_pos; // [x, y, z, exposure];
    glm::vec4 camera_dir; // [x, y, z, near plane];
    glm::vec4 screen_params; // [width, height, 1/width, 1/height];
    // other things
    // uint32_t shadow_map
    // glm::vec3 sunlight_dir;
    // glm::vec3 sunlight_color;
};

enum class ShaderSpecificType {
    kNone = 0,
    kParticles = 1,
    kAnimation = 2
};

struct Ssbos {
    ShaderSpecificType type;
    // decayed Handle<Buffer> for ssbo indexing of particles or animation
    uint32_t wip = std::numeric_limits<uint32_t>::max();
};

template<>
struct ResourceObject<BindGroup> {
    // slide 19/ pg 31 on Hypehype's weakly typed layouts
    // the idea is most like that they want to compact CPU bandwidth as small as possible.
    // if you statically typed this you, you would multiply your draw commands and draw lists.
    // so they choose if-branches or the templates.
    BindGroup::Type type;
    // globals
    Handle<Buffer> globals;
    // materials
    Handle<Buffer> material_buffer;
    Handle<Material> material;
    // shader specific
    uint32_t ssbo_type = std::numeric_limits<uint32_t>::max(); // particles or animation
    Handle<Buffer> ssbo;
};

template<>
struct ResourceDescriptor<BindGroup> {
    
};

// for temporary allocated bump data only
template<>
struct ResourceObject<DynamicBuffers> {
    DynamicBuffers::Type type;
    Handle<Buffer> buf = Handle<Buffer>::Null;
};

template<>
struct ResourceDescriptor<DynamicBuffers> {
    
};

template<>
struct ResourceObject<Sampler> {
    MTL::SamplerState* sampler_state = nullptr;
};

template<>
struct ResourceDescriptor<Sampler> {
    SamplerFilter minFilter = SamplerFilter::Linear;
    SamplerFilter magFilter = SamplerFilter::Linear;
    SamplerMipFilter mipFilter = SamplerMipFilter::None;
    SamplerAddressMode addressModeU = SamplerAddressMode::Repeat;
    SamplerAddressMode addressModeV = SamplerAddressMode::Repeat;
};

template<>
struct ResourceObject<Shader> {
    MTL::RenderPipelineState* pso = nullptr;
    cairns::rhi::GpuSceneRegistry gpu_scene_registry;
    // points to gpu_scene_registry in gpu memory
    Handle<Buffer> scene_registry_handle = Handle<Buffer>::Null;
};

template<>
struct ResourceDescriptor<Shader> {
    // this is somewhat surprising to me, I would have split PSO and shaders proper but :shrug:
    MTL::Library* metal_default_library = nullptr;
    // as an external constraint (I have to redact why), we can only have frustrum culling in this engine. So the engine uses a vertex descriptor for dedicated hardware culling of glm::vec4s instead of the SSBO-like bindless registry.
    static constexpr uint32_t kMeshPosBindSlot = 0;
};

template<>
struct ResourceObject<RenderPass> {
    
};

template<>
struct ResourceDescriptor<RenderPass> {
    MTL::RenderPassDescriptor* render_pass_descriptor = nullptr;
};

template<>
struct ResourceObject<Material> {
    // we only do unlit for now
    Handle<Texture> color;
    Handle<Sampler> sampler;
    // metallicRoughness...
    // emission...
    // specular...
    // glossiness...
};

template<>
struct ResourceDescriptor<Material> {
    
};

} // namespace cairns::rhi
