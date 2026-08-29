// tests/test_memory_budget.cpp
//
// Tier S. The MemoryBudget single-source-of-sizes + the ChunkAllocator
// fixed-reservation hard cap ("only ever use that X GB").
// TAGS: [spec][memory]

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "util/chunk_allocator.hpp"
#include "util/memory_budget.hpp"

SCENARIO("MemoryBudget::Default sizes every domain", "[spec][memory]") {
    const cairns::MemoryBudget b = cairns::MemoryBudget::Default();
    THEN("every domain carries a non-zero reservation") {
        REQUIRE(b.cpu_persistent_bytes > 0);
        REQUIRE(b.cpu_frame_slab_bytes > 0);
        REQUIRE(b.gpu_resident_bytes > 0);
        REQUIRE(b.gpu_staging_ring_bytes > 0);
        REQUIRE(b.js_heap_bytes > 0);
        REQUIRE(b.ndjson_scratch_bytes > 0);
    }
    THEN("the CPU-persistent arena is at least the mobile 256 MB floor") {
        REQUIRE(b.cpu_persistent_bytes >= 256ull * 1024 * 1024);
    }
}

SCENARIO("ChunkAllocator fixed reservation is a hard cap", "[spec][memory]") {
    GIVEN("a reservation of 2 small (64 KB) chunks") {
        cairns::ChunkAllocator a;
        const uint32_t block = 64u * 1024;
        a.InitReserved(2ull * block, block);
        REQUIRE(a.IsReserved());
        REQUIRE(a.MaxChunks() == 2);

        WHEN("allocating until the reservation is exhausted") {
            std::vector<void*> live;
            bool hit_cap = false;
            for (int i = 0; i < 100000; ++i) {
                void* p = a.Allocate(1024, 16);
                if (p == nullptr) {
                    hit_cap = true;
                    break;
                }
                live.push_back(p);
            }
            THEN("it fails loud (null) instead of growing past the budget") {
                REQUIRE(hit_cap);
                REQUIRE(a.ChunkCount() <= 2);
            }
            // Free everything so the Deinit outstanding-allocs assert holds.
            for (void* p : live) {
                a.Free(p);
            }
        }
    }
}

SCENARIO("ChunkAllocator on-demand mode never caps", "[spec][memory]") {
    cairns::ChunkAllocator a;
    const uint32_t block = 64u * 1024;
    a.Init(block);  // NOT reserved
    REQUIRE_FALSE(a.IsReserved());
    std::vector<void*> live;
    for (int i = 0; i < 1000; ++i) {
        void* p = a.Allocate(1024, 16);
        REQUIRE(p != nullptr);  // grows on demand, no budget cap
        live.push_back(p);
    }
    REQUIRE(a.ChunkCount() > 2);  // grew well past a 2-chunk reservation
    for (void* p : live) {
        a.Free(p);
    }
}
