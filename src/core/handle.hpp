// core/handle.hpp
//
// Generic typed generational pool. Originally lived under namespace
// cairns::rhi as the GPU resource pool, but nothing in either type is
// GPU-specific -- lifted here so worlds, assets, skins, and any future
// pooled persistent state share the exact same machinery. The rhi
// namespace re-exports both via using-aliases so existing code is
// unchanged.
//
// The pattern: Sebastian Aaltonen, "Modern Mobile Rendering Architecture"
// slide 19 ("arrays you walk"). T must define T::Hot and T::Cold nested
// types. Handles are { uint16_t index, uint16_t generation }: stable
// across pool growth (the index doesn't move; you re-resolve via GetHot
// each access). Stale handles fail safe (GetHot/GetCold -> nullptr).

#pragma once

#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
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
    Handle<T> Acquire() {
        uint16_t idx;
        if (!freelist_.empty()) {
            idx = freelist_.back();
            freelist_.pop_back();
            // Reset reused storage so the caller sees a fresh slot.
            // Without this, LoadPrefabFromGltf-style append-into-vector
            // patterns carry over the previous occupant's data
            // (228 R1.x: aatrox loaded -> unloadAll -> aatrox re-loaded
            // hit "nodes>kAnimMaxNodes" because the old nodes vector
            // still had 129 entries before the loader appended 129 more).
            hot_[idx] = typename T::Hot{};
            cold_[idx] = typename T::Cold{};
        } else {
            // #229 M0b: a Reserved pool must NOT grow (a grow relocates Cold
            // records -> dangling Cold* + changed addresses break the hash);
            // un-Reserved pools (capacity_==0) grow on the malloc fallback.
            assert((capacity_ == 0 || hot_.size() < capacity_) &&
                   "ResourceManager over cap -- raise the MemoryBudget reserve");
            idx = static_cast<uint16_t>(hot_.size());
            hot_.emplace_back();
            cold_.emplace_back();
            generation_.push_back(1);
        }
        return Handle<T>{idx, generation_[idx]};
    }

    void Release(Handle<T> h) {
        if (h.index >= generation_.size()) {
            return;
        }
        if (generation_[h.index] != h.generation) {
            return;
        }
        generation_[h.index]++;
        freelist_.push_back(h.index);
    }

    typename T::Hot* GetHot(Handle<T> h) {
        if (h.index >= generation_.size()) {
            return nullptr;
        }
        if (generation_[h.index] != h.generation) {
            return nullptr;
        }
        return &hot_[h.index];
    }

    typename T::Cold* GetCold(Handle<T> h) {
        if (h.index >= generation_.size()) {
            return nullptr;
        }
        if (generation_[h.index] != h.generation) {
            return nullptr;
        }
        return &cold_[h.index];
    }

    size_t Size() const { return hot_.size(); }
    uint16_t Capacity() const { return capacity_; }

    // #229 M0b: bind the four arrays to the block + fix capacity. After this,
    // Acquire never grows, so Reserve MUST precede the first Acquire. POCMA on
    // ChunkStdAllocator makes the move-assign adopt the block allocator;
    // un-Reserved pools keep the malloc fallback (RHI pools, tests).
    void Reserve(ChunkAllocator& block, uint16_t cap) {
        hot_ = HotVec(ChunkStdAllocator<typename T::Hot>(block));
        cold_ = ColdVec(ChunkStdAllocator<typename T::Cold>(block));
        generation_ = U16Vec(ChunkStdAllocator<uint16_t>(block));
        freelist_ = U16Vec(ChunkStdAllocator<uint16_t>(block));
        hot_.reserve(cap);
        cold_.reserve(cap);
        generation_.reserve(cap);
        freelist_.reserve(cap);
        capacity_ = cap;
    }

    template <typename Fn>
    void ForEachLive(Fn fn) {
        std::vector<bool> is_free(hot_.size(), false);
        for (uint16_t fi : freelist_) {
            is_free[fi] = true;
        }
        for (size_t i = 0; i < hot_.size(); ++i) {
            if (!is_free[i]) {
                fn(hot_[i], cold_[i]);
            }
        }
    }

private:
    using HotVec = std::vector<typename T::Hot, ChunkStdAllocator<typename T::Hot>>;
    using ColdVec = std::vector<typename T::Cold, ChunkStdAllocator<typename T::Cold>>;
    using U16Vec = std::vector<uint16_t, ChunkStdAllocator<uint16_t>>;
    HotVec hot_;
    ColdVec cold_;
    U16Vec generation_;
    U16Vec freelist_;
    uint16_t capacity_ = 0;  // #229 M0b: 0 = un-Reserved (malloc fallback).
};

}  // namespace cairns
