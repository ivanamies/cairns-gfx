// rhi/resource_manager.hpp
//
// Public API for the cross-platform GPU resource manager.
// Implementations live in {metal,vulkan,webgpu}/resource_manager.cpp.
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
#include <initializer_list>
#include <vector>

#include "util/define.hpp"
#include "util/offset_allocator.hpp"

#if CAIRNS_VULKAN
#include <vulkan/vulkan.h>
#endif  // CAIRNS_VULKAN

#if CAIRNS_METAL
namespace MTL {
class Device;
class CommandQueue;
class Buffer;
class Heap;
class Texture;
class SamplerState;
class RenderPipelineState;
class ComputePipelineState;
}  // namespace MTL
#endif  // CAIRNS_METAL

namespace cairns::rhi {

#if CAIRNS_METAL
using ApiTextureHandle = MTL::Texture*;
using ApiSamplerHandle = MTL::SamplerState*;
using ApiPsoHandle = MTL::RenderPipelineState*;
using ApiArgBufferHandle = MTL::Buffer*;
using ApiKernelHandle = MTL::ComputePipelineState*;
#else
using ApiTextureHandle = void*;
using ApiSamplerHandle = void*;
using ApiPsoHandle = void*;
using ApiArgBufferHandle = void*;
using ApiKernelHandle = void*;
#endif

class ResourceManager;
struct Buffer;
struct Texture;
struct Sampler;
struct BindGroup;
struct DynamicBuffers;
struct Shader;
struct Kernel;

template <typename T>
struct Handle {
    uint16_t index = 0xFFFF;
    uint16_t generation = 0xFFFF;

    constexpr bool IsNull() const { return index == 0xFFFF; }
    constexpr auto operator<=>(const Handle&) const = default;

    static const Handle Null;
};

template <typename T>
const Handle<T> Handle<T>::Null = Handle<T>{};

// Generational typed pool with SoA hot/cold storage. T must define T::Hot and
// T::Cold nested types.
template <typename T>
class Pool {
public:
    Handle<T> Acquire() {
        uint16_t idx;
        if (!freelist_.empty()) {
            idx = freelist_.back();
            freelist_.pop_back();
        } else {
            idx = static_cast<uint16_t>(hot_.size());
            hot_.emplace_back();
            cold_.emplace_back();
            generation_.push_back(1);
        }
        return Handle<T>{idx, generation_[idx]};
    }

    void Release(Handle<T> h) {
        if (h.index >= generation_.size()) {
            return;
        }
        if (generation_[h.index] != h.generation) {
            return;
        }
        generation_[h.index]++;
        freelist_.push_back(h.index);
    }

    typename T::Hot* GetHot(Handle<T> h) {
        if (h.index >= generation_.size()) {
            return nullptr;
        }
        if (generation_[h.index] != h.generation) {
            return nullptr;
        }
        return &hot_[h.index];
    }

    typename T::Cold* GetCold(Handle<T> h) {
        if (h.index >= generation_.size()) {
            return nullptr;
        }
        if (generation_[h.index] != h.generation) {
            return nullptr;
        }
        return &cold_[h.index];
    }

    size_t Size() const { return hot_.size(); }

    template <typename Fn>
    void ForEachLive(Fn fn) {
        std::vector<bool> is_free(hot_.size(), false);
        for (uint16_t fi : freelist_) {
            is_free[fi] = true;
        }
        for (size_t i = 0; i < hot_.size(); ++i) {
            if (!is_free[i]) {
                fn(hot_[i], cold_[i]);
            }
        }
    }

private:
    std::vector<typename T::Hot> hot_;
    std::vector<typename T::Cold> cold_;
    std::vector<uint16_t> generation_;
    std::vector<uint16_t> freelist_;
};

template <typename T>
struct Span {
    const T* data_ = nullptr;
    size_t size_ = 0;

    constexpr Span() = default;
    constexpr Span(const T* d, size_t s) : data_(d), size_(s) {}
    constexpr Span(std::initializer_list<T> il)
        : data_(il.begin()), size_(il.size()) {}

    constexpr const T* data() const { return data_; }
    constexpr size_t size() const { return size_; }
    constexpr bool empty() const { return size_ == 0; }
    constexpr const T& operator[](size_t i) const { return data_[i]; }
    constexpr const T* begin() const { return data_; }
    constexpr const T* end() const { return data_ + size_; }
};

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
    Span<const uint8_t> initial_data;
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
    Span<const uint8_t> initial_data;
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
    uint32_t sampler_id = 0;  // app-side sampler cache
    ShaderStage stages = kStageAll;
};

struct BindGroupDesc {
    const char* debug_name = nullptr;
    Span<const TextureBinding> textures;
    Span<const BufferBinding> buffers;
    Span<const SamplerBinding> samplers;
};

struct DynamicBinding {
    uint32_t slot = 0;
    BufferKind kind = BufferKind::kUniform;
    uint32_t max_range = 0;  // bytes; needed for validation
    ShaderStage stages = kStageAll;
};

struct DynamicBuffersDesc {
    const char* debug_name = nullptr;
    Span<const DynamicBinding> bindings;
};

// Each resource type defines Hot (read every draw) and Cold (touched only on
// create/update/destroy). Pool<T> stores them in two separate dense arrays.

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
    };
};

struct Texture {
    struct Hot {
        ApiTextureHandle api_view = nullptr;  // VkImageView / MTLTexture / WGPUTextureView
        uint32_t descriptor_index = 0;    // bindless index if used, else 0
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
        void* api_descriptor_set = nullptr;  // pre-built once at create
        uint8_t binding_count = 0;
    };
    struct Cold {
        std::vector<DynamicBinding> layout;
        const char* debug_name = nullptr;
    };
};

struct Shader {
    struct Hot {
        ApiPsoHandle api_pso = nullptr;  // VkPipeline / MTLRenderPipelineState
#if CAIRNS_VULKAN
        VkPipeline vk_pipeline = VK_NULL_HANDLE;
        VkPipelineLayout vk_layout = VK_NULL_HANDLE;
#endif
    };
    struct Cold {
        const char* debug_name = nullptr;
    };
};

struct ShaderDesc {
    ApiPsoHandle api_pso = nullptr;  // engine-compiled; rhi takes ownership
    const char* debug_name = nullptr;
#if CAIRNS_VULKAN
    VkPipeline vk_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout vk_layout = VK_NULL_HANDLE;
#endif
};

struct Kernel {
    struct Hot {
        ApiKernelHandle api_pso = nullptr;  // MTLComputePipelineState
#if CAIRNS_VULKAN
        VkPipeline vk_pipeline = VK_NULL_HANDLE;
        VkPipelineLayout vk_layout = VK_NULL_HANDLE;
#endif
    };
    struct Cold {
        const char* debug_name = nullptr;
    };
};

struct KernelDesc {
    ApiKernelHandle api_pso = nullptr;  // engine-compiled; rhi takes ownership
    const char* debug_name = nullptr;
#if CAIRNS_VULKAN
    VkPipeline vk_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout vk_layout = VK_NULL_HANDLE;
#endif
};

#if CAIRNS_VULKAN
struct BackendInitParams {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family_index = 0;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    bool enable_bda = false;
};
#elif CAIRNS_METAL
struct BackendInitParams {
    MTL::Device* device = nullptr;
    MTL::CommandQueue* queue = nullptr;
};
#else
struct BackendInitParams;
#endif  // CAIRNS_VULKAN

class ResourceManager {
public:
    static constexpr uint32_t kFramesInFlight = 2;
    static constexpr uint32_t kHeapBlockBytes = 128u * 1024u * 1024u;
    static constexpr uint32_t kLargeThreshold = 64u * 1024u * 1024u;

    ResourceManager() = default;
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    bool Init(const BackendInitParams& params);
    void Deinit();

    Handle<Buffer> CreateBuffer(const BufferDesc& desc);
    Handle<Texture> CreateTexture(const TextureDesc& desc);
    Handle<Sampler> CreateSampler(const SamplerDesc& desc);
    Handle<BindGroup> CreateBindGroup(const BindGroupDesc& desc);
    Handle<DynamicBuffers> CreateDynamicBuffers(const DynamicBuffersDesc& desc);
    Handle<Shader> CreateShader(const ShaderDesc& desc);
    Handle<Kernel> CreateKernel(const KernelDesc& desc);

    void Destroy(Handle<Buffer> h);
    void Destroy(Handle<Texture> h);
    void Destroy(Handle<Sampler> h);
    void Destroy(Handle<BindGroup> h);
    void Destroy(Handle<DynamicBuffers> h);
    void Destroy(Handle<Shader> h);
    void Destroy(Handle<Kernel> h);

    Buffer::Hot* GetHot(Handle<Buffer> h);
    Texture::Hot* GetHot(Handle<Texture> h);
    Sampler::Hot* GetHot(Handle<Sampler> h);
    BindGroup::Hot* GetHot(Handle<BindGroup> h);
    DynamicBuffers::Hot* GetHot(Handle<DynamicBuffers> h);
    Shader::Hot* GetHot(Handle<Shader> h);
    Kernel::Hot* GetHot(Handle<Kernel> h);

    // Per-frame bump ring (transient data). Returns a CPU-writable pointer that
    // maps directly into the bump ring's master platform buffer.
    void* BumpAllocate(uint32_t bytes, uint32_t align, Memory mem);
    uint32_t BumpOffset(void* ptr) const;
    Handle<Buffer> BumpMasterBuffer(Memory mem) const;

    void BeginFrame();
    void EndFrame();

    uint32_t GetBufferByteSize(Handle<Buffer> h) const;

#if CAIRNS_VULKAN
    // Same-backend native-handle access for Engine2's hand-written draw loop.
    // The neutral surface above stays pointer-free; this is Vulkan-only.
    VkBuffer GetVkBuffer(Handle<Buffer> h, uint32_t* out_offset);
    // Returns the current bump-ring slot's master VkBuffer for mem type.
    // Forces ring initialization if the current slot is uninitialized.
    VkBuffer GetVkBumpMasterBuffer(Memory mem);
    uint8_t* MappedPtr(Handle<Buffer> h);
    // Wrap an app-provided VkDescriptorSet as a BindGroup handle (wrap-only;
    // rhi does not own the set). Parallel to Metal's CreateBindGroupFromMtlBuffer.
    Handle<BindGroup> CreateBindGroupFromVkDescriptorSet(VkDescriptorSet set);
#endif  // CAIRNS_VULKAN

#if CAIRNS_METAL
    MTL::Buffer* GetMtlBuffer(Handle<Buffer> h, uint32_t* out_offset);
    uint8_t* MappedPtr(Handle<Buffer> h);
    MTL::Heap* GetMtlHeap(Handle<Buffer> h);
    MTL::Buffer* GetBumpMasterBuffer(Memory mem) const;
    Handle<BindGroup> CreateBindGroupFromMtlBuffer(MTL::Buffer* buf, uint32_t offset);
#endif  // CAIRNS_METAL

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace cairns::rhi
