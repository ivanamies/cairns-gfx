// rhi/vulkan/bindless.cpp

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <vulkan/vulkan.h>

#include <vector>

#include "rhi/bindless.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/vulkan/internal/bindless_impl.hpp"

namespace cairns::rhi {

Bindless::~Bindless() { Deinit(); }

bool Bindless::Init(Device& device, Resources& resources) {
    if (impl_) {
        return true;
    }
    impl_ = new Impl();
    impl_->device = device.device_;
    impl_->res = &resources;
    return true;
}

void Bindless::Deinit() {
    if (!impl_) {
        return;
    }
    if (impl_->bindless_pool) {
        vkDestroyDescriptorPool(impl_->device, impl_->bindless_pool, nullptr);
    }
    if (impl_->bindless_layout) {
        vkDestroyDescriptorSetLayout(impl_->device, impl_->bindless_layout, nullptr);
    }
    delete impl_;
    impl_ = nullptr;
}

Handle<BindGroup> Bindless::CreateRegistry(const BindlessRegistryDesc& desc) {
    VkDevice device = impl_->device;

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
    bindings[0].binding = desc.texture_slot;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[0].descriptorCount = desc.max_textures;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = desc.attr_buffer_slot;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = desc.max_attr_buffers;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = desc.sampler_slot;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[2].descriptorCount = desc.max_samplers;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_info.bindingCount = 3;
    layout_info.pBindings = bindings;
    layout_info.pNext = &flags_info;
    if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr,
                                    &impl_->bindless_layout) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }

    VkDescriptorPoolSize pool_sizes[3]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[0].descriptorCount = desc.max_textures;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[1].descriptorCount = desc.max_attr_buffers;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    pool_sizes[2].descriptorCount = desc.max_samplers;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(device, &pool_info, nullptr,
                               &impl_->bindless_pool) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = impl_->bindless_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &impl_->bindless_layout;
    if (vkAllocateDescriptorSets(device, &alloc_info,
                                 &impl_->bindless_set) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }

    impl_->bindless_tex_binding = desc.texture_slot;
    impl_->bindless_attr_binding = desc.attr_buffer_slot;
    impl_->bindless_samp_binding = desc.sampler_slot;
    impl_->bindless_tex_infos.clear();
    impl_->bindless_attr_infos.clear();
    impl_->bindless_sampler_infos.clear();

    Handle<BindGroup> h = impl_->res->bind_groups.Acquire();
    impl_->res->bind_groups.GetHot(h)->api_descriptor_set = impl_->bindless_set;
    impl_->res->bind_groups.GetCold(h)->debug_name = desc.debug_name;
    impl_->bindless_handle = h;
    return h;
}

uint32_t Bindless::AddTexture(Handle<BindGroup>, Handle<Texture> tex) {
    Texture::Hot* hot = impl_->res->textures.GetHot(tex);
    VkDescriptorImageInfo img{};
    img.imageView = static_cast<VkImageView>(hot->api_view);
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    impl_->bindless_tex_infos.push_back(img);
    return static_cast<uint32_t>(impl_->bindless_tex_infos.size()) - 1;
}

uint32_t Bindless::AddAttrBuffer(Handle<BindGroup>, Handle<Buffer> buf) {
    uint32_t off = 0;
    VkBuffer vk = impl_->res->GetVkBuffer(buf, &off);
    VkDescriptorBufferInfo info{};
    info.buffer = vk;
    info.offset = off;
    info.range = impl_->res->GetBufferByteSize(buf);
    impl_->bindless_attr_infos.push_back(info);
    return static_cast<uint32_t>(impl_->bindless_attr_infos.size()) - 1;
}

uint32_t Bindless::AddSampler(Handle<BindGroup>, Handle<Sampler> samp) {
    Sampler::Hot* hot = impl_->res->samplers.GetHot(samp);
    VkDescriptorImageInfo info{};
    info.sampler = static_cast<VkSampler>(hot->api_sampler);
    impl_->bindless_sampler_infos.push_back(info);
    return static_cast<uint32_t>(impl_->bindless_sampler_infos.size()) - 1;
}

void Bindless::Finalize(Handle<BindGroup>) {
    std::vector<VkWriteDescriptorSet> writes;
    if (!impl_->bindless_tex_infos.empty()) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = impl_->bindless_set;
        w.dstBinding = impl_->bindless_tex_binding;
        w.dstArrayElement = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.descriptorCount = static_cast<uint32_t>(impl_->bindless_tex_infos.size());
        w.pImageInfo = impl_->bindless_tex_infos.data();
        writes.push_back(w);
    }
    if (!impl_->bindless_attr_infos.empty()) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = impl_->bindless_set;
        w.dstBinding = impl_->bindless_attr_binding;
        w.dstArrayElement = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = static_cast<uint32_t>(impl_->bindless_attr_infos.size());
        w.pBufferInfo = impl_->bindless_attr_infos.data();
        writes.push_back(w);
    }
    if (!impl_->bindless_sampler_infos.empty()) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = impl_->bindless_set;
        w.dstBinding = impl_->bindless_samp_binding;
        w.dstArrayElement = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.descriptorCount = static_cast<uint32_t>(impl_->bindless_sampler_infos.size());
        w.pImageInfo = impl_->bindless_sampler_infos.data();
        writes.push_back(w);
    }
    if (!writes.empty()) {
        vkUpdateDescriptorSets(impl_->device,
                               static_cast<uint32_t>(writes.size()), writes.data(),
                               0, nullptr);
    }
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
