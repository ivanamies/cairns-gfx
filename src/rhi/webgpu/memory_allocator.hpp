// rhi/webgpu/memory_allocator.hpp
//
// PRIVATE to the WebGPU backend. Mirrors the metal allocator's contract: ONE
// master WGPUBuffer for the bump path (kUpload/kDynamic/kReadback share regions,
// kFramesInFlight slots inside each), blocks_[1+] hold per-resource heaps.
#pragma once

#include "util/define.hpp"

#if CAIRNS_WEBGPU

#include <array>
#include <vector>

#include <webgpu/webgpu.h>

#include "rhi/resource_manager.hpp"
#include "util/offset_allocator.hpp"

namespace cairns::rhi::webgpu {

inline constexpr uint32_t kInvalidBlock = 0xFFFFFFFFu;
inline constexpr size_t kMemoryCount = static_cast<size_t>(Memory::kCount);

inline constexpr size_t mem_index(Memory mem) {
    return static_cast<size_t>(mem);
}

struct HeapBlock {
    WGPUBuffer master_buffer = nullptr;  // null for image pools
    void* mapped_ptr = nullptr;          // null if device-only
    uint32_t size_bytes = 0;
    Memory mem_type = Memory::kDefault;
    bool is_image_pool = false;
};

struct BumpLayout {
    std::array<uint32_t, kMemoryCount> region_base{};
    std::array<uint32_t, kMemoryCount> slot_size{};
    uint32_t cursors[kMemoryCount][kFramesInFlight]{};
    uint32_t current_slot = 0;
};

struct PendingFree {
    uint32_t heap_index = 0;
    OffsetAllocator::Allocation alloc;
    bool is_image = false;
    WGPUTexture texture = nullptr;
};

struct AllocResult {
    uint32_t heap_index = kInvalidBlock;
    OffsetAllocator::Allocation alloc;  // raw, used for Free
    uint32_t offset = 0;                // alignment-corrected, used for binding
    bool ok = false;
};

class MemoryAllocator {
public:
    MemoryAllocator() = default;
    ~MemoryAllocator();

    MemoryAllocator(const MemoryAllocator&) = delete;
    MemoryAllocator& operator=(const MemoryAllocator&) = delete;

    bool Init(WGPUDevice device);
    void Deinit();

    AllocResult AllocBuffer(uint32_t bytes, BufferUsage usage, Memory mem,
                            uint32_t align);
    AllocResult AllocImage(uint32_t bytes, uint32_t align, Memory mem);

    void FreeBuffer(uint32_t heap_index, OffsetAllocator::Allocation alloc,
                    uint32_t retire_frame);
    void FreeImage(uint32_t heap_index, OffsetAllocator::Allocation alloc,
                   WGPUTexture texture, uint32_t retire_frame);

    void* BumpAllocate(uint32_t bytes, uint32_t align, Memory mem,
                       uint32_t* out_offset = nullptr);
    uint32_t BumpMasterHeapIndex(Memory mem) const;
    uint32_t BumpRingBytes(Memory mem) const;
    uint32_t BumpSaveCursor(Memory mem) const;
    void BumpRestoreCursor(Memory mem, uint32_t cursor);

    WGPUBuffer HeapMasterBuffer(uint32_t heap_index) const;
    void* HeapMappedPtr(uint32_t heap_index) const;

    void BeginFrame(uint32_t frame_index);
    void RetireFrame(uint32_t frame_slot);

    // Upload the current frame slot's written bump-ring bytes (per Memory) from
    // the CPU mirror to the GPU master buffer. WebGPU has no persistent host
    // mapping, so this runs once per frame before submit -- without it the
    // globals/drawtmp dynamic UBOs reach the GPU as zeros.
    void FlushBumpRing(WGPUQueue queue);

    static constexpr uint32_t kBumpHeapIndex = 0;

private:
    WGPUDevice device_ = nullptr;
    bool initialized_ = false;
    std::vector<HeapBlock> blocks_;
    BumpLayout bump_{};
    std::vector<PendingFree> pending_frees_[kFramesInFlight];
};

}  // namespace cairns::rhi::webgpu

#endif  // CAIRNS_WEBGPU
