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
#if CAIRNS_METAL
#include "rhi/metal/allocator_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/allocator_plat.hpp"
#endif

namespace cairns::rhi {

class Device;

class Allocator {
public:
    Allocator() = default;
    ~Allocator();
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    // CALLER: ENGINE.
    [[nodiscard]] bool Init(Device& device);
    // CALLER: ENGINE.
    void Deinit();

    // Per-frame bump ring (transient data). CALLER: ENGINE (frame globals/UBOs).
    void* BumpAllocate(uint32_t bytes, uint32_t align, Memory mem,
                       uint32_t* out_offset = nullptr);
    Handle<Buffer> BumpMasterBuffer(Memory mem) const;

    // Minimum dynamic-UBO / SSBO offset alignment. CALLER: ENGINE, RESOURCES.
    uint32_t UboAlign() const;
    uint32_t StorageAlign() const;

    // Advance the bump ring (retire + begin). CALLER: RESOURCES (AdvanceFrame).
    void AdvanceFrame(uint32_t frame_index);

    // MemoryAllocator + alignments; Resources walks plat.memory_ during
    // create/destroy.
    AllocatorPlat plat;

private:
    bool inited_ = false;
};

}  // namespace cairns::rhi
