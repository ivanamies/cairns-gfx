// rhi/vulkan/memory_allocator.cpp

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include "rhi/vulkan/memory_allocator.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

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

    bump_.slot_size[static_cast<size_t>(Memory::kUpload)]   = 64u * 1024u * 1024u;
    // #221 Phase 3: 16 -> 32 MB. Palettes alone are ~12 MB at the v7 3000-
    // skinned-actor target (3000 * 64 joints * sizeof(mat4) = 12 MB);
    // existing UBO traffic + InstanceMeta + Params + headroom needs the
    // doubling. Host-visible memory cost: +16 MB / slot * kFramesInFlight.
    bump_.slot_size[static_cast<size_t>(Memory::kDynamic)]  = 32u * 1024u * 1024u;
    bump_.slot_size[static_cast<size_t>(Memory::kReadback)] =  8u * 1024u * 1024u;

    uint32_t running = 0;
    for (size_t m = 0; m < kMemoryCount; ++m) {
        bump_.region_base[m] = running;
        running += bump_.slot_size[m] * kFramesInFlight;
    }

    return CreateBumpHeap();
}

bool MemoryAllocator::CreateBumpHeap() {
    // ONE VkDeviceMemory + ONE VkBuffer at Init, never recreated.
    // Memory props = HOST_VISIBLE | HOST_COHERENT covers Upload + Dynamic;
    // Readback also lives here (slightly slower CPU reads than HOST_CACHED
    // but functionally fine for small per-frame readback).
    uint32_t total = 0;
    for (size_t m = 0; m < kMemoryCount; ++m) {
        total += bump_.slot_size[m] * kFramesInFlight;
    }
    if (total == 0) {
        return false;
    }

    VkBufferUsageFlags all_usage =
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = total;
    bci.usage = all_usage;
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

    uint32_t type_idx = 0;
    if (!PickMemoryType(req.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        0, &type_idx)) {
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
    vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, &mapped);

    // Defensive: paint freshly-allocated bytes with 0xCC. Real init happens
    // at first use (UploadBuffer / LoadOp::kClear); pre-init reads surface
    // as 0xCCCCCCCC instead of silently working off a coincidentally-good
    // value. See metal allocator for the long-form rationale.
    if (mapped) {
        const uint8_t pat = std::getenv("CAIRNS_HEAP_ZERO") ? 0x00 : 0xCC;
        std::memset(mapped, pat, total);
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
    blk.size_bytes = total;
    blk.memory_type_index = type_idx;
    blk.mem_type = Memory::kDynamic;
    blk.is_image_pool = false;

    blocks_.push_back(std::move(blk));
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
        // Garbage-init -- see metal allocator + the CreateBumpHeap comment
        // for the long-form rationale.
        if (mapped) {
            const uint8_t pat = std::getenv("CAIRNS_HEAP_ZERO") ? 0x00 : 0xCC;
            std::memset(mapped, pat, bytes);
        }
    }
    // DEVICE_LOCAL-only memory cannot be CPU-memset and the MemoryAllocator
    // doesn't currently own a queue + command pool to issue a
    // vkCmdFillBuffer. On Apple (MoltenVK / unified memory) this branch is
    // not taken -- HOST_VISIBLE memory typically IS device-local. On a
    // discrete-GPU Vulkan path we'd need to extend Init to take the
    // graphics queue + a one-shot command pool. Left as TODO; the
    // symptom-on-leak guarantee only holds for HOST_VISIBLE-backed
    // allocations until then.

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
                               ? padded + (padded >> 3)
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

void* MemoryAllocator::BumpAllocate(uint32_t bytes, uint32_t align, Memory mem,
                                   uint32_t* out_offset) {
    const size_t mi = static_cast<size_t>(mem);
    const uint32_t slot_size = bump_.slot_size[mi];
    if (slot_size == 0) {
        return nullptr;
    }

    const uint32_t slot = bump_.current_slot;
    const uint32_t cursor_local = align_up(bump_.cursors[mi][slot], align);
    if (cursor_local + bytes > slot_size) {
        return nullptr;
    }
    bump_.cursors[mi][slot] = cursor_local + bytes;

    const uint32_t offset_in_buffer =
        bump_.region_base[mi] + slot * slot_size + cursor_local;
    if (out_offset) {
        *out_offset = offset_in_buffer;
    }
    return static_cast<uint8_t*>(blocks_[kBumpHeapIndex].mapped_ptr) +
           offset_in_buffer;
}

uint32_t MemoryAllocator::BumpRingBytes(Memory mem) const {
    return bump_.slot_size[static_cast<size_t>(mem)];
}

uint32_t MemoryAllocator::BumpSaveCursor(Memory mem) const {
    return bump_.cursors[static_cast<size_t>(mem)][bump_.current_slot];
}

void MemoryAllocator::BumpRestoreCursor(Memory mem, uint32_t cursor) {
    bump_.cursors[static_cast<size_t>(mem)][bump_.current_slot] = cursor;
}

uint32_t MemoryAllocator::BumpMasterHeapIndex(Memory /*mem*/) const {
    return kBumpHeapIndex;
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
    bump_.current_slot = frame_index % kFramesInFlight;
    for (size_t m = 0; m < kMemoryCount; ++m) {
        bump_.cursors[m][bump_.current_slot] = 0;
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
