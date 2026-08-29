// rhi/vulkan/memory_allocator.hpp
//
// PRIVATE to the Vulkan backend. Not exposed through rhi/resource_manager.hpp.
//
// Manages 128 MB heap blocks. Each block is one VkDeviceMemory + one master
// VkBuffer covering its entire range (or, for image pools, one VkDeviceMemory
// to which individual VkImages are bound at offsets).
//
// Per-Memory bump rings hand out CPU-writable pointers that map directly into
// host-visible memory.

#pragma once

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include <array>
#include <vector>

#include <vulkan/vulkan.h>

#include "rhi/resource_manager.hpp"
#include "util/offset_allocator.hpp"

namespace cairns::rhi::vulkan {

inline constexpr uint32_t kInvalidBlock = 0xFFFFFFFFu;
inline constexpr size_t kMemoryCount = static_cast<size_t>(Memory::kCount);

struct HeapBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer master_buffer = VK_NULL_HANDLE;  // null for image pools
    void* mapped_ptr = nullptr;               // null if device-only
    uint64_t device_address = 0;              // BDA (if enabled)
    OffsetAllocator::Allocator offset_alloc;
    uint32_t size_bytes = 0;
    uint32_t memory_type_index = 0;
    Memory mem_type = Memory::kDefault;
    bool is_image_pool = false;
};

struct BumpRing {
    std::array<uint32_t, kFramesInFlight> block_indices{};
    std::array<uint32_t, kFramesInFlight> cursors{};
    uint32_t current_slot = 0;
    uint32_t block_bytes = 0;
};

struct PendingFree {
    uint32_t heap_index = 0;
    OffsetAllocator::Allocation alloc;
    bool is_image = false;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
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

    bool Init(VkDevice device, VkPhysicalDevice phys, bool enable_bda);
    // Frees all heaps/blocks. Idempotent; the dtor calls it. Must run while the
    // VkDevice is still alive (Allocator::Deinit calls it before device teardown).
    void Deinit();

    // Persistent allocations.
    AllocResult AllocBuffer(uint32_t bytes, BufferUsage usage, Memory mem,
                            uint32_t align);
    AllocResult AllocImage(uint32_t bytes, uint32_t align,
                           uint32_t memory_type_bits, Memory mem);

    void FreeBuffer(uint32_t heap_index, OffsetAllocator::Allocation alloc,
                    uint32_t retire_frame);
    void FreeImage(uint32_t heap_index, OffsetAllocator::Allocation alloc,
                   VkImage image, VkImageView view, uint32_t retire_frame);

    // Per-frame bump ring.
    void* BumpAllocate(uint32_t bytes, uint32_t align, Memory mem,
                       uint32_t* out_offset = nullptr);
    uint32_t BumpMasterHeapIndex(Memory mem) const;

    // Save/restore the current bump cursor. Used to reclaim transient staging
    // memory after a synchronous upload so it does not accumulate.
    uint32_t BumpSaveCursor(Memory mem) const;
    void BumpRestoreCursor(Memory mem, uint32_t cursor);

    // Backend access.
    VkBuffer HeapMasterBuffer(uint32_t heap_index) const;
    VkDeviceMemory HeapDeviceMemory(uint32_t heap_index) const;
    void* HeapMappedPtr(uint32_t heap_index) const;
    uint64_t HeapDeviceAddr(uint32_t heap_index) const;

    // Frame lifecycle.
    void BeginFrame(uint32_t frame_index);
    void RetireFrame(uint32_t frame_slot);

private:
    bool CreateBufferBlock(uint32_t bytes, BufferUsage usage, Memory mem,
                           uint32_t* out_index);
    bool CreateImageBlock(uint32_t bytes, uint32_t memory_type_bits, Memory mem,
                          uint32_t* out_index);
    void DestroyBlock(uint32_t heap_index);

    bool PickMemoryType(uint32_t type_bits_allowed,
                        VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred, uint32_t* out) const;
    VkMemoryPropertyFlags PropsFor(Memory mem) const;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props_{};
    bool bda_enabled_ = false;

    std::vector<HeapBlock> blocks_;

    std::vector<uint32_t> buffer_pools_[kMemoryCount];
    std::vector<uint32_t> image_pools_[kMemoryCount];

    BumpRing rings_[kMemoryCount]{};

    std::vector<PendingFree> pending_frees_[kFramesInFlight];
};

}  // namespace cairns::rhi::vulkan

#endif  // CAIRNS_VULKAN
