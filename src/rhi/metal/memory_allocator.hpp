// rhi/metal/memory_allocator.hpp
//
// PRIVATE to the Metal backend. Not exposed through rhi/resource_manager.hpp.
//
// Manages 128 MB heap blocks via MTLHeapTypePlacement so the OffsetAllocator
// drives sub-allocation offsets (same model as the Vulkan backend). Each
// buffer-heap block also exposes a master MTLBuffer covering its full range
// (the "one platform buffer per heap block" trick). Image blocks place
// MTLTextures into the heap at sub-allocation offsets.
//
// On Apple Silicon (UMA) kUpload / kDynamic / kReadback use StorageModeShared.
// kTransient maps to StorageModeMemoryless for tile-only render targets.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <array>
#include <vector>

#include "rhi/resource_manager.hpp"
#include "util/offset_allocator.hpp"

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

struct BumpRing {
    std::array<uint32_t, ResourceManager::kFramesInFlight> block_indices{};
    std::array<uint32_t, ResourceManager::kFramesInFlight> cursors{};
    uint32_t current_slot = 0;
    uint32_t block_bytes = 0;
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

    // Persistent allocations.
    AllocResult AllocBuffer(uint32_t bytes, BufferUsage usage, Memory mem,
                            uint32_t align);
    AllocResult AllocImage(uint32_t bytes, uint32_t align, Memory mem);

    void FreeBuffer(uint32_t heap_index, OffsetAllocator::Allocation alloc,
                    uint32_t retire_frame);
    void FreeImage(uint32_t heap_index, OffsetAllocator::Allocation alloc,
                   MTL::Texture* texture, uint32_t retire_frame);

    // Per-frame bump ring.
    void* BumpAllocate(uint32_t bytes, uint32_t align, Memory mem);
    uint32_t BumpOffset(void* ptr) const;
    uint32_t BumpMasterHeapIndex(Memory mem) const;

    // Backend access.
    MTL::Buffer* HeapMasterBuffer(uint32_t heap_index) const;
    MTL::Heap* HeapHandle(uint32_t heap_index) const;
    void* HeapMappedPtr(uint32_t heap_index) const;
    uint64_t HeapGpuAddress(uint32_t heap_index) const;

    // Frame lifecycle.
    void BeginFrame(uint32_t frame_index);
    void RetireFrame(uint32_t frame_slot);

private:
    bool CreateBufferBlock(uint32_t bytes, Memory mem, uint32_t* out_index);
    bool CreateImageBlock(uint32_t bytes, Memory mem, uint32_t* out_index);
    void DestroyBlock(uint32_t heap_index);

    MTL::Device* device_ = nullptr;
    bool initialized_ = false;

    std::vector<HeapBlock> blocks_;

    std::vector<uint32_t> buffer_pools_[kMemoryCount];
    std::vector<uint32_t> image_pools_[kMemoryCount];

    BumpRing rings_[kMemoryCount]{};

    std::vector<PendingFree> pending_frees_[ResourceManager::kFramesInFlight];
};

}  // namespace cairns::rhi::metal

#endif  // CAIRNS_METAL
