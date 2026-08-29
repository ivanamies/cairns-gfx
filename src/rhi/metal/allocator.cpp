// rhi/metal/allocator.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include <cstdint>

#include "rhi/allocator.hpp"
#include "rhi/device.hpp"
#include "rhi/resource_manager.hpp"

namespace cairns::rhi {

Allocator::~Allocator() { Deinit(); }

bool Allocator::Init(Device& device) {
    if (inited_) {
        return true;
    }
    inited_ = memory_.Init(device.device_);
    return inited_;
}

void Allocator::Deinit() {
    if (!inited_) {
        return;
    }
    // Free device heaps now, while the device is still alive (Device::Deinit
    // runs after Allocator::Deinit in engine teardown order). The by-value
    // memory_ would otherwise not free until ~Allocator, after device death.
    memory_.Deinit();
    inited_ = false;
}

void* Allocator::BumpAllocate(uint32_t bytes, uint32_t align, Memory mem,
                              uint32_t* out_offset) {
    return memory_.BumpAllocate(bytes, align, mem, out_offset);
}

Handle<Buffer> Allocator::BumpMasterBuffer(Memory mem) const {
    Handle<Buffer> h;
    h.index = static_cast<uint16_t>(memory_.BumpMasterHeapIndex(mem));
    h.generation = 0;
    return h;
}

uint32_t Allocator::UboAlign() const { return 32; }
uint32_t Allocator::StorageAlign() const { return 32; }

void Allocator::AdvanceFrame(uint32_t frame_index) {
    const uint32_t slot = frame_index % kFramesInFlight;
    memory_.RetireFrame(slot);
    memory_.BeginFrame(frame_index);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
