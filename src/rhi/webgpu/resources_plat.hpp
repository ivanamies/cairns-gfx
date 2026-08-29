// rhi/webgpu/resources_plat.hpp
#pragma once

#include <cstdint>

// Opaque-handle forward declarations instead of webgpu.h -- the pure-CPU spec
// tests reach this header with no backend include dirs.
extern "C" {
typedef struct WGPUDeviceImpl* WGPUDevice;
typedef struct WGPUQueueImpl* WGPUQueue;
typedef struct WGPUBufferImpl* WGPUBuffer;
}

#include "rhi/resource_manager.hpp"  // Handle<>, Memory, Buffer

namespace cairns::rhi {

class Resources;
class Allocator;

struct ResourcesPlat {
    WGPUDevice device_ = nullptr;          // mirrored from Device
    WGPUQueue queue_ = nullptr;            // mirrored from Device
    uint32_t frame_index_ = 1;             // drives deferred-free + bump retire
    Resources* resources_ = nullptr;       // owner back-pointer (set in Resources::Init)

    // Native-handle resolution. Definitions in rhi/webgpu/resources.cpp.
    WGPUBuffer GetWgpuBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t* out_offset);
    uint8_t* MappedPtr(Allocator& alloc, Handle<Buffer> h);
    WGPUBuffer GetBumpMasterBuffer(Allocator& alloc, Memory mem) const;
};

}  // namespace cairns::rhi
