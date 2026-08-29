// src/util/cpu_pool.hpp
//
// Persistent tier (Tier 2): objects with individual lifetimes -- born and freed
// one at a time as skinned entities spawn / despawn. This is the ONE skinning
// tier that needs a real suballocator with a free list, so it wraps Sebastian
// Aaltonen's OffsetAllocator (the same allocator rhi's GPU heap blocks use).
//
// The unit of currency is an OFFSET, not a pointer. That is deliberate: the
// canonical user is skin_output_pool -- a single persistent GPU buffer whose
// sub-ranges are owned by individual skinned instances. The CPU never
// dereferences those ranges; it only assigns and recycles them. OffsetAllocator
// owns offsets, not memory, so it maps onto "carve ranges out of a GPU buffer"
// exactly, with O(1) alloc/free and neighbor coalescing.
//
// The handle (PoolSlice) carries its own OffsetAllocator::Allocation, so Free
// needs nothing but the slice back. There is no void*->Allocation side table --
// the metadata lives with the owner (store the slice in the skin's cold record),
// the way Acton-style hot/cold layouts keep each record's own indices. This is
// the std_allocator.hpp Arena's std::map<void*,Allocation> done away with.

#pragma once

#include "util/offset_allocator.hpp"

#include <cassert>
#include <cstdint>

namespace cairns {

struct PoolSlice {
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    uint32_t offset = kInvalid;                  // offset into the pool, in `units`
    uint32_t count  = 0;                         // units requested (caller's stride)
    OffsetAllocator::Allocation alloc{};         // metadata for O(1) Free -- store w/ owner

    bool IsValid() const { return offset != kInvalid; }
};

// Offset-only pool: assigns sub-ranges of an externally-owned buffer (typically a
// persistent GPU buffer such as skin_output_pool). Allocate in element units
// (e.g. one skinned vertex, one joint-matrix slot) so offsets land on natural
// strides; multiply by the element size when you bind on the GPU.
struct RangePool {
    // `capacity_units` is the total number of elements the backing pool holds.
    // `max_allocs` caps live sub-ranges (OffsetAllocator node budget).
    void Init(uint32_t capacity_units, uint32_t max_allocs = 64 * 1024) {
        alloc_ = OffsetAllocator::Allocator(capacity_units, max_allocs);
        capacity_units_ = capacity_units;
        max_allocs_ = max_allocs;
    }

    // #229: free every outstanding slice at once, reusing the same capacity.
    // Safe only at a teardown point where no live PoolSlice is subsequently
    // Free'd or dereferenced (e.g. after the render thread is drained and all
    // owning actors are gone).
    void Reset() {
        alloc_ = OffsetAllocator::Allocator(capacity_units_, max_allocs_);
    }

    // Returns an invalid slice (check IsValid()) when the pool is full; the
    // caller decides whether to grow the backing buffer or evict.
    PoolSlice Alloc(uint32_t units) {
        OffsetAllocator::Allocation a = alloc_.allocate(units);
        if (a.offset == OffsetAllocator::Allocation::NO_SPACE) {
            return PoolSlice{};
        }
        return PoolSlice{ .offset = a.offset, .count = units, .alloc = a };
    }

    void Free(const PoolSlice& s) {
        if (!s.IsValid()) return;
        alloc_.free(s.alloc);
    }

    uint32_t Capacity() const { return capacity_units_; }
    OffsetAllocator::StorageReport Report() const { return alloc_.storageReport(); }

private:
    OffsetAllocator::Allocator alloc_;
    uint32_t capacity_units_ = 0;
    uint32_t max_allocs_ = 64 * 1024;
};

// CPU-backed persistent pool: owns a typed slab and hands out pointers whose
// ranges recycle individually. Use this only for persistent data the CPU
// actually reads/writes; skin_output is GPU-resident, so it uses RangePool above.
template <typename T>
struct TypedPool {
    struct Ref {
        T* ptr = nullptr;
        PoolSlice slice;
        bool IsValid() const { return ptr != nullptr; }
    };

    // `mem` is a slab of at least capacity_elems * sizeof(T) bytes (not owned).
    void Init(void* mem, uint32_t capacity_elems, uint32_t max_allocs = 64 * 1024) {
        base_ = static_cast<T*>(mem);
        pool_.Init(capacity_elems, max_allocs);
    }

    Ref Alloc(uint32_t count) {
        PoolSlice s = pool_.Alloc(count);
        if (!s.IsValid()) return Ref{};
        return Ref{ base_ + s.offset, s };
    }
    void Free(const Ref& r) { pool_.Free(r.slice); }

    OffsetAllocator::StorageReport Report() const { return pool_.Report(); }

private:
    RangePool pool_;
    T* base_ = nullptr;
};

}  // namespace cairns
