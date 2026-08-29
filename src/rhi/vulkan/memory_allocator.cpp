// rhi/vulkan/memory_allocator.cpp

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include "rhi/vulkan/memory_allocator.hpp"

#include <cstdint>

namespace cairns::rhi::vulkan {

namespace {

constexpr uint32_t align_up(uint32_t v, uint32_t a) {
    if (a == 0) {
        return v;
    }
    return (v + a - 1) & ~(a - 1);
}

VkBufferUsageFlags to_vk_buffer_usage(BufferUsage u) {
    VkBufferUsageFlags f = 0;
    if (u & kUsageVertex) {
        f |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (u & kUsageIndex) {
        f |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (u & kUsageUniform) {
        f |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (u & kUsageStorage) {
        f |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (u & kUsageIndirect) {
        f |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    if (u & kUsageTransferSrc) {
        f |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (u & kUsageTransferDst) {
        f |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    return f;
}

}  // namespace

MemoryAllocator::~MemoryAllocator() { Deinit(); }

void MemoryAllocator::Deinit() {
    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
        RetireFrame(slot);
    }
    for (uint32_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].memory != VK_NULL_HANDLE) {
            DestroyBlock(i);
        }
    }
}

bool MemoryAllocator::Init(VkDevice device, VkPhysicalDevice phys,
                           bool enable_bda) {
    device_ = device;
    physical_ = phys;
    bda_enabled_ = enable_bda;
    vkGetPhysicalDeviceMemoryProperties(physical_, &mem_props_);

    for (size_t m = 0; m < kMemoryCount; ++m) {
        rings_[m].block_indices.fill(kInvalidBlock);
        rings_[m].cursors.fill(0);
    }

    rings_[static_cast<size_t>(Memory::kUpload)].block_bytes =
        64u * 1024u * 1024u;
    rings_[static_cast<size_t>(Memory::kDynamic)].block_bytes =
        16u * 1024u * 1024u;
    rings_[static_cast<size_t>(Memory::kReadback)].block_bytes =
        8u * 1024u * 1024u;
    return true;
}

VkMemoryPropertyFlags MemoryAllocator::PropsFor(Memory mem) const {
    switch (mem) {
        case Memory::kDefault:
            return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        case Memory::kUpload:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        case Memory::kReadback:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        case Memory::kDynamic:
            return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        case Memory::kTransient:
            return VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        default:
            return 0;
    }
}

bool MemoryAllocator::PickMemoryType(uint32_t type_bits_allowed,
                                     VkMemoryPropertyFlags required,
                                     VkMemoryPropertyFlags preferred,
                                     uint32_t* out) const {
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if (!(type_bits_allowed & (1u << i))) {
            continue;
        }
        VkMemoryPropertyFlags f = mem_props_.memoryTypes[i].propertyFlags;
        if ((f & (required | preferred)) == (required | preferred)) {
            *out = i;
            return true;
        }
    }
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
        if (!(type_bits_allowed & (1u << i))) {
            continue;
        }
        VkMemoryPropertyFlags f = mem_props_.memoryTypes[i].propertyFlags;
        if ((f & required) == required) {
            *out = i;
            return true;
        }
    }
    return false;
}

bool MemoryAllocator::CreateBufferBlock(uint32_t bytes, BufferUsage usage,
                                        Memory mem, uint32_t* out_index) {
    // CODE SMELL (revisit): the master buffer carries the union of every buffer
    // usage because one block is shared by sub-allocations with differing
    // usages. Consider per-usage pools or per-sub-alloc buffers later.
    VkBufferUsageFlags all_usage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = to_vk_buffer_usage(usage) | all_usage;
    if (bda_enabled_) {
        bci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device_, &bci, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buffer, &req);

    VkMemoryPropertyFlags required = PropsFor(mem);
    VkMemoryPropertyFlags preferred = 0;
    if (mem == Memory::kDynamic) {
        preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    }

    uint32_t type_idx = 0;
    if (!PickMemoryType(req.memoryTypeBits, required, preferred, &type_idx)) {
        vkDestroyBuffer(device_, buffer, nullptr);
        return false;
    }

    VkMemoryAllocateFlagsInfo flags_info{};
    flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags_info.flags =
        bda_enabled_ ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    if (bda_enabled_) {
        mai.pNext = &flags_info;
    }
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type_idx;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &mai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device_, buffer, nullptr);
        return false;
    }
    vkBindBufferMemory(device_, buffer, memory, 0);

    void* mapped = nullptr;
    VkMemoryPropertyFlags prop_flags =
        mem_props_.memoryTypes[type_idx].propertyFlags;
    if (prop_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    }

    uint64_t bda = 0;
    if (bda_enabled_) {
        VkBufferDeviceAddressInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        info.buffer = buffer;
        bda = vkGetBufferDeviceAddress(device_, &info);
    }

    HeapBlock blk;
    blk.memory = memory;
    blk.master_buffer = buffer;
    blk.mapped_ptr = mapped;
    blk.device_address = bda;
    blk.offset_alloc = OffsetAllocator::Allocator(bytes);
    blk.size_bytes = bytes;
    blk.memory_type_index = type_idx;
    blk.mem_type = mem;
    blk.is_image_pool = false;

    blocks_.push_back(std::move(blk));
    *out_index = static_cast<uint32_t>(blocks_.size()) - 1;
    return true;
}

bool MemoryAllocator::CreateImageBlock(uint32_t bytes, uint32_t memory_type_bits,
                                       Memory mem, uint32_t* out_index) {
    VkMemoryPropertyFlags required = PropsFor(mem);
    uint32_t type_idx = 0;
    if (!PickMemoryType(memory_type_bits, required, 0, &type_idx)) {
        return false;
    }

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = bytes;
    mai.memoryTypeIndex = type_idx;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device_, &mai, nullptr, &memory) != VK_SUCCESS) {
        return false;
    }

    HeapBlock blk;
    blk.memory = memory;
    blk.master_buffer = VK_NULL_HANDLE;
    blk.mapped_ptr = nullptr;
    blk.offset_alloc = OffsetAllocator::Allocator(bytes);
    blk.size_bytes = bytes;
    blk.memory_type_index = type_idx;
    blk.mem_type = mem;
    blk.is_image_pool = true;

    blocks_.push_back(std::move(blk));
    *out_index = static_cast<uint32_t>(blocks_.size()) - 1;
    return true;
}

void MemoryAllocator::DestroyBlock(uint32_t heap_index) {
    HeapBlock& b = blocks_[heap_index];
    if (b.master_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, b.master_buffer, nullptr);
    }
    if (b.mapped_ptr) {
        vkUnmapMemory(device_, b.memory);
    }
    if (b.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, b.memory, nullptr);
    }
    b = HeapBlock{};
}

AllocResult MemoryAllocator::AllocBuffer(uint32_t bytes, BufferUsage usage,
                                         Memory mem, uint32_t align) {
    uint32_t padded = bytes + (align ? align - 1 : 0);
    std::vector<uint32_t>& pool = buffer_pools_[static_cast<size_t>(mem)];

    for (uint32_t hi : pool) {
        OffsetAllocator::Allocation a = blocks_[hi].offset_alloc.allocate(padded);
        if (a.offset != OffsetAllocator::Allocation::NO_SPACE) {
            return {hi, a, align_up(a.offset, align), true};
        }
    }

    uint32_t block_bytes = (padded > kHeapBlockBytes)
                               ? padded
                               : kHeapBlockBytes;
    uint32_t hi = kInvalidBlock;
    if (!CreateBufferBlock(block_bytes, usage, mem, &hi)) {
        return {};
    }
    pool.push_back(hi);
    OffsetAllocator::Allocation a = blocks_[hi].offset_alloc.allocate(padded);
    if (a.offset == OffsetAllocator::Allocation::NO_SPACE) {
        return {};
    }
    return {hi, a, align_up(a.offset, align), true};
}

AllocResult MemoryAllocator::AllocImage(uint32_t bytes, uint32_t align,
                                        uint32_t memory_type_bits, Memory mem) {
    uint32_t padded = bytes + (align ? align - 1 : 0);
    std::vector<uint32_t>& pool = image_pools_[static_cast<size_t>(mem)];

    for (uint32_t hi : pool) {
        HeapBlock& b = blocks_[hi];
        if (!(memory_type_bits & (1u << b.memory_type_index))) {
            continue;
        }
        OffsetAllocator::Allocation a = b.offset_alloc.allocate(padded);
        if (a.offset != OffsetAllocator::Allocation::NO_SPACE) {
            return {hi, a, align_up(a.offset, align), true};
        }
    }

    uint32_t block_bytes = (padded > kHeapBlockBytes)
                               ? padded
                               : kHeapBlockBytes;
    uint32_t hi = kInvalidBlock;
    if (!CreateImageBlock(block_bytes, memory_type_bits, mem, &hi)) {
        return {};
    }
    pool.push_back(hi);
    OffsetAllocator::Allocation a = blocks_[hi].offset_alloc.allocate(padded);
    if (a.offset == OffsetAllocator::Allocation::NO_SPACE) {
        return {};
    }
    return {hi, a, align_up(a.offset, align), true};
}

void MemoryAllocator::FreeBuffer(uint32_t heap_index,
                                 OffsetAllocator::Allocation alloc,
                                 uint32_t retire_frame) {
    PendingFree p;
    p.heap_index = heap_index;
    p.alloc = alloc;
    p.is_image = false;
    pending_frees_[retire_frame % kFramesInFlight].push_back(p);
}

void MemoryAllocator::FreeImage(uint32_t heap_index,
                                OffsetAllocator::Allocation alloc, VkImage image,
                                VkImageView view, uint32_t retire_frame) {
    PendingFree p;
    p.heap_index = heap_index;
    p.alloc = alloc;
    p.is_image = true;
    p.image = image;
    p.view = view;
    pending_frees_[retire_frame % kFramesInFlight].push_back(p);
}

void* MemoryAllocator::BumpAllocate(uint32_t bytes, uint32_t align, Memory mem) {
    BumpRing& r = rings_[static_cast<size_t>(mem)];
    if (r.block_bytes == 0) {
        return nullptr;
    }

    uint32_t slot = r.current_slot;
    if (r.block_indices[slot] == kInvalidBlock) {
        BufferUsage u = kUsageUniform | kUsageStorage | kUsageVertex |
                        kUsageIndex | kUsageTransferSrc;
        uint32_t hi = kInvalidBlock;
        if (!CreateBufferBlock(r.block_bytes, u, mem, &hi)) {
            return nullptr;
        }
        r.block_indices[slot] = hi;
    }

    uint32_t off = align_up(r.cursors[slot], align);
    if (off + bytes > r.block_bytes) {
        return nullptr;
    }
    r.cursors[slot] = off + bytes;

    HeapBlock& blk = blocks_[r.block_indices[slot]];
    return static_cast<uint8_t*>(blk.mapped_ptr) + off;
}

uint32_t MemoryAllocator::BumpSaveCursor(Memory mem) const {
    const BumpRing& r = rings_[static_cast<size_t>(mem)];
    return r.cursors[r.current_slot];
}

void MemoryAllocator::BumpRestoreCursor(Memory mem, uint32_t cursor) {
    BumpRing& r = rings_[static_cast<size_t>(mem)];
    r.cursors[r.current_slot] = cursor;
}

uint32_t MemoryAllocator::BumpOffset(void* ptr) const {
    for (uint32_t i = 0; i < blocks_.size(); ++i) {
        const HeapBlock& b = blocks_[i];
        if (!b.mapped_ptr) {
            continue;
        }
        uint8_t* base = static_cast<uint8_t*>(b.mapped_ptr);
        uint8_t* p = static_cast<uint8_t*>(ptr);
        if (p >= base && p < base + b.size_bytes) {
            return static_cast<uint32_t>(p - base);
        }
    }
    return 0xFFFFFFFFu;
}

uint32_t MemoryAllocator::BumpMasterHeapIndex(Memory mem) const {
    const BumpRing& r = rings_[static_cast<size_t>(mem)];
    return r.block_indices[r.current_slot];
}

VkBuffer MemoryAllocator::HeapMasterBuffer(uint32_t heap_index) const {
    return blocks_[heap_index].master_buffer;
}

VkDeviceMemory MemoryAllocator::HeapDeviceMemory(uint32_t heap_index) const {
    return blocks_[heap_index].memory;
}

void* MemoryAllocator::HeapMappedPtr(uint32_t heap_index) const {
    return blocks_[heap_index].mapped_ptr;
}

uint64_t MemoryAllocator::HeapDeviceAddr(uint32_t heap_index) const {
    return blocks_[heap_index].device_address;
}

void MemoryAllocator::BeginFrame(uint32_t frame_index) {
    for (size_t m = 0; m < kMemoryCount; ++m) {
        BumpRing& r = rings_[m];
        if (r.block_bytes == 0) {
            continue;
        }
        r.current_slot = frame_index % kFramesInFlight;
        r.cursors[r.current_slot] = 0;
    }
}

void MemoryAllocator::RetireFrame(uint32_t frame_slot) {
    std::vector<PendingFree>& pending = pending_frees_[frame_slot];
    for (PendingFree& p : pending) {
        if (p.is_image) {
            if (p.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, p.view, nullptr);
            }
            if (p.image != VK_NULL_HANDLE) {
                vkDestroyImage(device_, p.image, nullptr);
            }
        }
        blocks_[p.heap_index].offset_alloc.free(p.alloc);
    }
    pending.clear();
}

}  // namespace cairns::rhi::vulkan

#endif  // CAIRNS_VULKAN
