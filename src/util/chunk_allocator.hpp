// src/util/chunk_allocator.hpp
//
// Allocator B (per ALLOCATOR_HANDOFF.md §4): the root CPU memory source. A
// segregated-free-list (size-class) pool backed by large malloc'd chunks, with a
// malloc fallback for oversize requests. Every other CPU allocator borrows from
// here: per-frame bump (A) gets its slab from B; load-time bump arenas (a
// BumpArena around a B chunk) are freed wholesale at asset unload by returning
// the chunk to B; std::vector sites use `ChunkStdAllocator<T>`.
//
// O(1) Free with NO side table: each user pointer is preceded by a 16-byte
// header carrying the size class + region tag, so `Free(p)` reads `p - 16` and
// pushes the cell back onto its class's free list. This is the concrete fix for
// the std::map<void*, Allocation> wart in std_allocator.hpp.
//
// SINGLE THREADED. Honor the existing convention -- carve per-thread arenas
// rather than locking when CAIRNS_RG_PARALLEL / physics eventually go MT.

#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cairns {

inline constexpr uint16_t kNoRegion = 0;

class ChunkAllocator {
public:
    // Size classes are powers of two of the TOTAL cell size (header + user
    // bytes). Min cell is 32 B (16 B header + 16 B min usable); max class is set
    // by block_bytes at Init -- anything larger than a chunk routes to oversize.
    static constexpr uint32_t kMinClassLog = 5;   // 32 B cell
    static constexpr uint32_t kMaxClassLog = 28;  // up to 256 MB cell (capped by block_bytes_)
    static constexpr uint32_t kNumClasses = kMaxClassLog - kMinClassLog + 1;
    static constexpr uint32_t kHeaderBytes = 16;  // keep cell_start 16-aligned -> user_ptr 16-aligned
    static constexpr uint32_t kDefaultAlign = 16;
    static constexpr uint32_t kDefaultBlockBytes = 4u * 1024u * 1024u;

    ChunkAllocator() = default;
    ~ChunkAllocator() { Deinit(); }
    ChunkAllocator(const ChunkAllocator&) = delete;
    ChunkAllocator& operator=(const ChunkAllocator&) = delete;

    void Init(uint32_t block_bytes = kDefaultBlockBytes) {
        assert(block_bytes >= (1u << kMinClassLog) && "block_bytes too small");
        block_bytes_ = block_bytes;
        max_class_log_ = std::bit_width(block_bytes_) - 1;  // largest class that still fits a chunk
        for (uint32_t i = 0; i < kNumClasses; ++i) {
            free_lists_[i] = nullptr;
        }
        chunks_.clear();
        oversize_count_ = 0;
        bytes_in_use_ = 0;
        for (uint32_t i = 0; i < kNumClasses; ++i) {
            bytes_in_use_by_class_[i] = 0;
        }
#ifndef NDEBUG
        outstanding_ = 0;
#endif
    }

    void Deinit() {
        if (block_bytes_ == 0) {
            return;
        }
#ifndef NDEBUG
        assert(outstanding_ == 0 && "ChunkAllocator: outstanding allocations at Deinit");
#endif
        for (Chunk& c : chunks_) {
            std::free(c.base);
        }
        chunks_.clear();
        for (uint32_t i = 0; i < kNumClasses; ++i) {
            free_lists_[i] = nullptr;
        }
        block_bytes_ = 0;
    }

    // CALLER: any CPU subsystem that needs a heap allocation. `align` defaults
    // to 16 (covers std::max_align_t on the macs we target). Passing align > 16
    // routes to the oversize/malloc path so the cell layout doesn't have to
    // carry padding metadata.
    void* Allocate(uint32_t bytes, uint32_t align = kDefaultAlign, uint16_t region = kNoRegion) {
        const uint32_t needed = bytes + kHeaderBytes;
        const bool oversize =
            align > kDefaultAlign || needed > block_bytes_ || bytes > 0xFFFFFFFFu - kHeaderBytes;
        if (oversize) {
            return AllocateOversize(bytes, align, region);
        }
        const uint32_t cls_log = ClassLogFor(needed);
        const uint32_t cell_bytes = 1u << cls_log;
        const uint32_t idx = cls_log - kMinClassLog;
        uint8_t* cell = reinterpret_cast<uint8_t*>(free_lists_[idx]);
        if (cell != nullptr) {
            free_lists_[idx] = reinterpret_cast<FreeCell*>(cell)->next;
        } else {
            cell = Carve(cell_bytes);
            if (cell == nullptr) {
                return nullptr;
            }
        }
        WriteHeader(cell, static_cast<uint8_t>(cls_log), 0 /*flags*/, region, cell_bytes);
        bytes_in_use_ += cell_bytes;
        bytes_in_use_by_class_[idx] += cell_bytes;
#ifndef NDEBUG
        ++outstanding_;
#endif
        return cell + kHeaderBytes;
    }

    void Free(void* p) {
        if (p == nullptr) {
            return;
        }
        uint8_t* user = static_cast<uint8_t*>(p);
        Header* h = reinterpret_cast<Header*>(user - kHeaderBytes);
        if (h->flags & kFlagOversize) {
            // Recover the malloc base from the header's stored prefix bytes.
            uint8_t* malloc_base = user - h->prefix_bytes;
            std::free(malloc_base);
            assert(oversize_count_ > 0);
            --oversize_count_;
            bytes_in_use_ -= h->cell_bytes;
#ifndef NDEBUG
            --outstanding_;
#endif
            return;
        }
        const uint32_t idx = h->size_class - kMinClassLog;
        const uint32_t cell_bytes = static_cast<uint32_t>(h->cell_bytes);
        FreeCell* fc = reinterpret_cast<FreeCell*>(user - kHeaderBytes);
        fc->next = free_lists_[idx];
        free_lists_[idx] = fc;
        bytes_in_use_ -= cell_bytes;
        bytes_in_use_by_class_[idx] -= cell_bytes;
#ifndef NDEBUG
        --outstanding_;
#endif
    }

    // FreeRegion: bulk free everything tagged with `region`. Hooked for future
    // per-asset unload; not exercised in this pass. Asserting-stub keeps
    // accidental use from silently leaking.
    void FreeRegion(uint16_t region) {
        (void)region;
        assert(false && "ChunkAllocator::FreeRegion not implemented yet");
    }

    uint64_t BytesInUse() const { return bytes_in_use_; }
    uint64_t BytesInUseByClass(uint32_t cls_log) const {
        if (cls_log < kMinClassLog || cls_log > kMaxClassLog) {
            return 0;
        }
        return bytes_in_use_by_class_[cls_log - kMinClassLog];
    }
    uint32_t OversizeAllocs() const { return oversize_count_; }
    uint32_t ChunkCount() const { return static_cast<uint32_t>(chunks_.size()); }
    uint32_t BlockBytes() const { return block_bytes_; }

private:
    static constexpr uint8_t kFlagOversize = 1 << 0;

    struct Header {
        uint8_t  size_class;   // log2(cell_bytes) for in-class; unused-but-set for oversize
        uint8_t  flags;
        uint16_t region;
        uint32_t prefix_bytes; // bytes from malloc base to user ptr (oversize path)
        uint64_t cell_bytes;   // total bytes accounted (cell_bytes for class; user+header for oversize)
    };
    static_assert(sizeof(Header) == kHeaderBytes, "Header must be 16 bytes");

    struct FreeCell {
        FreeCell* next;
    };

    struct Chunk {
        uint8_t* base = nullptr;
        uint32_t size = 0;
        uint32_t cursor = 0;
    };

    uint32_t ClassLogFor(uint32_t needed) const {
        const uint32_t bw = std::bit_width(needed - 1);
        return bw < kMinClassLog ? kMinClassLog : bw;
    }

    uint8_t* Carve(uint32_t cell_bytes) {
        if (chunks_.empty() || chunks_.back().cursor + cell_bytes > chunks_.back().size) {
            if (!AddChunk()) {
                return nullptr;
            }
        }
        Chunk& c = chunks_.back();
        uint8_t* p = c.base + c.cursor;
        c.cursor += cell_bytes;
        return p;
    }

    bool AddChunk() {
        // 16-byte aligned chunks so every cell_start is 16-aligned -> user_ptr is 16-aligned.
        void* mem = std::aligned_alloc(kDefaultAlign, block_bytes_);
        if (mem == nullptr) {
            return false;
        }
        Chunk c{};
        c.base = static_cast<uint8_t*>(mem);
        c.size = block_bytes_;
        c.cursor = 0;
        chunks_.push_back(c);
        return true;
    }

    void WriteHeader(uint8_t* cell, uint8_t size_class, uint8_t flags, uint16_t region,
                     uint64_t cell_bytes) {
        Header* h = reinterpret_cast<Header*>(cell);
        h->size_class = size_class;
        h->flags = flags;
        h->region = region;
        h->prefix_bytes = kHeaderBytes;  // user is right after header for in-class cells
        h->cell_bytes = cell_bytes;
    }

    void* AllocateOversize(uint32_t bytes, uint32_t align, uint16_t region) {
        // We need: |...padding...|Header|...user bytes (aligned to `align`)...|
        // user_ptr - kHeaderBytes must hold the header; user_ptr % align == 0.
        const uint32_t a = align < kDefaultAlign ? kDefaultAlign : align;
        // Worst-case prefix to align user_ptr is (a-1) bytes before the header, plus the header.
        const size_t total = static_cast<size_t>(bytes) + kHeaderBytes + (a - 1);
        void* mem = std::malloc(total);
        if (mem == nullptr) {
            return nullptr;
        }
        // Find user_ptr: smallest address >= mem + kHeaderBytes that is `a`-aligned.
        uintptr_t base = reinterpret_cast<uintptr_t>(mem);
        uintptr_t user = (base + kHeaderBytes + (a - 1)) & ~static_cast<uintptr_t>(a - 1);
        uint8_t* user_ptr = reinterpret_cast<uint8_t*>(user);
        Header* h = reinterpret_cast<Header*>(user_ptr - kHeaderBytes);
        h->size_class = 0;
        h->flags = kFlagOversize;
        h->region = region;
        h->prefix_bytes = static_cast<uint32_t>(user_ptr - static_cast<uint8_t*>(mem));
        h->cell_bytes = static_cast<uint64_t>(bytes) + kHeaderBytes;
        ++oversize_count_;
        bytes_in_use_ += h->cell_bytes;
#ifndef NDEBUG
        ++outstanding_;
#endif
        return user_ptr;
    }

    uint32_t block_bytes_ = 0;
    uint32_t max_class_log_ = 0;
    FreeCell* free_lists_[kNumClasses] = {};
    std::vector<Chunk> chunks_;
    uint64_t bytes_in_use_ = 0;
    uint64_t bytes_in_use_by_class_[kNumClasses] = {};
    uint32_t oversize_count_ = 0;
#ifndef NDEBUG
    uint64_t outstanding_ = 0;
#endif
};

// STL adapter so existing `std::vector<T, cairns::Allocator<T>>` sites can route
// through B. Holds a non-owning pointer to a ChunkAllocator; deallocate() does a
// real Free (unlike the bump-arena adapter, which is no-op).
template <typename T>
class ChunkStdAllocator {
public:
    using value_type = T;

    explicit ChunkStdAllocator(ChunkAllocator& alloc) : alloc_(&alloc) {}
    template <typename U>
    ChunkStdAllocator(const ChunkStdAllocator<U>& other) : alloc_(other.alloc_) {}

    T* allocate(size_t n) {
        const size_t bytes = n * sizeof(T);
        assert(bytes <= 0xFFFFFFFFu && "ChunkStdAllocator: allocation > 4 GB");
        void* p = alloc_->Allocate(static_cast<uint32_t>(bytes), alignof(T));
        if (p == nullptr) {
            std::printf("ChunkStdAllocator: out of memory\n");
            std::abort();
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t /*n*/) { alloc_->Free(p); }

    template <typename U>
    bool operator==(const ChunkStdAllocator<U>& o) const { return alloc_ == o.alloc_; }
    template <typename U>
    bool operator!=(const ChunkStdAllocator<U>& o) const { return alloc_ != o.alloc_; }

    ChunkAllocator* alloc_;
};

}  // namespace cairns
