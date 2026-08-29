// rhi/bindless.hpp
//
// The bindless registry: one descriptor set (Vulkan) / argument buffer (Metal)
// holding all textures, attribute buffers, and samplers a frame can reference by
// index. Depends on Device + Resources (resolves handles to native objects).
// One registry in flight at a time (build via CreateRegistry → Add* → Finalize).

#pragma once

#include "util/define.hpp"

#include <cstdint>
#include <vector>

#include "rhi/resource_manager.hpp"  // Handle<>, resource types, BindlessRegistryDesc
#if CAIRNS_VULKAN
#include <vulkan/vulkan.h>
#elif CAIRNS_METAL
#include <Metal/Metal.hpp>
#endif

namespace cairns::rhi {

class Device;
class Resources;

class Bindless {
public:
    Bindless() = default;
    ~Bindless();
    Bindless(const Bindless&) = delete;
    Bindless& operator=(const Bindless&) = delete;

    // CALLER: ENGINE.
    [[nodiscard]] bool Init(Device& device, Resources& resources);
    // CALLER: ENGINE.
    void Deinit();

    // CALLER: ENGINE (registry build: CreateRegistry -> Add* -> Finalize).
    Handle<BindGroup> CreateRegistry(const BindlessRegistryDesc& desc);
    uint32_t AddTexture(Handle<BindGroup> reg, Handle<Texture> tex);
    uint32_t AddAttrBuffer(Handle<BindGroup> reg, Handle<Buffer> buf);
    uint32_t AddSampler(Handle<BindGroup> reg, Handle<Sampler> samp);
    void Finalize(Handle<BindGroup> reg);

    // Registry state; Pipelines reads bindless_layout_ (vk pipeline layout).
#if CAIRNS_VULKAN
    VkDevice device_ = VK_NULL_HANDLE;          // mirrored from Device
    VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
    Handle<BindGroup> bindless_handle_;
    uint32_t bindless_tex_binding_ = 0;
    uint32_t bindless_attr_binding_ = 0;
    uint32_t bindless_samp_binding_ = 0;
    std::vector<VkDescriptorImageInfo> bindless_tex_infos_;
    std::vector<VkDescriptorBufferInfo> bindless_attr_infos_;
    std::vector<VkDescriptorImageInfo> bindless_sampler_infos_;
#elif CAIRNS_METAL
    MTL::Device* device_ = nullptr;             // mirrored from Device
    MTL::ArgumentEncoder* bindless_encoder_ = nullptr;
    uint32_t bindless_tex_base_ = 0;
    uint32_t bindless_attr_base_ = 0;
    uint32_t bindless_samp_base_ = 0;
    uint32_t bindless_num_tex_ = 0;
    uint32_t bindless_num_attr_ = 0;
    uint32_t bindless_num_samp_ = 0;
#endif
    Resources* res_ = nullptr;                  // borrowed

private:
    bool inited_ = false;
};

}  // namespace cairns::rhi
