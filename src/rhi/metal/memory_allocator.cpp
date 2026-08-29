// rhi/metal/memory_allocator.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi/metal/memory_allocator.hpp"

#include <Metal/Metal.hpp>

#include <cassert>
#include <cstdlib>
#include <cstring>
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
        if (blocks_[i].heap || blocks_[i].master_buffer) {
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

    // Per-slot byte budget for each Memory type. Heap = sum * kFramesInFlight.
    bump_.slot_size[mem_index(Memory::kUpload)]   = 64u * 1024u * 1024u;
    // #221 Phase 3: 16 -> 32 MB. Matches the Vulkan side; reasoning is in
    // src/rhi/vulkan/memory_allocator.cpp's Phase-3 comment.
    bump_.slot_size[mem_index(Memory::kDynamic)]  = 32u * 1024u * 1024u;
    bump_.slot_size[mem_index(Memory::kReadback)] =  8u * 1024u * 1024u;
    bump_.slot_size[mem_index(Memory::kDefault)]  = 0;
    bump_.slot_size[mem_index(Memory::kTransient)] = 0;

    uint32_t running = 0;
    for (size_t m = 0; m < kMemoryCount; ++m) {
        bump_.region_base[m] = running;
        running += bump_.slot_size[m] * kFramesInFlight;
    }
    // running now = total bump heap bytes.

    if (!CreateBumpHeap()) {
        return false;
    }

    initialized_ = true;
    return true;
}

bool MemoryAllocator::CreateBumpHeap() {
    // Total span = sum of (slot_size[m] * kFramesInFlight). All slots/types
    // share ONE MTL::Heap + ONE MTL::Buffer; BumpAllocate is offset arithmetic.
    uint32_t total = 0;
    for (size_t m = 0; m < kMemoryCount; ++m) {
        total += bump_.slot_size[m] * kFramesInFlight;
    }
    assert(total > 0 && "bump heap budget is zero");

    // Skip the heap wrapper for the Shared bump master. iOS Simulator's
    // MTLSimDevice rejects any non-Private heap ('MTLStorageModePrivate is
    // required for heaps'); a standalone Shared MTL::Buffer behaves identically
    // for our bump-ring use case (one giant CPU-mapped buffer carved up by
    // offset) on real devices.
    MTL::Buffer* master = device_->newBuffer(
        total,
        MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked);
    if (!master) {
        return false;
    }

    // Paint every freshly-allocated byte with 0xCC at allocation time;
    // proper initialization happens at first use (UploadBuffer for buffers,
    // LoadOp::kClear for render targets). The point is to surface any
    // read-before-proper-init path LOUDLY: an uninitialized read gets
    // 0xCCCCCCCC everywhere instead of an accidentally-zeroed value that
    // might render "fine" by coincidence. Set CAIRNS_HEAP_ZERO=1 to fall
    // back to 0x00 fill (useful when chasing whether 0xCC itself perturbs
    // pixels). Empirical note 2026-06-05: Metal pre-zeros newBuffer +
    // heap newBuffer at allocation -- proven by hexdump under
    // CAIRNS_HEAPDUMP=1 -- but driver behavior is not contract, and we
    // want the symptom-on-first-leak guarantee regardless.
    if (void* ptr = master->contents()) {
        const uint8_t pat = std::getenv("CAIRNS_HEAP_ZERO") ? 0x00 : 0xCC;
        std::memset(ptr, pat, total);
    }

    HeapBlock blk;
    blk.heap = nullptr;
    blk.master_buffer = master;
    blk.mapped_ptr = master->contents();
    blk.gpu_address = master->gpuAddress();
    blk.size_bytes = total;
    blk.mem_type = Memory::kDynamic;
    blk.is_image_pool = false;

    assert(blocks_.empty() && "bump heap must be blocks_[0]");
    blocks_.push_back(std::move(blk));
    return true;
}

bool MemoryAllocator::CreateBufferBlock(uint32_t bytes, Memory mem,
                                        uint32_t* out_index) {
    const MTL::StorageMode sm = storage_mode_for(mem);
    MTL::Heap* heap = nullptr;
    MTL::Buffer* master = nullptr;

    if (sm == MTL::StorageModePrivate) {
        MTL::HeapDescriptor* hd = MTL::HeapDescriptor::alloc()->init();
        hd->setSize(bytes);
        hd->setType(MTL::HeapTypePlacement);
        hd->setResourceOptions(options_for(mem));
        heap = device_->newHeap(hd);
        hd->release();
        if (!heap) {
            return false;
        }
        master = heap->newBuffer(bytes, options_for(mem), 0);
        if (!master) {
            heap->release();
            return false;
        }
        // Private memory cannot be CPU-memset. Paint via a blit fillBuffer
        // so anything reading pre-UploadBuffer hits 0xCC (see bump heap
        // comment above for the rationale). One-shot queue; init path only.
        const uint8_t pat = std::getenv("CAIRNS_HEAP_ZERO") ? 0x00 : 0xCC;
        MTL::CommandQueue* q = device_->newCommandQueue();
        if (q) {
            MTL::CommandBuffer* cb = q->commandBuffer();
            MTL::BlitCommandEncoder* blit = cb->blitCommandEncoder();
            blit->fillBuffer(master, NS::Range::Make(0, bytes), pat);
            blit->endEncoding();
            cb->commit();
            cb->waitUntilCompleted();
            q->release();
        }
    } else {
        // Shared / memoryless: skip the heap wrapper. iOS Simulator's
        // MTLSimDevice rejects any non-Private heap, and on real Apple devices
        // a standalone Shared buffer is functionally equivalent for our
        // bump-ring use case (one giant CPU-mapped buffer carved up by offset).
        master = device_->newBuffer(bytes, options_for(mem));
        if (!master) {
            return false;
        }
        // Same garbage-init as bump heap.
        if (void* p = master->contents()) {
            const uint8_t pat = std::getenv("CAIRNS_HEAP_ZERO") ? 0x00 : 0xCC;
            std::memset(p, pat, bytes);
        }
    }

    HeapBlock blk;
    blk.heap = heap;
    blk.master_buffer = master;
    blk.mapped_ptr = (sm == MTL::StorageModePrivate) ? nullptr
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
                                     ? padded + (padded >> 3)
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
    const size_t mi = mem_index(mem);
    const uint32_t slot_size = bump_.slot_size[mi];
    assert(slot_size != 0 && "bump: memory type not budgeted");
    if (slot_size == 0) {
        return nullptr;
    }

    const uint32_t slot = bump_.current_slot;
    const uint32_t cursor_local = align_up(bump_.cursors[mi][slot], align);
    assert(cursor_local + bytes <= slot_size && "bump slot overflow");
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

uint32_t MemoryAllocator::BumpMasterHeapIndex(Memory /*mem*/) const {
    return kBumpHeapIndex;
}

uint32_t MemoryAllocator::BumpRingBytes(Memory mem) const {
    return bump_.slot_size[mem_index(mem)];
}

uint32_t MemoryAllocator::BumpSaveCursor(Memory mem) const {
    return bump_.cursors[mem_index(mem)][bump_.current_slot];
}

void MemoryAllocator::BumpRestoreCursor(Memory mem, uint32_t cursor) {
    bump_.cursors[mem_index(mem)][bump_.current_slot] = cursor;
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
    bump_.current_slot = frame_index % kFramesInFlight;
    for (size_t m = 0; m < kMemoryCount; ++m) {
        bump_.cursors[m][bump_.current_slot] = 0;
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
