// rhi/metal/resources_plat.hpp

#pragma once

#include <cstdint>

#include "rhi/resource_manager.hpp"  // Handle<>, Memory, Buffer

namespace MTL {
class Device;
class CommandQueue;
class Buffer;
}

namespace cairns::rhi {

class Resources;
class Allocator;

struct ResourcesPlat {
    MTL::Device* device_ = nullptr;             // mirrored from Device
    MTL::CommandQueue* queue_ = nullptr;        // mirrored from Device
    uint32_t frame_index_ = 1;                  // drives deferred-free + bump retire
    Resources* resources_ = nullptr;            // owner back-pointer (set in Resources::Init)

    // Native-handle resolution. Definitions in rhi/metal/resources.cpp.
    MTL::Buffer* GetMtlBuffer(Allocator& alloc, Handle<Buffer> h, uint32_t* out_offset);
    uint8_t* MappedPtr(Allocator& alloc, Handle<Buffer> h);
    MTL::Buffer* GetBumpMasterBuffer(Allocator& alloc, Memory mem) const;
};

}  // namespace cairns::rhi
