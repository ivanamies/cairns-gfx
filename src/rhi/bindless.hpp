// rhi/bindless.hpp
//
// The bindless registry: one descriptor set (Vulkan) / argument buffer (Metal)
// holding all textures, attribute buffers, and samplers a frame can reference by
// index. Depends on Device + Resources (resolves handles to native objects).
// One registry in flight at a time (build via CreateRegistry → Add* → Finalize).

#pragma once

#include "util/define.hpp"

#include <cstdint>

#include "rhi/resource_manager.hpp"  // Handle<>, resource types, BindlessRegistryDesc

namespace cairns::rhi {

class Device;
class Resources;

class Bindless {
public:
    Bindless() = default;
    ~Bindless();
    Bindless(const Bindless&) = delete;
    Bindless& operator=(const Bindless&) = delete;

    [[nodiscard]] bool Init(Device& device, Resources& resources);
    void Deinit();

    Handle<BindGroup> CreateRegistry(const BindlessRegistryDesc& desc);
    uint32_t AddTexture(Handle<BindGroup> reg, Handle<Texture> tex);
    uint32_t AddAttrBuffer(Handle<BindGroup> reg, Handle<Buffer> buf);
    uint32_t AddSampler(Handle<BindGroup> reg, Handle<Sampler> samp);
    void Finalize(Handle<BindGroup> reg);

private:
    friend class ResourceManager;  // Vulkan pipeline layout reads bindless_layout

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace cairns::rhi
