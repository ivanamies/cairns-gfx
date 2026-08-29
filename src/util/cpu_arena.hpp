// src/util/cpu_arena.hpp
//
// CPU bump (linear) arenas -- OFFSET-NATIVE, to match the GPU bump ring
// (rhi::Allocator::BumpAllocate returns a uint32 offset) and the repo's
// handle/index/offset-over-pointer discipline.
//
// The unit of currency is a uint32 byte offset into the arena's slab. You STORE
// the offset (e.g. in InstanceMeta / SkinMeshBatch) and Resolve() it to a
// TRANSIENT pointer only for the duration of a write (one memcpy). A pointer
// here is a local, never a stored reference -- same contract as the GPU ring,
// which hands back a pointer to fill AND an offset to keep.
//
//   * BumpArena  -- monotonic bump over a fixed slab. O(1) alloc, no per-object
//                   free, bulk Reset(), LIFO Mark()/Rewind(). LOAD-TIME tier
//                   (Tier 1): clip keyframes, joint-index sets, inverse-bind
//                   matrices, packed-skin-SSBO staging -- born together at GLB
//                   load, freed together at asset unload.
//
//   * FrameArena -- kFramesInFlight BumpArenas in a ring; BeginFrame(i) resets
//                   slot i. PER-FRAME TRANSIENT tier (Tier 3): joint palettes,
//                   InstanceMeta, SkinMeshBatch. Double-buffered for the same
//                   reason the GPU ring is: the CPU builds frame N+1 while the
//                   GPU still reads frame N. Offsets are relative to the current
//                   slot and are valid only until that slot's next BeginFrame.
//
// Neither tier uses OffsetAllocator: when everything in a tier dies at the same
// instant, a free list is dead weight (that is the persistent tier -- cpu_pool.hpp).
//
// SINGLE THREADED per arena. For CAIRNS_RG_PARALLEL / physics, carve one
// BumpArena per worker out of the frame slab rather than locking.

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>

namespace cairns {

// Sentinel for "no space". Offset 0 is a valid allocation, so the failure value
// is the top of the range (mirrors OffsetAllocator::Allocation::NO_SPACE).
inline constexpr uint32_t kInvalidOffset = 0xFFFFFFFFu;

struct BumpArena {
    using Marker = size_t;

    BumpArena() = default;

    // Does not own `mem`; the caller provides the slab. `capacity` is in bytes and
    // must fit in a uint32 offset.
    void Init(void* mem, size_t capacity) {
        assert(capacity <= 0xFFFFFFFFull && "BumpArena slab must fit in a uint32 offset");
        base_ = static_cast<uint8_t*>(mem);
        capacity_ = capacity;
        cursor_ = 0;
        high_water_ = 0;
    }

    // PRIMARY API. Bump-allocate `bytes` aligned to `align` (power of two) and
    // return the byte offset into the slab. kInvalidOffset on overflow. STORE THIS.
    uint32_t AllocateOffset(size_t bytes, size_t align = alignof(std::max_align_t)) {
        assert((align & (align - 1)) == 0 && "align must be a power of two");
        const size_t aligned = (cursor_ + (align - 1)) & ~(align - 1);
        if (aligned + bytes > capacity_) {
            assert(false && "BumpArena overflow -- raise capacity or split the tier");
            return kInvalidOffset;
        }
        cursor_ = aligned + bytes;
        if (cursor_ > high_water_) high_water_ = cursor_;
        return static_cast<uint32_t>(aligned);
    }

    // Resolve an offset to a TRANSIENT pointer. Do not store the result; it is
    // invalidated by Reset()/Rewind() and (for FrameArena) by the next BeginFrame.
    void* Resolve(uint32_t offset) {
        assert(offset != kInvalidOffset && offset <= cursor_ && "stale or bad offset");
        return base_ + offset;
    }
    template <typename T>
    T* ResolveAs(uint32_t offset) { return static_cast<T*>(Resolve(offset)); }

    // Convenience mirroring rhi::Allocator::BumpAllocate: returns a transient write
    // pointer AND, via out_offset, the offset to store. Use the pointer for the
    // memcpy, keep *out_offset. nullptr on overflow. For the fill-then-upload case
    // where nothing is stored, out_offset may be null.
    void* Allocate(size_t bytes, size_t align = alignof(std::max_align_t),
                   uint32_t* out_offset = nullptr) {
        const uint32_t off = AllocateOffset(bytes, align);
        if (out_offset) *out_offset = off;
        return (off == kInvalidOffset) ? nullptr : base_ + off;
    }
    template <typename T>
    T* AllocateArray(size_t count, uint32_t* out_offset = nullptr) {
        return static_cast<T*>(Allocate(count * sizeof(T), alignof(T), out_offset));
    }

    // Scoped rewind. LIFO only (not for undo). Handy for load-time scratch.
    Marker Mark() const { return cursor_; }
    void Rewind(Marker m) {
        assert(m <= cursor_ && "Rewind marker is ahead of cursor");
        cursor_ = m;
    }

    // Wholesale reclaim. O(1). Does not zero. The only "free" Tier 1/3 get.
    void Reset() { cursor_ = 0; }

    size_t Used() const { return cursor_; }
    size_t Capacity() const { return capacity_; }
    // Peak bytes since Init -- size the slab from a real run instead of guessing.
    size_t HighWater() const { return high_water_; }

private:
    uint8_t* base_ = nullptr;
    size_t capacity_ = 0;
    size_t cursor_ = 0;
    size_t high_water_ = 0;
};

// Per-frame transient ring. Mirrors rhi::Allocator::AdvanceFrame: BeginFrame
// resets the slot we are about to write; the other slot(s) may still be read by
// the GPU. Offsets are relative to the current slot.
struct FrameArena {
    static constexpr uint32_t kMaxFrames = 4;  // bump if kFramesInFlight ever grows

    // Slices one caller-provided slab into FramesInFlight equal rings. Pass
    // rhi::kFramesInFlight so the CPU ring depth matches the GPU's exactly.
    template <uint32_t FramesInFlight>
    void Init(void* mem, size_t bytes_per_frame) {
        static_assert(FramesInFlight >= 1 && FramesInFlight <= kMaxFrames,
                      "FramesInFlight out of range");
        frames_ = FramesInFlight;
        uint8_t* p = static_cast<uint8_t*>(mem);
        for (uint32_t i = 0; i < FramesInFlight; ++i) {
            rings_[i].Init(p + i * bytes_per_frame, bytes_per_frame);
        }
    }

    // CALLER: ENGINE, once per frame, beside rhi_.alloc.AdvanceFrame(frame_).
    void BeginFrame(uint32_t frame_index) {
        slot_ = frame_index % frames_;
        rings_[slot_].Reset();
    }

    BumpArena& Current() { return rings_[slot_]; }

    // Offset-native, relative to the current slot; valid only until this slot's
    // next BeginFrame. Store the offset, Resolve transiently -- same as the GPU ring.
    uint32_t AllocateOffset(size_t bytes, size_t align = alignof(std::max_align_t)) {
        return rings_[slot_].AllocateOffset(bytes, align);
    }
    void* Resolve(uint32_t offset) { return rings_[slot_].Resolve(offset); }
    template <typename T>
    T* ResolveAs(uint32_t offset) { return rings_[slot_].ResolveAs<T>(offset); }

    void* Allocate(size_t bytes, size_t align = alignof(std::max_align_t),
                   uint32_t* out_offset = nullptr) {
        return rings_[slot_].Allocate(bytes, align, out_offset);
    }
    template <typename T>
    T* AllocateArray(size_t count, uint32_t* out_offset = nullptr) {
        return rings_[slot_].AllocateArray<T>(count, out_offset);
    }

    size_t HighWaterAny() const {
        size_t hw = 0;
        for (uint32_t i = 0; i < frames_; ++i) {
            if (rings_[i].HighWater() > hw) hw = rings_[i].HighWater();
        }
        return hw;
    }

private:
    std::array<BumpArena, kMaxFrames> rings_{};
    uint32_t frames_ = 1;
    uint32_t slot_ = 0;
};

// #219 Chunk B: minimal arena-backed fixed-capacity growable list. POD
// (T* + size + cap), no allocator template, no STL fight. push_back asserts
// on cap so the producer commits to a known upper bound; bulk-reset via
// BumpArena::Reset (clear() just zeroes size). Use this instead of
// std::vector<T, BumpStdAllocator<T>> whenever you don't need STL allocator
// composition -- it sidesteps the "construct vector before arena exists"
// problem.
template <typename T>
struct ArenaList {
    T* begin() { return data_; }
    T* end()   { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end()   const { return data_ + size_; }
    T& operator[](size_t i)             { return data_[i]; }
    const T& operator[](size_t i) const { return data_[i]; }
    T* data()             { return data_; }
    const T* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const  { return size_ == 0; }
    void clear() { size_ = 0; }
    void push_back(const T& v) {
        assert(size_ < cap_ && "ArenaList full -- raise Reset() cap");
        data_[size_++] = v;
    }
    // Bind to a fresh arena slice. Old slice is implicitly abandoned (the
    // arena's Reset() will reclaim the lot). cap may be 0 -- no-op (no
    // push_back will be attempted on an unused list).
    void Reset(BumpArena& arena, uint32_t cap) {
        data_ = cap ? arena.AllocateArray<T>(cap) : nullptr;
        size_ = 0;
        cap_ = cap;
    }

private:
    T* data_ = nullptr;
    uint32_t size_ = 0;
    uint32_t cap_ = 0;
};

// #229 M0b: a POINTER-FREE block-relative array -- {byte offset, count} into a
// BumpArena. Unlike ArenaList (which stores a raw T*), ArenaSlice stores an
// OFFSET, so a struct embedding it is raw-hashable (the pointer quarantine).
// Resolve transiently via the backing arena; never store the pointer. Count is
// known up-front (the glTF loader pre-passes), so Alloc-then-fill.
template <typename T>
struct ArenaSlice {
    uint32_t offset = kInvalidOffset;  // byte offset into the BumpArena slab
    uint32_t count = 0;

    uint32_t size() const { return count; }
    bool empty() const { return count == 0; }
    bool IsNull() const { return offset == kInvalidOffset; }

    T* data(BumpArena& a) const { return a.ResolveAs<T>(offset); }
    T& operator()(BumpArena& a, uint32_t i) const { return data(a)[i]; }

    // Allocate `n` elements from `a`; returns the slice (fill via data(a)[i]).
    static ArenaSlice Alloc(BumpArena& a, uint32_t n) {
        ArenaSlice s;
        if (n > 0) {
            s.offset = a.AllocateOffset(static_cast<size_t>(n) * sizeof(T),
                                        alignof(T));
            s.count = (s.offset == kInvalidOffset) ? 0 : n;
        }
        return s;
    }
};

// STL-compatible adapter so existing std::vector<T, cairns::Allocator<T>> sites
// can ride a BumpArena. STL mandates a pointer interface, so this is the one
// sanctioned pointer-returning path -- it is contained: the vector is the owner,
// referenced by handle/index one layer up, never raw-pointered across systems.
// deallocate() is a no-op (bulk reclaim via BumpArena::Reset). Reserve up front so
// the container never grows (growth bump-leaks until Reset); LoadPrefabsGpu already
// measures totals, so counts are known.
template <typename T>
class BumpStdAllocator {
public:
    using value_type = T;

    explicit BumpStdAllocator(BumpArena& arena) : arena_(&arena) {}
    template <typename U>
    BumpStdAllocator(const BumpStdAllocator<U>& other) : arena_(other.arena_) {}

    T* allocate(size_t n) {
        T* p = arena_->AllocateArray<T>(n);
        if (!p) {
            fprintf(stderr, "BumpStdAllocator: out of arena (std::bad_alloc)\n");
            std::terminate();
        }
        return p;
    }
    void deallocate(T*, size_t) {}  // no-op: bulk reclaim via BumpArena::Reset

    template <typename U>
    bool operator==(const BumpStdAllocator<U>& o) const { return arena_ == o.arena_; }
    template <typename U>
    bool operator!=(const BumpStdAllocator<U>& o) const { return arena_ != o.arena_; }

    BumpArena* arena_;
};

}  // namespace cairns
