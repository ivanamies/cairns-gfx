// rhi/metal/resources.cpp  (Phase 0g-a: pools + destroy/get + frame counter)

#include "util/define.hpp"

#if CAIRNS_METAL

#include <Metal/Metal.hpp>

#include "rhi/resources.hpp"
#include "rhi/device.hpp"
#include "rhi/allocator.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/metal/internal/resources_impl.hpp"
#include "rhi/metal/internal/device_impl.hpp"
#include "rhi/metal/internal/allocator_impl.hpp"

namespace cairns::rhi {

Resources::~Resources() { Deinit(); }

bool Resources::Init(Device& device, Allocator& alloc) {
    if (impl_) {
        return true;
    }
    impl_ = new Impl();
    impl_->device = device.impl_->device;
    impl_->alloc = &alloc;
    return true;
}

void Resources::Deinit() {
    if (!impl_) {
        return;
    }
    delete impl_;
    impl_ = nullptr;
}

void Resources::AdvanceFrame() {
    impl_->frame_index++;
    impl_->alloc->AdvanceFrame(impl_->frame_index);
}

uint32_t Resources::FrameIndex() const { return impl_->frame_index; }

void Resources::Destroy(Handle<Buffer> h) {
    Buffer::Hot* hot = buffers.GetHot(h);
    Buffer::Cold* cold = buffers.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    impl_->alloc->impl_->memory.FreeBuffer(
        hot->heap_buffer_index, cold->alloc,
        impl_->frame_index + ResourceManager::kFramesInFlight);
    buffers.Release(h);
}

void Resources::Destroy(Handle<Texture> h) {
    Texture::Hot* hot = textures.GetHot(h);
    Texture::Cold* cold = textures.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    impl_->alloc->impl_->memory.FreeImage(
        cold->heap_buffer_index, cold->alloc, hot->api_view,
        impl_->frame_index + ResourceManager::kFramesInFlight);
    textures.Release(h);
}

void Resources::Destroy(Handle<Sampler> h) {
    Sampler::Hot* hot = samplers.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_sampler) {
        hot->api_sampler->release();
        hot->api_sampler = nullptr;
    }
    samplers.Release(h);
}

void Resources::Destroy(Handle<BindGroup> h) { bind_groups.Release(h); }

void Resources::Destroy(Handle<DynamicBuffers> h) { dynamic_buffers.Release(h); }

void Resources::Destroy(Handle<Shader> h) {
    Shader::Hot* hot = shaders.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_pso) {
        hot->api_pso->release();
        hot->api_pso = nullptr;
    }
    shaders.Release(h);
}

void Resources::Destroy(Handle<Kernel> h) {
    Kernel::Hot* hot = kernels.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_pso) {
        hot->api_pso->release();
        hot->api_pso = nullptr;
    }
    kernels.Release(h);
}

Buffer::Hot* Resources::GetHot(Handle<Buffer> h) { return buffers.GetHot(h); }
Texture::Hot* Resources::GetHot(Handle<Texture> h) { return textures.GetHot(h); }
Sampler::Hot* Resources::GetHot(Handle<Sampler> h) { return samplers.GetHot(h); }
BindGroup::Hot* Resources::GetHot(Handle<BindGroup> h) { return bind_groups.GetHot(h); }
DynamicBuffers::Hot* Resources::GetHot(Handle<DynamicBuffers> h) {
    return dynamic_buffers.GetHot(h);
}
Shader::Hot* Resources::GetHot(Handle<Shader> h) { return shaders.GetHot(h); }
Kernel::Hot* Resources::GetHot(Handle<Kernel> h) { return kernels.GetHot(h); }

uint32_t Resources::GetBufferByteSize(Handle<Buffer> h) {
    Buffer::Cold* cold = buffers.GetCold(h);
    if (!cold) {
        return 0;
    }
    return cold->size_bytes;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
