// tests/test_range_pool.cpp
//
// SPEC: cairns::RangePool + cairns::PoolSlice     (src/util/cpu_pool.hpp)
//       cairns::SkinPoolFitsDevice + friends      (src/util/device_caps.hpp)
// TAGS: [spec][util][range_pool][acton][device_fit]
//
// RangePool is an OFFSET-only suballocator over an externally owned buffer --
// the canonical user is `skin_output_pool_`. Capacity is in ELEMENT units
// (skin: vec4, 16 B). The engine binds at byte offset slice.offset * 16; that
// byte offset is what the device's maxStorageBufferRange caps.
//
// The bug this spec exists to forbid: kSkinOutputBytes = 1 GB on Adreno 730
// (256 MB SSBO range) -> writes past 256 MB silently no-op -> "half the
// 500-actor grid garbled." The landed fix caps Android pool at 256 MB.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "util/cpu_pool.hpp"
#include "util/device_caps.hpp"

namespace {
constexpr uint32_t kAdrenoStorageRange = 256u * 1024u * 1024u;  // S22 floor
constexpr uint32_t kDesktopSkinBytes   = 1024u * 1024u * 1024u; // 1 GB
constexpr uint32_t kMobileSkinBytes    = 256u * 1024u * 1024u;  // the fix

// True iff [a.offset, a.offset+a.count) and [b.offset, b.offset+b.count) touch.
bool Overlaps(const cairns::PoolSlice& a, const cairns::PoolSlice& b) {
    const uint64_t a0 = a.offset;
    const uint64_t a1 = a0 + a.count;
    const uint64_t b0 = b.offset;
    const uint64_t b1 = b0 + b.count;
    return a0 < b1 && b0 < a1;
}
}  // namespace

SCENARIO("a default PoolSlice is invalid", "[spec][util][range_pool]") {
    cairns::PoolSlice s{};
    REQUIRE_FALSE(s.IsValid());
    REQUIRE(s.offset == cairns::PoolSlice::kInvalid);
}

SCENARIO("a fresh pool reports its capacity and hands out a valid slice",
         "[spec][util][range_pool]") {
    GIVEN("a pool of 1024 units") {
        cairns::RangePool pool;
        pool.Init(1024u);
        REQUIRE(pool.Capacity() == 1024u);

        WHEN("64 units are allocated") {
            cairns::PoolSlice s = pool.Alloc(64u);
            THEN("the slice is valid and records the request") {
                REQUIRE(s.IsValid());
                REQUIRE(s.count == 64u);
                REQUIRE(s.offset + s.count <= pool.Capacity());
            }
        }
    }
}

SCENARIO("live allocations never overlap", "[spec][util][range_pool][invariant]") {
    GIVEN("a pool and a batch of varied allocations") {
        cairns::RangePool pool;
        pool.Init(4096u);
        std::vector<cairns::PoolSlice> live;
        for (uint32_t n : {30u, 7u, 256u, 1u, 64u, 100u, 9u}) {
            cairns::PoolSlice s = pool.Alloc(n);
            REQUIRE(s.IsValid());
            for (const auto& other : live) {
                REQUIRE_FALSE(Overlaps(s, other));
            }
            live.push_back(s);
        }
    }
}

SCENARIO("exhaustion returns an invalid slice -- the pool never aborts",
         "[spec][util][range_pool][regression]") {
    GIVEN("a pool with only 100 units") {
        cairns::RangePool pool;
        pool.Init(100u);
        WHEN("a 101-unit allocation is requested") {
            cairns::PoolSlice s = pool.Alloc(101u);
            THEN("the pool signals failure by returning an invalid slice") {
                REQUIRE_FALSE(s.IsValid());
            }
        }
    }
}

SCENARIO("a freed range is reusable", "[spec][util][range_pool]") {
    GIVEN("a pool filled to capacity by one big slice") {
        cairns::RangePool pool;
        pool.Init(256u);
        cairns::PoolSlice big = pool.Alloc(256u);
        REQUIRE(big.IsValid());
        REQUIRE_FALSE(pool.Alloc(1u).IsValid());

        WHEN("the big slice is freed") {
            pool.Free(big);
            THEN("the space comes back") {
                cairns::PoolSlice again = pool.Alloc(256u);
                REQUIRE(again.IsValid());
            }
        }
    }
}

// ===========================================================================
// THE 256 MB BUG, as pure arithmetic.
// ===========================================================================

SCENARIO("skin slice byte offset is unit offset times the vec4 stride",
         "[spec][device_fit]") {
    REQUIRE(cairns::SkinSliceByteOffset(0u) == 0u);
    REQUIRE(cairns::SkinSliceByteOffset(1u) == 16u);
    // The exact threshold actor: the first unit offset whose byte offset
    // reaches Adreno's 256 MB range. 256MB / 16 = 16,777,216.
    REQUIRE(cairns::SkinSliceByteOffset(16u * 1024u * 1024u) ==
            static_cast<uint64_t>(kAdrenoStorageRange));
}

SCENARIO("a 1 GB skin pool does NOT fit an Adreno 730 (the garble)",
         "[spec][device_fit][regression]") {
    GIVEN("Adreno caps with a 256 MB storage-buffer range") {
        cairns::DeviceCaps adreno{};
        adreno.max_storage_buffer_range = kAdrenoStorageRange;

        THEN("the desktop 1 GB pool is rejected") {
            REQUIRE_FALSE(cairns::SkinPoolFitsDevice(kDesktopSkinBytes, adreno));
        }
        AND_THEN("the mobile 256 MB cap is accepted -- this is the landed fix") {
            REQUIRE(cairns::SkinPoolFitsDevice(kMobileSkinBytes, adreno));
        }
        AND_THEN("the exact boundary fits (<= is the right relation, not <)") {
            adreno.max_storage_buffer_range = kMobileSkinBytes;
            REQUIRE(cairns::SkinPoolFitsDevice(kMobileSkinBytes, adreno));
            REQUIRE_FALSE(cairns::SkinPoolFitsDevice(kMobileSkinBytes + 1u, adreno));
        }
    }
}

SCENARIO("a desktop 1 GB pool fits a desktop-class device",
         "[spec][device_fit]") {
    cairns::DeviceCaps desktop{};
    // ~2 GB - 1: just under UINT32_MAX (uint32 cap field).
    desktop.max_storage_buffer_range = 2u * 1024u * 1024u * 1024u - 1u;
    REQUIRE(cairns::SkinPoolFitsDevice(kDesktopSkinBytes, desktop));
}

SCENARIO("no slice the pool returns can address past a fitting device",
         "[spec][device_fit][invariant]") {
    GIVEN("a 256 MB-equivalent pool on a 256 MB Adreno") {
        cairns::DeviceCaps adreno{};
        adreno.max_storage_buffer_range = kAdrenoStorageRange;
        const uint32_t capacity_units = kMobileSkinBytes / cairns::kSkinVertexStride;
        REQUIRE(cairns::SkinPoolFitsDevice(kMobileSkinBytes, adreno));

        cairns::RangePool pool;
        pool.Init(capacity_units);

        WHEN("slices are allocated until the pool is full") {
            cairns::PoolSlice s;
            uint64_t max_end_byte = 0;
            while ((s = pool.Alloc(30u * 1024u)).IsValid()) {
                const uint64_t end_byte =
                    cairns::SkinSliceByteOffset(s.offset) +
                    static_cast<uint64_t>(s.count) * cairns::kSkinVertexStride;
                max_end_byte = end_byte > max_end_byte ? end_byte : max_end_byte;
            }
            THEN("the furthest byte any slice addresses is within the device range") {
                REQUIRE(max_end_byte <= adreno.max_storage_buffer_range);
            }
        }
    }
}
