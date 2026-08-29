// rhi/resources.hpp
//
// The resource pools (Aaltonen "arrays you walk") + their create/destroy/get +
// pipeline creation. Depends on Device + Allocator. The 7 typed ResourceManager<T> are
// public members; convenience methods delegate to them. Handle-resolving helpers
// (BufferBaseOffset/MappedPtr/GetVkBuffer/GetMtlBuffer) live here because they
// need the pool to resolve a Handle.

#pragma once

#include "util/define.hpp"

#include <cstdint>
#include <span>

#include "rhi/resource_manager.hpp"  // ResourceManager<T>, Handle<>, resource types, Descs
#if CAIRNS_METAL
#include "rhi/metal/resources_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/resources_plat.hpp"
#endif

namespace cairns::rhi {

class Device;
class Allocator;

class Resources {
public:
    Resources() = default;
    ~Resources();
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;

    [[nodiscard]] bool Init(Device& device);
    void Deinit();

    Handle<Buffer> CreateBuffer(Allocator& alloc, const BufferDesc& desc);
    void UploadBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t dst_offset,
                      std::span<const uint8_t> data);
    Handle<Texture> CreateTexture(Allocator& alloc, const TextureDesc& desc);
    Handle<Sampler> CreateSampler(const SamplerDesc& desc);
    Handle<BindGroup> CreateBindGroup(const BindGroupDesc& desc);
    Handle<DynamicBuffers> CreateDynamicBuffers(const DynamicBuffersDesc& desc);

    // Typed generational pools — public; walk them directly for debug/iteration.
    ResourceManager<Buffer> buffers;
    ResourceManager<Texture> textures;
    ResourceManager<Sampler> samplers;
    ResourceManager<BindGroup> bind_groups;
    ResourceManager<DynamicBuffers> dynamic_buffers;
    ResourceManager<Shader> shaders;
    ResourceManager<Kernel> kernels;

    void Destroy(Allocator& alloc, Handle<Buffer> h);
    void Destroy(Allocator& alloc, Handle<Texture> h);
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

    uint32_t GetBufferByteSize(Handle<Buffer> h);

    // Byte offset of a buffer within its backing master allocation.
    uint32_t BufferBaseOffset(Allocator& alloc, Handle<Buffer> h);

#if CAIRNS_VULKAN
    // Native-handle resolution used by the Vulkan CommandRecorder + Bindless.
    VkBuffer GetVkBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t* out_offset);
    VkBuffer GetVkBumpMasterBuffer(Allocator& alloc, Memory mem);
    uint8_t* MappedPtr(Allocator& alloc, Handle<Buffer> h);
    // set 2: shared layout for per-material combined image+sampler bind groups
    // (created lazily on first CreateBindGroup). The unlit pipeline layout references it.
    VkDescriptorSetLayout MaterialSetLayout();
#endif  // CAIRNS_VULKAN

#if CAIRNS_METAL
    // Native-handle resolution used by the Metal CommandRecorder + Bindless.
    MTL::Buffer* GetMtlBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t* out_offset);
    uint8_t* MappedPtr(Allocator& alloc, Handle<Buffer> h);
    MTL::Buffer* GetBumpMasterBuffer(Allocator& alloc, Memory mem) const;
#endif  // CAIRNS_METAL

    // Advance the resource frame counter (drives deferred-free + the bump ring).
    void AdvanceFrame(Allocator& alloc);
    uint32_t FrameIndex() const;

private:
    // Internal state — self-only (Bindless/CommandRecorder/Pipelines use the
    // PUBLIC pools + methods above, never this).
    ResourcesPlat plat;
    bool inited_ = false;
};

}  // namespace cairns::rhi
