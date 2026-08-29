// rhi/metal/allocator.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include <cstdint>

#include "rhi/allocator.hpp"
#include "rhi/device.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/metal/internal/allocator_impl.hpp"
#include "rhi/metal/internal/device_impl.hpp"

namespace cairns::rhi {

Allocator::~Allocator() { Deinit(); }

bool Allocator::Init(Device& device) {
    if (impl_) {
        return true;
    }
    impl_ = new Impl();
    return impl_->memory.Init(device.impl_->device);
}

void Allocator::Deinit() {
    if (!impl_) {
        return;
    }
    // MemoryAllocator dtor frees device heaps; the device is still alive
    // (Device::Deinit runs after Allocator::Deinit in engine teardown order).
    delete impl_;
    impl_ = nullptr;
}

void* Allocator::BumpAllocate(uint32_t bytes, uint32_t align, Memory mem) {
    return impl_->memory.BumpAllocate(bytes, align, mem);
}

uint32_t Allocator::BumpOffset(void* ptr) const {
    return impl_->memory.BumpOffset(ptr);
}

Handle<Buffer> Allocator::BumpMasterBuffer(Memory mem) const {
    Handle<Buffer> h;
    h.index = static_cast<uint16_t>(impl_->memory.BumpMasterHeapIndex(mem));
    h.generation = 0;
    return h;
}

uint32_t Allocator::UboAlign() const { return 32; }
uint32_t Allocator::StorageAlign() const { return 32; }

void Allocator::AdvanceFrame(uint32_t frame_index) {
    const uint32_t slot = frame_index % kFramesInFlight;
    impl_->memory.RetireFrame(slot);
    impl_->memory.BeginFrame(frame_index);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
