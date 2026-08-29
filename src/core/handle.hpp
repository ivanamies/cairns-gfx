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

#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

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
    std::vector<typename T::Hot> hot_;
    std::vector<typename T::Cold> cold_;
    std::vector<uint16_t> generation_;
    std::vector<uint16_t> freelist_;
};

}  // namespace cairns
