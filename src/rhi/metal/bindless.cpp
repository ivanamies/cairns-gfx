// rhi/metal/bindless.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include <Metal/Metal.hpp>

#include "rhi/bindless.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"

namespace cairns::rhi {

Bindless::~Bindless() { Deinit(); }

bool Bindless::Init(Device& device) {
    if (inited_) {
        return true;
    }
    device_ = device.device_;
    inited_ = true;
    return true;
}

void Bindless::Deinit() {
    if (!inited_) {
        return;
    }
    if (bindless_encoder_) {
        bindless_encoder_->release();
        bindless_encoder_ = nullptr;
    }
    inited_ = false;
}

Handle<BindGroup> Bindless::CreateRegistry(Resources& resources, Allocator& alloc,
                                           const BindlessRegistryDesc& desc) {
    MTL::Device* device = device_;

    auto* texArg = MTL::ArgumentDescriptor::alloc()->init();
    texArg->setDataType(MTL::DataTypeTexture);
    texArg->setIndex(desc.texture_slot);
    texArg->setArrayLength(desc.max_textures);
    texArg->setAccess(MTL::ArgumentAccessReadOnly);

    auto* attrArg = MTL::ArgumentDescriptor::alloc()->init();
    attrArg->setDataType(MTL::DataTypePointer);
    attrArg->setIndex(desc.attr_buffer_slot);
    attrArg->setArrayLength(desc.max_attr_buffers);
    attrArg->setAccess(MTL::ArgumentAccessReadOnly);

    auto* sampArg = MTL::ArgumentDescriptor::alloc()->init();
    sampArg->setDataType(MTL::DataTypeSampler);
    sampArg->setIndex(desc.sampler_slot);
    sampArg->setArrayLength(desc.max_samplers);
    sampArg->setAccess(MTL::ArgumentAccessReadOnly);

    NS::Array* args = NS::Array::array((NS::Object*[]){texArg, attrArg, sampArg}, 3);
    MTL::ArgumentEncoder* arg_encoder = device->newArgumentEncoder(args);

    BufferDesc bd;
    bd.byte_size = static_cast<uint32_t>(arg_encoder->encodedLength());
    bd.usage = kUsageUniform | kUsageStorage;
    bd.memory = Memory::kUpload;
    Handle<Buffer> arg_buf_h = resources.CreateBuffer(alloc, bd);
    uint32_t arg_off = 0;
    MTL::Buffer* arg_buf = resources.GetMtlBuffer(alloc, arg_buf_h, &arg_off);
    arg_encoder->setArgumentBuffer(arg_buf, arg_off);

    bindless_encoder_ = arg_encoder;
    bindless_tex_base_ = desc.texture_slot;
    bindless_attr_base_ = desc.attr_buffer_slot;
    bindless_samp_base_ = desc.sampler_slot;
    bindless_num_tex_ = 0;
    bindless_num_attr_ = 0;
    bindless_num_samp_ = 0;

    Handle<BindGroup> h = resources.bind_groups.Acquire();
    BindGroup::Hot* hot = resources.bind_groups.GetHot(h);
    hot->api_descriptor_set = arg_buf;
    hot->arg_buf_offset = arg_off;
    resources.bind_groups.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

uint32_t Bindless::AddTexture(Resources& resources, Handle<BindGroup>, Handle<Texture> tex) {
    MTL::Texture* t = resources.textures.GetHot(tex)->api_view;
    const uint32_t slot = bindless_num_tex_;
    bindless_encoder_->setTexture(t, bindless_tex_base_ + slot);
    bindless_num_tex_ = slot + 1;
    return slot;
}

uint32_t Bindless::AddAttrBuffer(Resources& resources, Allocator& alloc, Handle<BindGroup>,
                                 Handle<Buffer> buf) {
    uint32_t off = 0;
    MTL::Buffer* b = resources.GetMtlBuffer(alloc, buf, &off);
    const uint32_t slot = bindless_num_attr_;
    bindless_encoder_->setBuffer(b, off, bindless_attr_base_ + slot);
    bindless_num_attr_ = slot + 1;
    return slot;
}

uint32_t Bindless::AddSampler(Resources& resources, Handle<BindGroup>, Handle<Sampler> samp) {
    MTL::SamplerState* s = resources.samplers.GetHot(samp)->api_sampler;
    const uint32_t slot = bindless_num_samp_;
    bindless_encoder_->setSamplerState(s, bindless_samp_base_ + slot);
    bindless_num_samp_ = slot + 1;
    return slot;
}

void Bindless::Finalize(Handle<BindGroup>) {
    if (bindless_encoder_) {
        bindless_encoder_->release();
        bindless_encoder_ = nullptr;
    }
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
