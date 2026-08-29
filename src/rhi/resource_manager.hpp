// rhi/resource_manager.hpp
//
// The resource vocabulary: Handle<T>, the generational ResourceManager<T> pool
// template (Aaltonen "arrays you walk"), the resource types (Buffer/Texture/...)
// with their Hot/Cold SoA split, and the *Desc creation structs. The cooperating
// subsystems (Device/Allocator/Resources/Frames/Pipelines) live in their
// own headers; this one is what they all share.
//
// Three rules that drive every decision:
//   1. Handles, not pointers. Every resource is referenced by a 32-bit
//      Handle<T> { uint16 index; uint16 generation; }. Pools own the data.
//   2. Hot/Cold SoA. Each resource type has a Hot struct (touched every draw)
//      and a Cold struct (auxiliary). Pools store them in two separate dense
//      arrays at the same index.
//   3. One platform buffer per heap block. A 128 MB heap block is exactly one
//      VkBuffer / MTLBuffer / GPUBuffer. Sub-allocations are offsets, not
//      separate platform objects.

#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
#include <span>
#include <filesystem>
#include <initializer_list>
#include <vector>

#include "core/handle.hpp"
#include "rhi/barrier.hpp"  // PipelineEvent (Granite per-resource sync state)
#include "util/define.hpp"
#include "util/offset_allocator.hpp"

#if CAIRNS_METAL
#include "rhi/metal/resource_manager_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/resource_manager_plat.hpp"
#elif CAIRNS_WEBGPU
#include "rhi/webgpu/resource_manager_plat.hpp"
#endif

struct SDL_Window;

namespace cairns::rhi {

class Device;
class Allocator;
class Resources;
class Bindless;
class Frames;
struct Buffer;
struct Texture;
struct Sampler;
struct BindGroup;
struct DynamicBuffers;
struct Shader;
struct Kernel;
struct SwapChain;
struct FrameContext;

// Handle<T> / ResourceManager<T> live in cairns::core (core/handle.hpp);
// re-exported so rhi code and non-GPU persistent pools (scenes, assets)
// share one pool substrate.
template <typename T> using Handle = ::cairns::Handle<T>;
template <typename T> using ResourceManager = ::cairns::ResourceManager<T>;

enum class Memory : uint8_t {
    kDefault,    // device-local, GPU-only
    kUpload,     // CPU writes, GPU reads (write-combined)
    kReadback,   // GPU writes, CPU reads (cached)
    kDynamic,    // host-write per frame, GPU-read (bump rings)
    kTransient,  // tile / lazily allocated render targets
    kCount,
};

enum BufferUsage : uint32_t {
    kUsageNone = 0,
    kUsageVertex = 1 << 0,
    kUsageIndex = 1 << 1,
    kUsageUniform = 1 << 2,
    kUsageStorage = 1 << 3,
    kUsageIndirect = 1 << 4,
    kUsageTransferSrc = 1 << 5,
    kUsageTransferDst = 1 << 6,
};

inline constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) |
                                    static_cast<uint32_t>(b));
}

enum TextureUsage : uint32_t {
    kTexUsageNone = 0,
    kTexUsageSampled = 1 << 0,
    kTexUsageStorage = 1 << 1,
    kTexUsageColorTarget = 1 << 2,
    kTexUsageDepthTarget = 1 << 3,
    kTexUsageTransferSrc = 1 << 4,
    kTexUsageTransferDst = 1 << 5,
};

inline constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) |
                                     static_cast<uint32_t>(b));
}

enum class Format : uint16_t {
    kUndefined = 0,
    kR8Unorm,
    kRg8Unorm,
    kRgba8Unorm,
    kRgba8Srgb,
    kBgra8Unorm,
    kBgra8Srgb,
    kR16F,
    kRgba16F,
    kR32F,
    kRgba32F,
    kD32F,
    kD24S8,
    kBc7Rgba,
    kAstc4x4,
    kRg32F,
    // Picking ID buffer attachment: R32U packed {type<<24 | id}. Distinct
    // from kR32F because uint sampling + integer-output fragments need
    // integer formats end-to-end.
    kR32Uint,
};

enum ShaderStage : uint32_t {
    kStageNone = 0,
    kStageVertex = 1 << 0,
    kStageFragment = 1 << 1,
    kStageCompute = 1 << 2,
    kStageAll = kStageVertex | kStageFragment | kStageCompute,
};

inline constexpr ShaderStage operator|(ShaderStage a, ShaderStage b) {
    return static_cast<ShaderStage>(static_cast<uint32_t>(a) |
                                    static_cast<uint32_t>(b));
}

enum class BufferKind : uint8_t { kUniform, kStorage };

enum class Filter : uint8_t { kNearest, kLinear };

enum class AddressMode : uint8_t {
    kRepeat,
    kMirroredRepeat,
    kClampToEdge,
    kClampToBorder,
};

struct Vector3I {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

struct BufferDesc {
    const char* debug_name = nullptr;
    uint32_t byte_size = 0;
    BufferUsage usage = kUsageUniform;
    Memory memory = Memory::kDefault;
    std::span<const uint8_t> initial_data;
};

struct TextureDesc {
    const char* debug_name = nullptr;
    Vector3I dimensions = {1, 1, 1};
    uint32_t mip_levels = 1;
    uint32_t array_layers = 1;
    uint32_t sample_count = 1;
    Format format = Format::kRgba8Srgb;
    TextureUsage usage = kTexUsageSampled;
    Memory memory = Memory::kDefault;
    std::span<const uint8_t> initial_data;
};

struct SamplerDesc {
    const char* debug_name = nullptr;
    Filter mag_filter = Filter::kLinear;
    Filter min_filter = Filter::kLinear;
    Filter mip_filter = Filter::kLinear;
    AddressMode address_mode = AddressMode::kRepeat;
    float max_anisotropy = 0.0f;  // 0 => anisotropy disabled
    float max_lod = 0.0f;
};

struct TextureBinding {
    uint32_t slot = 0;
    Handle<Texture> texture;
    ShaderStage stages = kStageAll;
};

struct BufferBinding {
    uint32_t slot = 0;
    Handle<Buffer> buffer;
    uint32_t offset = 0;
    uint32_t range = 0;  // 0 = whole buffer
    BufferKind kind = BufferKind::kUniform;
    ShaderStage stages = kStageAll;
};

struct SamplerBinding {
    uint32_t slot = 0;
    Handle<Sampler> sampler;
    ShaderStage stages = kStageAll;
};

struct BindGroupDesc {
    const char* debug_name = nullptr;
    std::span<const TextureBinding> textures;
    std::span<const BufferBinding> buffers;
    std::span<const SamplerBinding> samplers;
};

struct DynamicBinding {
    uint32_t slot = 0;
    BufferKind kind = BufferKind::kUniform;
    uint32_t max_range = 0;  // bytes; needed for validation
    ShaderStage stages = kStageAll;
    // Backing buffer. Null = kDynamic bump master (Aaltonen slot 4 with
    // per-draw dynamic offsets). Non-null = persistent SSBO (e.g. skin
    // palettes / scene tables / particle parity buffer).
    Handle<Buffer> backing;
    // False: regular UBO/SSBO descriptor (no dynamic offset), written over
    // the whole backing buffer (range = max_range or VK_WHOLE_SIZE).
    // True: UBO_DYNAMIC/SSBO_DYNAMIC; caller supplies the per-dispatch
    // byte offset at bind time.
    bool has_dynamic_offset = true;
};

struct DynamicBuffersDesc {
    const char* debug_name = nullptr;
    std::span<const DynamicBinding> bindings;
};

// Each resource type defines Hot (read every draw) and Cold (touched only on
// create/update/destroy). ResourceManager<T> stores them in two separate dense arrays.

struct Buffer {
    struct Hot {
        uint16_t heap_buffer_index = 0;  // which 128 MB master buffer
        uint16_t pad = 0;
        uint32_t offset_in_heap = 0;     // sub-allocation offset
    };
    struct Cold {
        OffsetAllocator::Allocation alloc;
        uint32_t size_bytes = 0;
        BufferUsage usage = kUsageNone;
        Memory mem_type = Memory::kDefault;
        const char* debug_name = nullptr;
        PipelineEvent sync;  // Granite per-resource barrier state (persists)
    };
};

struct Texture {
    struct Hot {
        ApiTextureHandle api_view = nullptr;  // VkImageView / MTLTexture / WGPUTextureView
    };
    struct Cold {
        OffsetAllocator::Allocation alloc;
        ApiTextureHandle api_image = nullptr;  // VkImage / MTLTexture root / WGPUTexture
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
        uint32_t mip_levels = 0;
        uint32_t array_layers = 0;
        Format format = Format::kUndefined;
        TextureUsage usage = kTexUsageNone;
        Memory mem_type = Memory::kDefault;
        uint32_t heap_buffer_index = 0xFFFFFFFFu;  // 0xFFFFFFFF if dedicated
        const char* debug_name = nullptr;
        PipelineEvent sync;  // Granite per-resource barrier state (persists)
        TextureColdPlat plat;
    };
};

struct Sampler {
    struct Hot {
        ApiSamplerHandle api_sampler = nullptr;  // VkSampler / MTLSamplerState / WGPUSampler
    };
    struct Cold {
        const char* debug_name = nullptr;
    };
};

struct BindGroup {
    struct Hot {
        ApiArgBufferHandle api_descriptor_set = nullptr;  // VkDescriptorSet / MTLArgumentBuffer / WGPUBindGroup
        uint32_t arg_buf_offset = 0;         // byte offset into api_descriptor_set (Metal only)
    };
    struct Cold {
        const char* debug_name = nullptr;
    };
};

// DynamicBuffers is NOT a resource — it is a bind group TYPE that holds
// dynamically-offset buffer bindings. The buffer itself is always the bump
// ring's master buffer; only the per-draw offsets change.
struct DynamicBuffers {
    struct Hot {
        void* api_descriptor_set = nullptr;  // unused; state lives in plat
        uint8_t binding_count = 0;
        // Per-backend state (vk: layout + per-FIF sets).
        DynamicBuffersHotPlat plat;
    };
    struct Cold {
        std::vector<DynamicBinding> layout;
        const char* debug_name = nullptr;
    };
};

struct Shader {
    struct Hot {
        ApiPsoHandle api_pso = nullptr;  // VkPipeline / MTLRenderPipelineState
        ShaderHotPlat plat;
    };
    struct Cold {
        const char* debug_name = nullptr;
    };
};

struct Kernel {
    struct Hot {
        ApiKernelHandle api_pso = nullptr;  // MTLComputePipelineState
        KernelHotPlat plat;
    };
    struct Cold {
        const char* debug_name = nullptr;
    };
};

// --- Pipeline creation -----------------------------------------------------
// Portable description of a graphics/compute pipeline. The backend resolves
// logical_shader to its own shader file(s) + entry point(s) (e.g. "unlit" ->
// unlit.metal / unlit.{vert,frag}.spv). Some fields are consumed only by the
// backend that needs them (Metal applies cull/winding/depth-compare at encode;
// Vulkan takes color_format from the render pass). Backend-owned handles not
// yet abstracted by the rhi (Vulkan render pass + descriptor set layouts) are
// passed in under #if for now.

enum class PrimitiveTopology : uint8_t { kTriangleList, kPointList };
enum class CullMode : uint8_t { kNone, kBack, kFront };
enum class FrontFace : uint8_t { kCounterClockwise, kClockwise };
enum class CompareOp : uint8_t {
    kNever, kLess, kEqual, kLessEqual, kGreater, kNotEqual, kGreaterEqual, kAlways,
};
enum class BlendFactor : uint8_t { kZero, kOne, kSrcAlpha, kOneMinusSrcAlpha };

struct VertexInputAttribute {
    uint32_t location = 0;     // vk location / metal attribute index
    uint32_t buffer_slot = 0;  // vk binding / metal buffer index
    Format format = Format::kRgba32F;
    uint32_t offset = 0;
};

struct VertexBufferLayout {
    uint32_t buffer_slot = 0;
    uint32_t stride = 0;
};

struct BlendState {
    bool enable = false;
    BlendFactor src_color = BlendFactor::kOne;
    BlendFactor dst_color = BlendFactor::kZero;
    BlendFactor src_alpha = BlendFactor::kOne;
    BlendFactor dst_alpha = BlendFactor::kZero;
};

struct GraphicsPipelineDesc {
    const char* logical_shader = nullptr;  // "unlit" / "particle"
    const char* shader_dir = nullptr;      // base dir for shader files
    // Optional DynamicBuffers handles for set 0 (pass globals) + set 2
    // (per-draw drawtmp). When non-null, pipelines resolves the descriptor
    // set layout from these; null = the Pipelines-owned globals_/drawtmp_
    // layouts (still required for non-unlit pipelines).
    Handle<DynamicBuffers> dyn_set_0;
    Handle<DynamicBuffers> dyn_set_2;
    std::span<const VertexInputAttribute> vertex_attributes;
    std::span<const VertexBufferLayout> vertex_buffers;
    PrimitiveTopology topology = PrimitiveTopology::kTriangleList;
    CullMode cull = CullMode::kNone;
    FrontFace front_face = FrontFace::kCounterClockwise;
    bool depth_test = true;
    bool depth_write = true;
    CompareOp depth_compare = CompareOp::kLess;
    BlendState blend;
    // Multi-color attachments. color_count == 0 means "single attachment,
    // use color_format below" (shorthand). When color_count > 0,
    // color_formats[0..count) drives the renderpass / pipeline and
    // color_format is ignored. kMaxColorFormats matches the RHI ceiling on
    // simultaneous color attachments.
    static constexpr uint8_t kMaxColorFormats = 4;
    Format color_formats[kMaxColorFormats] = {Format::kUndefined,
                                                Format::kUndefined,
                                                Format::kUndefined,
                                                Format::kUndefined};
    uint8_t color_count = 0;
    // When the fragment shader writes fewer outputs than color_count
    // (e.g. particle frag writes 1, but the offscreen PSO has 2
    // attachments to match the id-MRT renderpass), the pipeline sets
    // colorWriteMask=0 on attachments [frag_color_output_count,
    // color_count). 0 means "auto: match color_count".
    uint8_t frag_color_output_count = 0;
    Format color_format = Format::kBgra8Unorm;
    Format depth_format = Format::kD32F;
    uint32_t sample_count = 1;
    uint32_t push_constant_bytes = 0;
    const char* debug_name = nullptr;
    // Neutral: the backend reads its render pass / target info from the swap
    // chain; descriptor set layouts are resolved rhi-side from logical_shader.
    SwapChain* swap_chain = nullptr;
};

// Descriptor-layout discriminator. Particle is a single set (dynUBO +
// 2 SSBO); skinning needs two (Group B frame-global + Group A per-mesh
// slices). Lets CreateComputePipeline pick the right layouts without
// conditional code in shader resolution.
enum class ComputePipelineLayout : uint8_t {
    kParticle = 0,
    kSkin = 1,
    kAnimEval = 2,
};

struct ComputePipelineDesc {
    const char* logical_shader = nullptr;  // "particle" or "skin"
    const char* shader_dir = nullptr;
    const char* debug_name = nullptr;
    ComputePipelineLayout layout = ComputePipelineLayout::kParticle;
    // Optional DynamicBuffers handle for set 0. When non-null, pipelines
    // resolves the descriptor set layout from GetHot(dyn_set_0)->plat.
    // Required for kSkin (Group B), kAnimEval, and kParticle.
    Handle<DynamicBuffers> dyn_set_0;
};

// --- Bindless registry -----------------------------------------------------
// One descriptor table (Vulkan) / argument buffer (Metal) holding three arrays:
// sampled textures, attribute storage buffers, and samplers. Build it by
// CreateBindlessRegistry, then BindlessAdd* (each returns the dense per-array
// slot index), then BindlessFinalize. `*_slot` is the Metal argument-index base
// for that array; on Vulkan it is the descriptor binding number.
struct BindlessRegistryDesc {
    uint32_t max_textures = 0;
    uint32_t max_attr_buffers = 0;
    uint32_t max_samplers = 0;
    uint32_t texture_slot = 0;
    uint32_t attr_buffer_slot = 0;
    uint32_t sampler_slot = 0;
    const char* debug_name = nullptr;
};

// BackendInitParams lives in the per-backend resource_manager_plat header.

// rhi-wide config constants.
// kFramesInFlight is the one home for FIF: bump rings, CPU arenas,
// descriptor pools, and sync vectors all scale off it. Each
// kDynamic/kUpload/kReadback ring slot is allocated per frame in flight,
// so raising FIF is a host-visible memory bill (see PERFORMANCE.md
// triple-buffer ledger).
inline constexpr uint32_t kFramesInFlight = 2;
inline constexpr uint32_t kHeapBlockBytes = 128u * 1024u * 1024u;
inline constexpr uint32_t kLargeThreshold = 64u * 1024u * 1024u;

}  // namespace cairns::rhi
