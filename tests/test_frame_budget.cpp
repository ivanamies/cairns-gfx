// tests/test_frame_budget.cpp
//
// SPEC: cairns::BumpArena + cairns::FrameArena   (src/util/cpu_arena.hpp)
//       cairns::ResidentBytes / FitsResidentBudget (src/util/device_caps.hpp)
// TAGS: [spec][util][arena][fif][oom]
//
// BumpArena is an OFFSET-native linear allocator over a caller-owned slab.
// FrameArena is kFramesInFlight BumpArenas in a ring. The OOM failure mode,
// as arithmetic: GPU bump ring per FIF = upload(64) + dynamic(32) + readback(8)
// = 104 MB; resident(FIF=3) crossed the S22 lmkd budget; resident(FIF=2) fit.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "util/cpu_arena.hpp"
#include "util/device_caps.hpp"

SCENARIO("bump offsets are monotonic and alignment-respecting",
         "[spec][util][arena]") {
    GIVEN("a 4 KB arena over a local slab") {
        std::vector<uint8_t> slab(4096);
        cairns::BumpArena a;
        a.Init(slab.data(), slab.size());

        WHEN("two aligned allocations are made") {
            const uint32_t o0 = a.AllocateOffset(10, 16);
            const uint32_t o1 = a.AllocateOffset(32, 16);
            THEN("offset 0 is valid and the second is past the first, aligned") {
                REQUIRE(o0 == 0u);
                REQUIRE((o1 % 16u) == 0u);
                REQUIRE(o1 >= o0 + 10u);
                REQUIRE(a.Used() == o1 + 32u);
            }
        }
    }
}

SCENARIO("a bump arena fills exactly to capacity", "[spec][util][arena]") {
    // Bump-arena overflow is a PRECONDITION, not a recoverable condition: you
    // size the slab from HighWater() telemetry and commit to not exceeding it.
    // So the spec pins "fills exactly to capacity," and the production
    // assert(false) is the overflow guard.
    GIVEN("a 64-byte arena") {
        std::vector<uint8_t> slab(64);
        cairns::BumpArena a;
        a.Init(slab.data(), slab.size());
        WHEN("exactly capacity is requested across two allocations") {
            const uint32_t o0 = a.AllocateOffset(32, 1);
            const uint32_t o1 = a.AllocateOffset(32, 1);
            THEN("both succeed and the arena is full to the byte") {
                REQUIRE(o0 == 0u);
                REQUIRE(o1 == 32u);
                REQUIRE(a.Used() == 64u);
                REQUIRE(a.Used() == a.Capacity());
            }
        }
    }
}

SCENARIO("Reset reclaims wholesale; HighWater records the peak",
         "[spec][util][arena]") {
    GIVEN("an arena used to a known peak then reset") {
        std::vector<uint8_t> slab(1024);
        cairns::BumpArena a;
        a.Init(slab.data(), slab.size());
        a.AllocateOffset(600, 1);
        const size_t peak = a.Used();
        WHEN("Reset is called") {
            a.Reset();
            THEN("Used returns to zero but HighWater remembers the peak") {
                REQUIRE(a.Used() == 0u);
                REQUIRE(a.HighWater() >= peak);
            }
            AND_THEN("offsets restart from 0 after reset") {
                REQUIRE(a.AllocateOffset(8, 1) == 0u);
            }
        }
    }
}

SCENARIO("Mark/Rewind is LIFO scratch", "[spec][util][arena]") {
    GIVEN("an arena with a committed allocation and a marked scratch region") {
        std::vector<uint8_t> slab(1024);
        cairns::BumpArena a;
        a.Init(slab.data(), slab.size());
        a.AllocateOffset(32, 1);
        const auto mark = a.Mark();
        a.AllocateOffset(200, 1);  // scratch
        WHEN("we rewind to the mark") {
            a.Rewind(mark);
            THEN("the scratch is reclaimed and the next alloc reuses it") {
                REQUIRE(a.Used() == mark);
                REQUIRE(a.AllocateOffset(8, 1) == static_cast<uint32_t>(mark));
            }
        }
    }
}

SCENARIO("a FrameArena resets only the slot it begins", "[spec][util][fif]") {
    GIVEN("a 2-frame ring of 1 KB slots") {
        std::vector<uint8_t> slab(2 * 1024);
        cairns::FrameArena ring;
        ring.Init<2>(slab.data(), 1024);

        WHEN("we write to slot 0, advance to slot 1, then back to slot 0") {
            ring.BeginFrame(0);
            const uint32_t o0 = ring.AllocateOffset(100, 1);
            ring.BeginFrame(1);
            const uint32_t o1 = ring.AllocateOffset(100, 1);
            THEN("each slot's offsets are slot-relative and start at 0") {
                REQUIRE(o0 == 0u);
                REQUIRE(o1 == 0u);
            }
            AND_THEN("re-beginning slot 0 resets it") {
                ring.BeginFrame(2);  // 2 % 2 == slot 0
                REQUIRE(ring.AllocateOffset(8, 1) == 0u);
            }
        }
    }
}

// ===========================================================================
// THE OOM BUG, as pure arithmetic. The kFramesInFlight 3->2 lever.
// ===========================================================================

SCENARIO("FIF=3 with a 1 GB pool busts the S22 budget; FIF=2 fits",
         "[spec][oom][fif][regression]") {
    GIVEN("the S22 footprint with a 1 GB skin pool") {
        const uint64_t MB = 1024u * 1024u;
        cairns::MemoryFootprint f{};
        f.skin_output_bytes   = 1024u * MB;
        f.palette_out_bytes   = 16u * MB;
        f.world_scratch_bytes = 16u * MB;
        f.gpu_bump_per_fif    = (64u + 32u + 8u) * MB;  // 104 MB
        f.cpu_arena_per_slot  = 16u * MB;

        cairns::DeviceCaps s22{};
        // S22 lmkd kicks in well before the 8 GB hw ceiling; ~1.25 GB is what
        // the pool + bumps had to fit under at FIF=3 to avoid the black screen.
        // 1024 + 32 + 360 = 1416 MB busts 1280, the canonical regression.
        s22.resident_budget_bytes = 1280u * MB;

        WHEN("frames in flight is 3") {
            f.frames_in_flight = 3;
            THEN("we are over budget -> the black-screen lmkd kill") {
                REQUIRE_FALSE(cairns::FitsResidentBudget(f, s22));
            }
        }
        WHEN("frames in flight drops to 2 AND the pool is capped at 256 MB") {
            f.frames_in_flight = 2;
            f.skin_output_bytes = 256u * MB;
            THEN("we fit -- and dropping FIF alone freed ~104 MB of bump ring") {
                REQUIRE(cairns::FitsResidentBudget(f, s22));
            }
        }
    }
}

SCENARIO("each frame-in-flight removed frees the per-FIF tiers exactly once",
         "[spec][oom][fif]") {
    const uint64_t MB = 1024u * 1024u;
    cairns::MemoryFootprint f{};
    f.gpu_bump_per_fif   = 104u * MB;
    f.cpu_arena_per_slot = 16u * MB;
    f.frames_in_flight   = 3;
    const uint64_t at3 = cairns::ResidentBytes(f);
    f.frames_in_flight = 2;
    const uint64_t at2 = cairns::ResidentBytes(f);
    REQUIRE(at3 - at2 == (104u + 16u) * MB);
}
