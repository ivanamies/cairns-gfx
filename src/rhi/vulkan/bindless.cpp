// rhi/vulkan/bindless.cpp

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <vulkan/vulkan.h>

#include <vector>

#include "rhi/bindless.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"

namespace cairns::rhi {

Bindless::~Bindless() { Deinit(); }

bool Bindless::Init(Device& device, Resources& resources) {
    if (inited_) {
        return true;
    }
    device_ = device.device_;
    res_ = &resources;
    inited_ = true;
    return true;
}

void Bindless::Deinit() {
    if (!inited_) {
        return;
    }
    if (bindless_pool_) {
        vkDestroyDescriptorPool(device_, bindless_pool_, nullptr);
    }
    if (bindless_layout_) {
        vkDestroyDescriptorSetLayout(device_, bindless_layout_, nullptr);
    }
    inited_ = false;
}

Handle<BindGroup> Bindless::CreateRegistry(const BindlessRegistryDesc& desc) {
    VkDevice device = device_;

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
                                    &bindless_layout_) != VK_SUCCESS) {
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
                               &bindless_pool_) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = bindless_pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &bindless_layout_;
    if (vkAllocateDescriptorSets(device, &alloc_info,
                                 &bindless_set_) != VK_SUCCESS) {
        return Handle<BindGroup>::Null;
    }

    bindless_tex_binding_ = desc.texture_slot;
    bindless_attr_binding_ = desc.attr_buffer_slot;
    bindless_samp_binding_ = desc.sampler_slot;
    bindless_tex_infos_.clear();
    bindless_attr_infos_.clear();
    bindless_sampler_infos_.clear();

    Handle<BindGroup> h = res_->bind_groups.Acquire();
    res_->bind_groups.GetHot(h)->api_descriptor_set = bindless_set_;
    res_->bind_groups.GetCold(h)->debug_name = desc.debug_name;
    bindless_handle_ = h;
    return h;
}

uint32_t Bindless::AddTexture(Handle<BindGroup>, Handle<Texture> tex) {
    Texture::Hot* hot = res_->textures.GetHot(tex);
    VkDescriptorImageInfo img{};
    img.imageView = static_cast<VkImageView>(hot->api_view);
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bindless_tex_infos_.push_back(img);
    return static_cast<uint32_t>(bindless_tex_infos_.size()) - 1;
}

uint32_t Bindless::AddAttrBuffer(Handle<BindGroup>, Handle<Buffer> buf) {
    uint32_t off = 0;
    VkBuffer vk = res_->GetVkBuffer(buf, &off);
    VkDescriptorBufferInfo info{};
    info.buffer = vk;
    info.offset = off;
    info.range = res_->GetBufferByteSize(buf);
    bindless_attr_infos_.push_back(info);
    return static_cast<uint32_t>(bindless_attr_infos_.size()) - 1;
}

uint32_t Bindless::AddSampler(Handle<BindGroup>, Handle<Sampler> samp) {
    Sampler::Hot* hot = res_->samplers.GetHot(samp);
    VkDescriptorImageInfo info{};
    info.sampler = static_cast<VkSampler>(hot->api_sampler);
    bindless_sampler_infos_.push_back(info);
    return static_cast<uint32_t>(bindless_sampler_infos_.size()) - 1;
}

void Bindless::Finalize(Handle<BindGroup>) {
    std::vector<VkWriteDescriptorSet> writes;
    if (!bindless_tex_infos_.empty()) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = bindless_set_;
        w.dstBinding = bindless_tex_binding_;
        w.dstArrayElement = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.descriptorCount = static_cast<uint32_t>(bindless_tex_infos_.size());
        w.pImageInfo = bindless_tex_infos_.data();
        writes.push_back(w);
    }
    if (!bindless_attr_infos_.empty()) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = bindless_set_;
        w.dstBinding = bindless_attr_binding_;
        w.dstArrayElement = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = static_cast<uint32_t>(bindless_attr_infos_.size());
        w.pBufferInfo = bindless_attr_infos_.data();
        writes.push_back(w);
    }
    if (!bindless_sampler_infos_.empty()) {
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = bindless_set_;
        w.dstBinding = bindless_samp_binding_;
        w.dstArrayElement = 0;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        w.descriptorCount = static_cast<uint32_t>(bindless_sampler_infos_.size());
        w.pImageInfo = bindless_sampler_infos_.data();
        writes.push_back(w);
    }
    if (!writes.empty()) {
        vkUpdateDescriptorSets(device_,
                               static_cast<uint32_t>(writes.size()), writes.data(),
                               0, nullptr);
    }
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
