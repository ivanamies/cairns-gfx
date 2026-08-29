// util/print_allocator.hpp
//
// Compile-time-tagged std::allocator wrapper that prints every
// allocate/deallocate so we can triangulate STL heap activity against
// the [LOAD]/[RELOAD]/[STEADY] markers in src/engine.hpp.
//
// DIAGNOSTIC tool, not a migration tool. Wrapping a vector with
// print_allocator<T, Tag> does NOT change its memory source -- it
// still goes through std::allocator -- it just emits a tagged fprintf
// on every (de)allocation.
//
// Each STL container we want to observe carries a compile-time tag
// type identifying its owner + field. The tag type's static name()
// returns the literal printed on every event:
//   "[ALLOC] Engine::prefab_ids_ +16*4=64 p=0x6000..."
//   "[ALLOC] Engine::prefab_ids_ -16*4=64 p=0x6000..."
//
// Once we have the log we can:
//   1. See which containers are allocation hotspots during LOAD.
//   2. See which containers steady-state at zero allocations (good --
//      they live in pool storage already or never grow during frames).
//   3. See which containers leak allocations across [STEADY] frames
//      (bad -- those are candidates for arena / pool migration).
//
// Toggled via CAIRNS_ALLOC_TRACE:
//   #define CAIRNS_ALLOC_TRACE 1    -- prints on each (de)alloc
//   #define CAIRNS_ALLOC_TRACE 0    -- silent, identical to std::allocator
//
// Defining a tag type + the matching Vec alias:
//   struct kTagPrefabIds { static constexpr const char* name() {
//       return "Engine::prefab_ids_"; } };
//   using PrefabIdsVec = std::vector<PrefabId,
//       cairns::print_allocator<PrefabId, kTagPrefabIds>>;
//
// Then declare the field:
//   PrefabIdsVec prefab_ids_;
//
// Every push_back / resize / clear goes through print_allocator and
// emits a tagged line on stderr in CAIRNS_ALLOC_TRACE=1 builds.

#pragma once

#include "util/define.hpp"

#include <cstddef>
#include <cstdio>
#include <memory>

#ifndef CAIRNS_ALLOC_TRACE
#define CAIRNS_ALLOC_TRACE 0
#endif

namespace cairns {

template <typename T, typename TagT>
class print_allocator {
public:
    using value_type = T;

    print_allocator() noexcept = default;

    template <typename U>
    print_allocator(const print_allocator<U, TagT>&) noexcept {}

    template <typename U>
    struct rebind {
        using other = print_allocator<U, TagT>;
    };

    [[nodiscard]] T* allocate(std::size_t n) {
        T* p = std::allocator<T>{}.allocate(n);
#if CAIRNS_ALLOC_TRACE
        std::fprintf(stderr, "[ALLOC] %s +%zu*%zu=%zu p=%p\n",
                      TagT::name(), n, sizeof(T), n * sizeof(T),
                      static_cast<void*>(p));
#endif
        return p;
    }

    void deallocate(T* p, std::size_t n) noexcept {
#if CAIRNS_ALLOC_TRACE
        std::fprintf(stderr, "[ALLOC] %s -%zu*%zu=%zu p=%p\n",
                      TagT::name(), n, sizeof(T), n * sizeof(T),
                      static_cast<void*>(p));
#endif
        std::allocator<T>{}.deallocate(p, n);
    }

    // Same tag -> same allocator; different tag -> different allocator.
    // Containers cannot swap storage across different tags (which is
    // what we want -- the tag *is* the identity).
    template <typename U>
    bool operator==(const print_allocator<U, TagT>&) const noexcept {
        return true;
    }
    template <typename U, typename OtherTagT>
    bool operator!=(const print_allocator<U, OtherTagT>&) const noexcept {
        return true;
    }
};

}  // namespace cairns
