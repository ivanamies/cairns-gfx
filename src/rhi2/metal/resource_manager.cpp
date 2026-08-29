// rhi2/metal/resource_manager.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi2/resource_manager.hpp"

#include <algorithm>
#include <cstring>

#include <Metal/Metal.hpp>

#include "rhi2/metal/memory_allocator.hpp"

namespace cairns::rhi2 {

struct ResourceManager::Impl {
    BackendInitParams params;
    metal::MemoryAllocator memory;

    Pool<Buffer> buffers;
    Pool<Texture> textures;
    Pool<Sampler> samplers;
    Pool<BindGroup> bind_groups;
    Pool<DynamicBuffers> dynamic_buffers;

    uint32_t frame_index = 0;
};

namespace {

bool is_host_visible(Memory mem) {
    return mem == Memory::kUpload || mem == Memory::kDynamic ||
           mem == Memory::kReadback;
}

}  // namespace

ResourceManager::~ResourceManager() {
    Deinit();
}

void ResourceManager::Deinit() {
    delete impl_;
    impl_ = nullptr;
}

bool ResourceManager::Init(const BackendInitParams& params) {
    impl_ = new Impl();
    impl_->params = params;
    if (!impl_->memory.Init(params.device)) {
        return false;
    }
    return true;
}

Handle<Buffer> ResourceManager::CreateBuffer(const BufferDesc& d) {
    metal::AllocResult r =
        impl_->memory.AllocBuffer(d.byte_size, d.usage, d.memory, 16);
    if (!r.ok) {
        return Handle<Buffer>::Null;
    }

    Handle<Buffer> h = impl_->buffers.Acquire();
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    hot->heap_buffer_index = static_cast<uint16_t>(r.heap_index);
    hot->pad = 0;
    hot->offset_in_heap = r.offset;

    Buffer::Cold* cold = impl_->buffers.GetCold(h);
    cold->alloc = r.alloc;
    cold->size_bytes = d.byte_size;
    cold->usage = d.usage;
    cold->mem_type = d.memory;
    cold->debug_name = d.debug_name;

    if (!d.initial_data.empty()) {
        if (is_host_visible(d.memory)) {
            uint8_t* dst = MappedPtr(h);
            if (dst) {
                std::memcpy(dst, d.initial_data.data(), d.initial_data.size());
            }
        } else {
            void* staging = impl_->memory.BumpAllocate(
                static_cast<uint32_t>(d.initial_data.size()), 16,
                Memory::kUpload);
            if (staging) {
                std::memcpy(staging, d.initial_data.data(),
                            d.initial_data.size());
                uint32_t src_off = impl_->memory.BumpOffset(staging);
                uint32_t src_hi =
                    impl_->memory.BumpMasterHeapIndex(Memory::kUpload);
                MTL::Buffer* src = impl_->memory.HeapMasterBuffer(src_hi);
                MTL::Buffer* dst_buf =
                    impl_->memory.HeapMasterBuffer(r.heap_index);
                MTL::CommandBuffer* cmd =
                    impl_->params.queue->commandBuffer();
                MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
                blit->copyFromBuffer(src, src_off, dst_buf, r.offset,
                                     static_cast<uint32_t>(
                                         d.initial_data.size()));
                blit->endEncoding();
                cmd->commit();
                cmd->waitUntilCompleted();
            }
        }
    }
    return h;
}

Handle<Texture> ResourceManager::CreateTexture(const TextureDesc&) {
    return Handle<Texture>::Null;
}

Handle<Sampler> ResourceManager::CreateSampler(const SamplerDesc&) {
    return Handle<Sampler>::Null;
}

Handle<BindGroup> ResourceManager::CreateBindGroup(const BindGroupDesc&) {
    return Handle<BindGroup>::Null;
}

Handle<DynamicBuffers> ResourceManager::CreateDynamicBuffers(
    const DynamicBuffersDesc&) {
    return Handle<DynamicBuffers>::Null;
}

void ResourceManager::Destroy(Handle<Buffer> h) {
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    Buffer::Cold* cold = impl_->buffers.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    impl_->memory.FreeBuffer(hot->heap_buffer_index, cold->alloc,
                             impl_->frame_index + kFramesInFlight);
    impl_->buffers.Release(h);
}

void ResourceManager::Destroy(Handle<Texture> h) {
    Texture::Hot* hot = impl_->textures.GetHot(h);
    Texture::Cold* cold = impl_->textures.GetCold(h);
    if (!hot || !cold) {
        return;
    }
    impl_->memory.FreeImage(cold->heap_buffer_index, cold->alloc,
                            static_cast<MTL::Texture*>(hot->api_view),
                            impl_->frame_index + kFramesInFlight);
    impl_->textures.Release(h);
}

void ResourceManager::Destroy(Handle<Sampler> h) {
    Sampler::Hot* hot = impl_->samplers.GetHot(h);
    if (!hot) {
        return;
    }
    if (hot->api_sampler) {
        static_cast<MTL::SamplerState*>(hot->api_sampler)->release();
        hot->api_sampler = nullptr;
    }
    impl_->samplers.Release(h);
}

void ResourceManager::Destroy(Handle<BindGroup> h) {
    impl_->bind_groups.Release(h);
}

void ResourceManager::Destroy(Handle<DynamicBuffers> h) {
    impl_->dynamic_buffers.Release(h);
}

Buffer::Hot* ResourceManager::GetHot(Handle<Buffer> h) {
    return impl_->buffers.GetHot(h);
}

Texture::Hot* ResourceManager::GetHot(Handle<Texture> h) {
    return impl_->textures.GetHot(h);
}

Sampler::Hot* ResourceManager::GetHot(Handle<Sampler> h) {
    return impl_->samplers.GetHot(h);
}

BindGroup::Hot* ResourceManager::GetHot(Handle<BindGroup> h) {
    return impl_->bind_groups.GetHot(h);
}

DynamicBuffers::Hot* ResourceManager::GetHot(Handle<DynamicBuffers> h) {
    return impl_->dynamic_buffers.GetHot(h);
}

void* ResourceManager::BumpAllocate(uint32_t bytes, uint32_t align,
                                    Memory mem) {
    return impl_->memory.BumpAllocate(bytes, align, mem);
}

uint32_t ResourceManager::BumpOffset(void* ptr) const {
    return impl_->memory.BumpOffset(ptr);
}

Handle<Buffer> ResourceManager::BumpMasterBuffer(Memory mem) const {
    Handle<Buffer> h;
    h.index = static_cast<uint16_t>(impl_->memory.BumpMasterHeapIndex(mem));
    h.generation = 0;
    return h;
}

void ResourceManager::BeginFrame() {
    impl_->frame_index++;
    uint32_t slot = impl_->frame_index % kFramesInFlight;
    impl_->memory.RetireFrame(slot);
    impl_->memory.BeginFrame(impl_->frame_index);
}

void ResourceManager::EndFrame() {}

MTL::Buffer* ResourceManager::GetMtlBuffer(Handle<Buffer> h,
                                            uint32_t* out_offset) {
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    if (!hot) {
        if (out_offset) {
            *out_offset = 0;
        }
        return nullptr;
    }
    if (out_offset) {
        *out_offset = hot->offset_in_heap;
    }
    return impl_->memory.HeapMasterBuffer(hot->heap_buffer_index);
}

uint8_t* ResourceManager::MappedPtr(Handle<Buffer> h) {
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    if (!hot) {
        return nullptr;
    }
    uint8_t* base = static_cast<uint8_t*>(
        impl_->memory.HeapMappedPtr(hot->heap_buffer_index));
    if (!base) {
        return nullptr;
    }
    return base + hot->offset_in_heap;
}

MTL::Heap* ResourceManager::GetMtlHeap(Handle<Buffer> h) {
    Buffer::Hot* hot = impl_->buffers.GetHot(h);
    if (!hot) {
        return nullptr;
    }
    return impl_->memory.HeapHandle(hot->heap_buffer_index);
}

}  // namespace cairns::rhi2

#endif  // CAIRNS_METAL
