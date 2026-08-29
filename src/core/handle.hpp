// core/handle.hpp
//
// Generic typed generational pool.

#pragma once

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "util/chunk_allocator.hpp"  // #229 M0b: block-backed pool storage.

namespace cairns {

template <typename T>
struct Handle {
    uint16_t index = 0xFFFF;
    uint16_t generation = 0xFFFF;

    constexpr bool IsNull() const { return index == 0xFFFF; }
    constexpr auto operator<=>(const Handle&) const = default;

    static const Handle Null;
};

template <typename T>
const Handle<T> Handle<T>::Null = Handle<T>{};

template <typename T>
class ResourceManager {
public:
    ResourceManager() = default;
    // Raw-pointer ownership: the four arrays are carved from the chunk and the
    // elements are placement-new'd, so a copy/move would double-free or dangle.
    // Pools are members of non-copyable owners; they are never copied or moved.
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;
    ResourceManager(ResourceManager&&) = delete;
    ResourceManager& operator=(ResourceManager&&) = delete;
    ~ResourceManager() {
        if (!block_) {
            return;  // never Reserve'd -- nothing carved
        }
        using HotT = typename T::Hot;
        using ColdT = typename T::Cold;
        for (uint32_t i = 0; i < capacity_; ++i) {
            hot_[i].~HotT();    // inner vectors/strings free their heap
            cold_[i].~ColdT();
        }
        block_->Free(hot_);
        block_->Free(cold_);
        block_->Free(generation_);
        block_->Free(freelist_);
    }

    Handle<T> Acquire() {
        uint16_t idx;
        if (freelist_count_ > 0) {
            idx = freelist_[--freelist_count_];
            hot_[idx] = typename T::Hot{};
            cold_[idx] = typename T::Cold{};
        } else {
            // The pool must not grow, or else all hot*/cold* break.
            // DO NOT STORE hot*/cold* pointers. capacity_==0 means the pool
            // was never Reserve'd: abort rather than grow (there is no growable
            // storage anymore -- the arrays are fixed-capacity chunk blocks).
            if (size_ >= capacity_) {
                std::fprintf(
                    stderr,
                    "ResourceManager: Acquire past capacity (%u/%u) -- pool "
                    "unreserved or over cap; raise the MemoryBudget reserve\n",
                    size_, capacity_);
                std::abort();
            }
            idx = static_cast<uint16_t>(size_++);  // slot pre-built in Reserve
        }
        return Handle<T>{idx, generation_[idx]};
    }

    void Release(Handle<T> h) {
        if (h.index >= size_) {
            return;
        }
        if (generation_[h.index] != h.generation) {
            return;
        }
        generation_[h.index]++;
        freelist_[freelist_count_++] = h.index;
    }

    // #229: recycle every live slot back to empty, reusing the backing (no
    // realloc). Bumps generations so any outstanding handle goes dead, and
    // re-default-constructs each slot so a later size_++ Acquire (which assumes
    // Reserve-fresh slots) sees clean state. Teardown-only: no live Handle may
    // be dereferenced afterward.
    void Clear() {
        for (uint32_t i = 0; i < size_; ++i) {
            hot_[i] = typename T::Hot{};
            cold_[i] = typename T::Cold{};
            ++generation_[i];
        }
        size_ = 0;
        freelist_count_ = 0;
    }

    typename T::Hot* GetHot(Handle<T> h) {
        if (h.index >= size_ || generation_[h.index] != h.generation) {
            return nullptr;
        }
        return &hot_[h.index];
    }

    typename T::Cold* GetCold(Handle<T> h) {
        if (h.index >= size_ || generation_[h.index] != h.generation) {
            return nullptr;
        }
        return &cold_[h.index];
    }

    size_t Size() const { return size_; }
    uint16_t Capacity() const { return capacity_; }

    // Chunk-backed fixed storage: one raw block per array, every slot
    // placement-new'd up front so element addresses are stable for the pool's
    // life and GetHot/GetCold never dangle across Acquire. No vector -> it
    // cannot grow or relocate. Reserve is mandatory and called exactly once.
    void Reserve(ChunkAllocator& block, uint16_t cap) {
        block_ = &block;
        capacity_ = cap;
        size_ = 0;
        freelist_count_ = 0;
        hot_ = static_cast<typename T::Hot*>(block.Allocate(
            static_cast<uint32_t>(cap * sizeof(typename T::Hot)),
            alignof(typename T::Hot)));
        cold_ = static_cast<typename T::Cold*>(block.Allocate(
            static_cast<uint32_t>(cap * sizeof(typename T::Cold)),
            alignof(typename T::Cold)));
        generation_ = static_cast<uint16_t*>(block.Allocate(
            static_cast<uint32_t>(cap * sizeof(uint16_t)), alignof(uint16_t)));
        freelist_ = static_cast<uint16_t*>(block.Allocate(
            static_cast<uint32_t>(cap * sizeof(uint16_t)), alignof(uint16_t)));
        for (uint32_t i = 0; i < cap; ++i) {
            new (&hot_[i]) typename T::Hot{};
            new (&cold_[i]) typename T::Cold{};
            generation_[i] = 1;
        }
    }

    template <typename Fn>
    void ForEachLive(Fn&& fn) {
        std::vector<bool> is_free(size_, false);
        for (uint32_t i = 0; i < freelist_count_; ++i) {
            is_free[freelist_[i]] = true;
        }
        for (uint32_t i = 0; i < size_; ++i) {
            if (!is_free[i]) {
                fn(hot_[i], cold_[i]);
            }
        }
    }

private:
    typename T::Hot* hot_ = nullptr;
    typename T::Cold* cold_ = nullptr;
    uint16_t* generation_ = nullptr;
    uint16_t* freelist_ = nullptr;
    uint32_t size_ = 0;            // high-water: slots [0,size_) handed out
    uint32_t freelist_count_ = 0;  // live entries in freelist_
    uint16_t capacity_ = 0;        // 0 = un-Reserved (Acquire aborts)
    ChunkAllocator* block_ = nullptr;
};

}  // namespace cairns
