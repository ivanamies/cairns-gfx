// tests/test_scene_draw_ranges.cpp
//
// TIER S spec: multi-scene per-viewport draw fan-out. CarveSceneDrawRanges
// turns each scene's [mesh) range in the proxy union into its [draw) slice of
// the shared sorted draw list; each viewport then renders only its own scene's
// slice. Guards the regression where two viewports on two scenes render the
// SAME content -- or nothing.
// TAGS: [spec][render][scene_draw_ranges]

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "render/scene_draw_ranges.hpp"

using cairns::CarveSceneDrawRanges;
using cairns::SceneDrawSpan;

namespace {

// proxy_first_draw[m] = prefix sum of per-mesh primitive (draw) counts; the
// engine computes the same array before carving.
std::vector<uint32_t> PrefixSum(const std::vector<uint32_t>& prim_counts,
                                uint32_t& total) {
    std::vector<uint32_t> first(prim_counts.size());
    uint32_t acc = 0;
    for (size_t i = 0; i < prim_counts.size(); ++i) {
        first[i] = acc;
        acc += prim_counts[i];
    }
    total = acc;
    return first;
}

}  // namespace

SCENARIO("one scene, one viewport: the viewport's draws cover the whole list",
         "[spec][render][scene_draw_ranges]") {
    // One scene of 3 meshes with [1,2,3] primitives -> 6 draws.
    uint32_t total = 0;
    const std::vector<uint32_t> first = PrefixSum({1, 2, 3}, total);
    REQUIRE(total == 6);

    std::array<SceneDrawSpan, 1> scenes{};
    scenes[0].mesh_lo = 0;
    scenes[0].mesh_hi = 3;  // one past the last mesh
    CarveSceneDrawRanges(first, total, scenes);

    // The single viewport binds scene 0 -> its sub-span is the whole draw list.
    REQUIRE(scenes[0].draw_lo == 0);
    REQUIRE(scenes[0].draw_hi == 6);
    REQUIRE(scenes[0].draw_hi - scenes[0].draw_lo == total);
}

SCENARIO("two scenes, two viewports: each viewport draws only its own scene",
         "[spec][render][scene_draw_ranges]") {
    // Proxy union: scene A meshes [0,2), scene B meshes [2,5). 5 meshes, one
    // primitive each -> 5 draws; A owns 2, B owns 3.
    uint32_t total = 0;
    const std::vector<uint32_t> first = PrefixSum({1, 1, 1, 1, 1}, total);
    REQUIRE(total == 5);

    std::array<SceneDrawSpan, 2> scenes{};
    scenes[0].mesh_lo = 0;  // scene A (vp0)
    scenes[0].mesh_hi = 2;
    scenes[1].mesh_lo = 2;  // scene B (vp1)
    scenes[1].mesh_hi = 5;
    CarveSceneDrawRanges(first, total, scenes);

    // vp0 -> scene A draws [0,2); vp1 -> scene B draws [2,5).
    REQUIRE(scenes[0].draw_lo == 0);
    REQUIRE(scenes[0].draw_hi == 2);
    REQUIRE(scenes[1].draw_lo == 2);
    REQUIRE(scenes[1].draw_hi == 5);

    // Disjoint and partition the whole list: A.hi == B.lo, union == [0,total).
    REQUIRE(scenes[0].draw_hi == scenes[1].draw_lo);
    REQUIRE(scenes[0].draw_lo == 0);
    REQUIRE(scenes[1].draw_hi == total);
    // The two viewports render DIFFERENT, non-overlapping draws.
    REQUIRE(scenes[0].draw_lo != scenes[1].draw_lo);
}

SCENARIO("an empty scene gets an empty draw range, not the whole list",
         "[spec][render][scene_draw_ranges]") {
    // Scene B is empty (mesh_lo == mesh_hi) -> draw_lo == draw_hi: it renders
    // nothing, instead of accidentally inheriting the whole list.
    uint32_t total = 0;
    const std::vector<uint32_t> first = PrefixSum({2, 3}, total);  // 2 meshes
    REQUIRE(total == 5);

    std::array<SceneDrawSpan, 2> scenes{};
    scenes[0].mesh_lo = 0;
    scenes[0].mesh_hi = 2;
    scenes[1].mesh_lo = 2;  // empty: starts past the last mesh
    scenes[1].mesh_hi = 2;
    CarveSceneDrawRanges(first, total, scenes);

    REQUIRE(scenes[0].draw_lo == 0);
    REQUIRE(scenes[0].draw_hi == 5);
    REQUIRE(scenes[1].draw_lo == scenes[1].draw_hi);  // empty span
}
