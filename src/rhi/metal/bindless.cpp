// rhi/metal/bindless.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include <Metal/Metal.hpp>

#include "rhi/bindless.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/metal/internal/bindless_impl.hpp"
#include "rhi/metal/internal/device_impl.hpp"

namespace cairns::rhi {

Bindless::~Bindless() { Deinit(); }

bool Bindless::Init(Device& device, Resources& resources) {
    if (impl_) {
        return true;
    }
    impl_ = new Impl();
    impl_->device = device.impl_->device;
    impl_->res = &resources;
    return true;
}

void Bindless::Deinit() {
    if (!impl_) {
        return;
    }
    if (impl_->bindless_encoder) {
        impl_->bindless_encoder->release();
        impl_->bindless_encoder = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
}

Handle<BindGroup> Bindless::CreateRegistry(const BindlessRegistryDesc& desc) {
    MTL::Device* device = impl_->device;

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
    Handle<Buffer> arg_buf_h = impl_->res->CreateBuffer(bd);
    uint32_t arg_off = 0;
    MTL::Buffer* arg_buf = impl_->res->GetMtlBuffer(arg_buf_h, &arg_off);
    arg_encoder->setArgumentBuffer(arg_buf, arg_off);

    impl_->bindless_encoder = arg_encoder;
    impl_->bindless_tex_base = desc.texture_slot;
    impl_->bindless_attr_base = desc.attr_buffer_slot;
    impl_->bindless_samp_base = desc.sampler_slot;
    impl_->bindless_num_tex = 0;
    impl_->bindless_num_attr = 0;
    impl_->bindless_num_samp = 0;

    Handle<BindGroup> h = impl_->res->bind_groups.Acquire();
    BindGroup::Hot* hot = impl_->res->bind_groups.GetHot(h);
    hot->api_descriptor_set = arg_buf;
    hot->arg_buf_offset = arg_off;
    impl_->res->bind_groups.GetCold(h)->debug_name = desc.debug_name;
    return h;
}

uint32_t Bindless::AddTexture(Handle<BindGroup>, Handle<Texture> tex) {
    MTL::Texture* t = impl_->res->textures.GetHot(tex)->api_view;
    const uint32_t slot = impl_->bindless_num_tex;
    impl_->bindless_encoder->setTexture(t, impl_->bindless_tex_base + slot);
    impl_->bindless_num_tex = slot + 1;
    return slot;
}

uint32_t Bindless::AddAttrBuffer(Handle<BindGroup>, Handle<Buffer> buf) {
    uint32_t off = 0;
    MTL::Buffer* b = impl_->res->GetMtlBuffer(buf, &off);
    const uint32_t slot = impl_->bindless_num_attr;
    impl_->bindless_encoder->setBuffer(b, off, impl_->bindless_attr_base + slot);
    impl_->bindless_num_attr = slot + 1;
    return slot;
}

uint32_t Bindless::AddSampler(Handle<BindGroup>, Handle<Sampler> samp) {
    MTL::SamplerState* s = impl_->res->samplers.GetHot(samp)->api_sampler;
    const uint32_t slot = impl_->bindless_num_samp;
    impl_->bindless_encoder->setSamplerState(s, impl_->bindless_samp_base + slot);
    impl_->bindless_num_samp = slot + 1;
    return slot;
}

void Bindless::Finalize(Handle<BindGroup>) {
    if (impl_->bindless_encoder) {
        impl_->bindless_encoder->release();
        impl_->bindless_encoder = nullptr;
    }
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
