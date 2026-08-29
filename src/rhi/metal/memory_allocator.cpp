// rhi/metal/memory_allocator.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi/metal/memory_allocator.hpp"

#include <Metal/Metal.hpp>

#include <cassert>
#include <utility>

namespace cairns::rhi::metal {

namespace {

uint32_t align_up(uint32_t v, uint32_t a) {
    if (a == 0) {
        return v;
    }
    return (v + a - 1) & ~(a - 1);
}

MTL::StorageMode storage_mode_for(Memory mem) {
    switch (mem) {
        case Memory::kDefault: return MTL::StorageModePrivate;
        case Memory::kUpload: return MTL::StorageModeShared;
        case Memory::kReadback: return MTL::StorageModeShared;
        case Memory::kDynamic: return MTL::StorageModeShared;
        case Memory::kTransient: return MTL::StorageModeMemoryless;
        default: return MTL::StorageModePrivate;
    }
}

MTL::ResourceOptions options_for(Memory mem) {
    MTL::ResourceOptions o = MTL::ResourceStorageModePrivate;
    switch (mem) {
        case Memory::kDefault:
            o = MTL::ResourceStorageModePrivate;
            break;
        case Memory::kUpload:
            o = MTL::ResourceStorageModeShared |
                MTL::ResourceCPUCacheModeWriteCombined;
            break;
        case Memory::kReadback:
            o = MTL::ResourceStorageModeShared;
            break;
        case Memory::kDynamic:
            o = MTL::ResourceStorageModeShared;
            break;
        case Memory::kTransient:
            o = MTL::ResourceStorageModeMemoryless;
            break;
        default:
            o = MTL::ResourceStorageModePrivate;
            break;
    }
    o |= MTL::ResourceHazardTrackingModeUntracked;
    return o;
}

}  // namespace

MemoryAllocator::~MemoryAllocator() { Deinit(); }

void MemoryAllocator::Deinit() {
    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
        RetireFrame(slot);
    }
    for (uint32_t i = 0; i < blocks_.size(); ++i) {
        if (blocks_[i].heap) {
            DestroyBlock(i);
        }
    }
}

bool MemoryAllocator::Init(MTL::Device* device) {
    if (initialized_) {
        return false;
    }
    if (!device) {
        return false;
    }
    device_ = device;

    rings_[mem_index(Memory::kUpload)].block_bytes = 64u * 1024u * 1024u;
    rings_[mem_index(Memory::kDynamic)].block_bytes = 16u * 1024u * 1024u;
    rings_[mem_index(Memory::kReadback)].block_bytes = 8u * 1024u * 1024u;

    initialized_ = true;
    return true;
}

bool MemoryAllocator::CreateBufferBlock(uint32_t bytes, Memory mem,
                                        uint32_t* out_index) {
    MTL::HeapDescriptor* hd = MTL::HeapDescriptor::alloc()->init();
    hd->setSize(bytes);
    hd->setType(MTL::HeapTypePlacement);
    hd->setResourceOptions(options_for(mem));

    MTL::Heap* heap = device_->newHeap(hd);
    hd->release();
    if (!heap) {
        return false;
    }

    MTL::Buffer* master = heap->newBuffer(bytes, options_for(mem), 0);
    if (!master) {
        heap->release();
        return false;
    }

    HeapBlock blk;
    blk.heap = heap;
    blk.master_buffer = master;
    blk.mapped_ptr = (storage_mode_for(mem) == MTL::StorageModePrivate)
                         ? nullptr
                         : master->contents();
    blk.gpu_address = master->gpuAddress();
    blk.offset_alloc = OffsetAllocator::Allocator(bytes);
    blk.size_bytes = bytes;
    blk.mem_type = mem;
    blk.is_image_pool = false;

    blocks_.push_back(std::move(blk));
    *out_index = static_cast<uint32_t>(blocks_.size()) - 1u;
    return true;
}

bool MemoryAllocator::CreateImageBlock(uint32_t bytes, Memory mem,
                                       uint32_t* out_index) {
    MTL::HeapDescriptor* hd = MTL::HeapDescriptor::alloc()->init();
    hd->setSize(bytes);
    hd->setType(MTL::HeapTypePlacement);
    hd->setStorageMode(storage_mode_for(mem));
    hd->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);

    MTL::Heap* heap = device_->newHeap(hd);
    hd->release();
    if (!heap) {
        return false;
    }

    HeapBlock blk;
    blk.heap = heap;
    blk.master_buffer = nullptr;
    blk.mapped_ptr = nullptr;
    blk.gpu_address = 0;
    blk.offset_alloc = OffsetAllocator::Allocator(bytes);
    blk.size_bytes = bytes;
    blk.mem_type = mem;
    blk.is_image_pool = true;

    blocks_.push_back(std::move(blk));
    *out_index = static_cast<uint32_t>(blocks_.size()) - 1u;
    return true;
}

void MemoryAllocator::DestroyBlock(uint32_t heap_index) {
    HeapBlock& b = blocks_[heap_index];
    if (b.master_buffer) {
        b.master_buffer->release();
        b.master_buffer = nullptr;
    }
    if (b.heap) {
        b.heap->release();
        b.heap = nullptr;
    }
    b = {};
}

AllocResult MemoryAllocator::AllocBuffer(uint32_t bytes, BufferUsage,
                                         Memory mem, uint32_t align) {
    AllocResult out;
    const uint32_t padded = bytes + (align > 1 ? align - 1 : 0);

    std::vector<uint32_t>& pool = buffer_pools_[mem_index(mem)];
    for (uint32_t hi : pool) {
        OffsetAllocator::Allocation a = blocks_[hi].offset_alloc.allocate(padded);
        if (a.offset != OffsetAllocator::Allocation::NO_SPACE) {
            out.heap_index = hi;
            out.alloc = a;
            out.offset = align_up(a.offset, align);
            out.ok = true;
            return out;
        }
    }

    const uint32_t block_bytes = (padded > kHeapBlockBytes)
                                     ? padded
                                     : kHeapBlockBytes;
    uint32_t new_hi = kInvalidBlock;
    if (!CreateBufferBlock(block_bytes, mem, &new_hi)) {
        return out;
    }
    pool.push_back(new_hi);

    OffsetAllocator::Allocation a = blocks_[new_hi].offset_alloc.allocate(padded);
    if (a.offset == OffsetAllocator::Allocation::NO_SPACE) {
        return out;
    }
    out.heap_index = new_hi;
    out.alloc = a;
    out.offset = align_up(a.offset, align);
    out.ok = true;
    return out;
}

AllocResult MemoryAllocator::AllocImage(uint32_t bytes, uint32_t align,
                                        Memory mem) {
    AllocResult out;
    const uint32_t padded = bytes + (align > 1 ? align - 1 : 0);

    std::vector<uint32_t>& pool = image_pools_[mem_index(mem)];
    for (uint32_t hi : pool) {
        OffsetAllocator::Allocation a = blocks_[hi].offset_alloc.allocate(padded);
        if (a.offset != OffsetAllocator::Allocation::NO_SPACE) {
            out.heap_index = hi;
            out.alloc = a;
            out.offset = align_up(a.offset, align);
            out.ok = true;
            return out;
        }
    }

    const uint32_t block_bytes = (padded > kHeapBlockBytes)
                                     ? padded
                                     : kHeapBlockBytes;
    uint32_t new_hi = kInvalidBlock;
    if (!CreateImageBlock(block_bytes, mem, &new_hi)) {
        return out;
    }
    pool.push_back(new_hi);

    OffsetAllocator::Allocation a = blocks_[new_hi].offset_alloc.allocate(padded);
    if (a.offset == OffsetAllocator::Allocation::NO_SPACE) {
        return out;
    }
    out.heap_index = new_hi;
    out.alloc = a;
    out.offset = align_up(a.offset, align);
    out.ok = true;
    return out;
}

void MemoryAllocator::FreeBuffer(uint32_t heap_index,
                                 OffsetAllocator::Allocation alloc,
                                 uint32_t retire_frame) {
    PendingFree p;
    p.heap_index = heap_index;
    p.alloc = alloc;
    pending_frees_[retire_frame % kFramesInFlight].push_back(p);
}

void MemoryAllocator::FreeImage(uint32_t heap_index,
                                OffsetAllocator::Allocation alloc,
                                MTL::Texture* texture, uint32_t retire_frame) {
    PendingFree p;
    p.heap_index = heap_index;
    p.alloc = alloc;
    p.is_image = true;
    p.texture = texture;
    pending_frees_[retire_frame % kFramesInFlight].push_back(p);
}

void* MemoryAllocator::BumpAllocate(uint32_t bytes, uint32_t align, Memory mem,
                                   uint32_t* out_offset) {
    BumpRing& r = rings_[mem_index(mem)];
    assert(r.block_bytes != 0 && "bump ring not initialized");
    if (r.block_bytes == 0) {
        return nullptr;
    }

    const uint32_t slot = r.current_slot;
    const bool needs_block =
        blocks_.empty() || r.block_indices[slot] >= blocks_.size() ||
        blocks_[r.block_indices[slot]].master_buffer == nullptr ||
        blocks_[r.block_indices[slot]].size_bytes != r.block_bytes ||
        blocks_[r.block_indices[slot]].mem_type != mem;
    if (needs_block) {
        uint32_t new_hi = kInvalidBlock;
        bool block_ok = CreateBufferBlock(r.block_bytes, mem, &new_hi);
        assert(block_ok && "bump CreateBufferBlock failed");
        if (!block_ok) {
            return nullptr;
        }
        r.block_indices[slot] = new_hi;
    }

    const uint32_t off = align_up(r.cursors[slot], align);
    assert(off + bytes <= r.block_bytes && "bump ring overflow");
    if (off + bytes > r.block_bytes) {
        return nullptr;
    }
    r.cursors[slot] = off + bytes;
    if (out_offset) {
        *out_offset = off;
    }
    void* p = static_cast<uint8_t*>(blocks_[r.block_indices[slot]].mapped_ptr) + off;
    assert(p && "bump allocate returned null");
    return p;
}

uint32_t MemoryAllocator::BumpMasterHeapIndex(Memory mem) const {
    const BumpRing& r = rings_[mem_index(mem)];
    return r.block_indices[r.current_slot];
}

uint32_t MemoryAllocator::BumpSaveCursor(Memory mem) const {
    const BumpRing& r = rings_[mem_index(mem)];
    return r.cursors[r.current_slot];
}

void MemoryAllocator::BumpRestoreCursor(Memory mem, uint32_t cursor) {
    BumpRing& r = rings_[mem_index(mem)];
    r.cursors[r.current_slot] = cursor;
}

MTL::Buffer* MemoryAllocator::HeapMasterBuffer(uint32_t heap_index) const {
    assert(heap_index < blocks_.size() && "heap_index out of range");
    assert(blocks_[heap_index].master_buffer && "null master buffer");
    return blocks_[heap_index].master_buffer;
}

MTL::Heap* MemoryAllocator::HeapHandle(uint32_t heap_index) const {
    return blocks_[heap_index].heap;
}

void* MemoryAllocator::HeapMappedPtr(uint32_t heap_index) const {
    return blocks_[heap_index].mapped_ptr;
}

uint64_t MemoryAllocator::HeapGpuAddress(uint32_t heap_index) const {
    return blocks_[heap_index].gpu_address;
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
        if (p.is_image && p.texture) {
            p.texture->release();
        }
        blocks_[p.heap_index].offset_alloc.free(p.alloc);
    }
    pending.clear();
}

}  // namespace cairns::rhi::metal

#endif  // CAIRNS_METAL
