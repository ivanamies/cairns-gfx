// rhi/resources.hpp
//
// The resource pools (Aaltonen "arrays you walk") + their create/destroy/get +
// pipeline creation. Depends on Device + Allocator. The 7 typed Pool<T> are
// public members; convenience methods delegate to them. Handle-resolving helpers
// (BufferBaseOffset/MappedPtr/GetVkBuffer/GetMtlBuffer) live here because they
// need the pool to resolve a Handle.

#pragma once

#include "util/define.hpp"

#include <cstdint>

#include "rhi/resource_manager.hpp"  // Pool<T>, Handle<>, resource types, Descs

namespace cairns::rhi {

class Device;
class Allocator;

class Resources {
public:
    Resources() = default;
    ~Resources();
    Resources(const Resources&) = delete;
    Resources& operator=(const Resources&) = delete;

    [[nodiscard]] bool Init(Device& device, Allocator& alloc);
    void Deinit();

    Handle<Buffer> CreateBuffer(const BufferDesc& desc);
    Handle<Texture> CreateTexture(const TextureDesc& desc);
    Handle<Sampler> CreateSampler(const SamplerDesc& desc);
    Handle<BindGroup> CreateBindGroup(const BindGroupDesc& desc);
    Handle<DynamicBuffers> CreateDynamicBuffers(const DynamicBuffersDesc& desc);

    // Typed generational pools — public; walk them directly for debug/iteration.
    Pool<Buffer> buffers;
    Pool<Texture> textures;
    Pool<Sampler> samplers;
    Pool<BindGroup> bind_groups;
    Pool<DynamicBuffers> dynamic_buffers;
    Pool<Shader> shaders;
    Pool<Kernel> kernels;

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

    uint32_t GetBufferByteSize(Handle<Buffer> h);

    // Byte offset of a buffer within its backing master allocation.
    uint32_t BufferBaseOffset(Handle<Buffer> h);

#if CAIRNS_VULKAN
    // Native-handle resolution used by the Vulkan CommandRecorder + Bindless.
    VkBuffer GetVkBuffer(Handle<Buffer> h, uint32_t* out_offset);
    VkBuffer GetVkBumpMasterBuffer(Memory mem);
    uint8_t* MappedPtr(Handle<Buffer> h);
#endif  // CAIRNS_VULKAN

#if CAIRNS_METAL
    // Native-handle resolution used by the Metal CommandRecorder + Bindless.
    MTL::Buffer* GetMtlBuffer(Handle<Buffer> h, uint32_t* out_offset);
    uint8_t* MappedPtr(Handle<Buffer> h);
    MTL::Buffer* GetBumpMasterBuffer(Memory mem) const;
#endif  // CAIRNS_METAL

    // Advance the resource frame counter (drives deferred-free + the bump ring).
    void AdvanceFrame();
    uint32_t FrameIndex() const;

private:
    friend class Bindless;
    friend class CommandRecorder;

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace cairns::rhi
