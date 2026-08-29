// rhi/allocator.hpp
//
// GPU memory: wraps the backend MemoryAllocator + the per-frame bump ring +
// the device alignment requirements. Depends on Device. Handle-resolving
// helpers (BufferBaseOffset/MappedPtr) live on Resources, not here, because
// they need the buffer pool to resolve a Handle<Buffer>.

#pragma once

#include "util/define.hpp"

#include <cstdint>

#include "rhi/resource_manager.hpp"  // Handle<>, Memory, Buffer

namespace cairns::rhi {

class Device;

class Allocator {
public:
    Allocator() = default;
    ~Allocator();
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    [[nodiscard]] bool Init(Device& device);
    void Deinit();

    // Per-frame bump ring (transient data).
    void* BumpAllocate(uint32_t bytes, uint32_t align, Memory mem);
    uint32_t BumpOffset(void* ptr) const;
    Handle<Buffer> BumpMasterBuffer(Memory mem) const;

    // Minimum dynamic-UBO / SSBO offset alignment for this backend.
    uint32_t UboAlign() const;
    uint32_t StorageAlign() const;

    // Advance the bump ring to the given frame index (retire + begin).
    void AdvanceFrame(uint32_t frame_index);

private:
    friend class ResourceManager;
    friend class Resources;
    friend class Frames;
    friend class CommandRecorder;

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace cairns::rhi
