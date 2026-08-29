// tests/test_hud_stats.cpp
//
// SPEC: cairns::SlotAvgUs / UsToMs / MsToFps / HudStats (src/util/hud_stats.hpp)
// TAGS: [spec][profiling][hud]   implementation-order: profiling (2)
// BUILDING BLOCK OF: Tier G scenario 6 (imgui stability).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "util/hud_stats.hpp"

using namespace cairns;

SCENARIO("slot averaging divides accumulated time by iterations",
         "[spec][hud]") {
    REQUIRE(SlotAvgUs(16600, 1) == 16600);
    REQUIRE(SlotAvgUs(33200, 2) == 16600);
}

SCENARIO("zero iterations and zero ms are safe (no NaN/inf in the overlay)",
         "[spec][hud]") {
    REQUIRE(SlotAvgUs(0, 0) == 0);          // not a divide-by-zero
    REQUIRE(MsToFps(0.0f) == 0.0f);         // not inf
}

SCENARIO("us->ms->fps conversions are exact at the 60fps anchor",
         "[spec][hud]") {
    REQUIRE(UsToMs(16600) == Catch::Approx(16.6f));
    REQUIRE(MsToFps(16.6f) == Catch::Approx(60.24f).margin(0.5f));
    REQUIRE(MsToFps(1000.0f / 60.0f) == Catch::Approx(60.0f));
}

SCENARIO("the mock HUD is the fixed 60fps fixture the imgui test seeds",
         "[spec][hud][regression]") {
    const HudStats m = HudStats::Mock();
    REQUIRE(m.fps == 60.0f);
    REQUIRE(m.cpu_ms == Catch::Approx(16.6f));
    for (float f : m.frame_ms) {
        REQUIRE(f == Catch::Approx(16.6f));  // flat graph
    }
}

SCENARIO("HudFromTimer composes the pure pieces", "[spec][hud]") {
    std::array<float, HudStats::kGraph> g{};
    g.fill(16.6f);
    const HudStats s = HudFromTimer(33200, 2, g, 3);
    REQUIRE(s.cpu_ms == Catch::Approx(16.6f));
    REQUIRE(s.fps == Catch::Approx(60.24f).margin(0.5f));
    REQUIRE(s.graph_head == 3);
}
