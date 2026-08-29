// rhi/vulkan/allocator.cpp

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <algorithm>
#include <cstdint>

#include <vulkan/vulkan.h>

#include "rhi/allocator.hpp"
#include "rhi/device.hpp"
#include "rhi/resource_manager.hpp"

namespace cairns::rhi {

Allocator::~Allocator() { Deinit(); }

bool Allocator::Init(Device& device) {
    if (inited_) {
        return true;
    }
    if (!memory_.Init(device.device_, device.physical_, false)) {
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.physical_, &props);
    uniform_align_ = std::max(
        1u, static_cast<uint32_t>(props.limits.minUniformBufferOffsetAlignment));
    storage_align_ = std::max(
        1u, static_cast<uint32_t>(props.limits.minStorageBufferOffsetAlignment));
    inited_ = true;
    return true;
}

void Allocator::Deinit() {
    if (!inited_) {
        return;
    }
    // Free device memory now, while the device is still alive (Device::Deinit
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

uint32_t Allocator::UboAlign() const { return uniform_align_; }
uint32_t Allocator::StorageAlign() const { return storage_align_; }

void Allocator::AdvanceFrame(uint32_t frame_index) {
    const uint32_t slot = frame_index % kFramesInFlight;
    memory_.RetireFrame(slot);
    memory_.BeginFrame(frame_index);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
