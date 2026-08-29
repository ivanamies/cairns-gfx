// rhi/metal/memory_allocator.hpp
//
// PRIVATE to the Metal backend. Not exposed through rhi/resource_manager.hpp.
//
// Bump path: ONE MTL::Heap + ONE MTL::Buffer created at Init, never recreated.
// Memory types (kUpload/kDynamic/kReadback) share regions inside the single
// master buffer; kFramesInFlight slots are subregions inside each memory-type
// region. BumpAllocate is pure offset arithmetic. blocks_[0] is the bump
// heap; blocks_[1+] hold per-resource heaps for AllocBuffer/AllocImage which
// still need variable-lifetime OffsetAllocator sub-allocation.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <array>
#include <vector>

#include "rhi/resource_manager.hpp"
#include "util/alloc_tags.hpp"
#include "util/offset_allocator.hpp"
#include "util/print_allocator.hpp"

namespace MTL {
class Device;
class Heap;
class Buffer;
class Texture;
}  // namespace MTL

namespace cairns::rhi::metal {

inline constexpr uint32_t kInvalidBlock = 0xFFFFFFFFu;
inline constexpr size_t kMemoryCount = static_cast<size_t>(Memory::kCount);

inline constexpr size_t mem_index(Memory mem) {
    return static_cast<size_t>(mem);
}

struct HeapBlock {
    MTL::Heap* heap = nullptr;
    MTL::Buffer* master_buffer = nullptr;  // null for image pools
    void* mapped_ptr = nullptr;            // null if private/device-only
    uint64_t gpu_address = 0;
    OffsetAllocator::Allocator offset_alloc;
    uint32_t size_bytes = 0;
    Memory mem_type = Memory::kDefault;
    bool is_image_pool = false;
};

// Bump region layout inside the single master buffer.
// region_base[m] + slot * slot_size[m] is the slot's start offset; cursors
// advance inside [0, slot_size[m]).
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
    MTL::Texture* texture = nullptr;
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

    bool Init(MTL::Device* device);
    void Deinit();

    // Persistent allocations.
    AllocResult AllocBuffer(uint32_t bytes, BufferUsage usage, Memory mem,
                            uint32_t align);
    AllocResult AllocImage(uint32_t bytes, uint32_t align, Memory mem);

    void FreeBuffer(uint32_t heap_index, OffsetAllocator::Allocation alloc,
                    uint32_t retire_frame);
    void FreeImage(uint32_t heap_index, OffsetAllocator::Allocation alloc,
                   MTL::Texture* texture, uint32_t retire_frame);

    // Per-frame bump. Offset returned is ABSOLUTE inside the single master
    // buffer (region_base + slot*slot_size + cursor). BumpMasterHeapIndex
    // always returns kBumpHeapIndex regardless of mem -- one buffer for all.
    void* BumpAllocate(uint32_t bytes, uint32_t align, Memory mem,
                       uint32_t* out_offset = nullptr);
    uint32_t BumpMasterHeapIndex(Memory mem) const;
    uint32_t BumpRingBytes(Memory mem) const;
    uint32_t BumpSaveCursor(Memory mem) const;
    void BumpRestoreCursor(Memory mem, uint32_t cursor);

    // Backend access (works for both the bump heap [index 0] and per-resource
    // heaps [index 1+] uniformly).
    MTL::Buffer* HeapMasterBuffer(uint32_t heap_index) const;
    MTL::Heap* HeapHandle(uint32_t heap_index) const;
    void* HeapMappedPtr(uint32_t heap_index) const;
    uint64_t HeapGpuAddress(uint32_t heap_index) const;

    // Frame lifecycle.
    void BeginFrame(uint32_t frame_index);
    void RetireFrame(uint32_t frame_slot);

    // The single bump heap lives at this fixed index in blocks_.
    static constexpr uint32_t kBumpHeapIndex = 0;

private:
    bool CreateBumpHeap();
    bool CreateBufferBlock(uint32_t bytes, Memory mem, uint32_t* out_index);
    bool CreateImageBlock(uint32_t bytes, Memory mem, uint32_t* out_index);
    void DestroyBlock(uint32_t heap_index);

    MTL::Device* device_ = nullptr;
    bool initialized_ = false;

    std::vector<HeapBlock,
                cairns::print_allocator<HeapBlock, cairns::tags::MemAllocBlocks>>
        blocks_;

    std::vector<uint32_t,
                cairns::print_allocator<uint32_t,
                                        cairns::tags::MemAllocBufferPool>>
        buffer_pools_[kMemoryCount];
    std::vector<uint32_t,
                cairns::print_allocator<uint32_t,
                                        cairns::tags::MemAllocImagePool>>
        image_pools_[kMemoryCount];

    BumpLayout bump_{};

    std::vector<PendingFree,
                cairns::print_allocator<PendingFree,
                                        cairns::tags::MemAllocPendingFree>>
        pending_frees_[kFramesInFlight];
};

}  // namespace cairns::rhi::metal

#endif  // CAIRNS_METAL
