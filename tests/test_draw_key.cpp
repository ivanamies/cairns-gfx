// tests/test_draw_key.cpp
//
// SPEC: cairns::BuildDrawKey (src/util/draw_key.hpp)
// TAGS: [spec][render_graph][draw_key]   implementation-order: render graph (6)
// BUILDING BLOCK OF: Tier G scenarios 3 & 4 (multiple framebuffers / viewports).
//
// Sort precedence, most significant first:
//   fullscreen_layer > viewport > viewport_layer > translucency > depth > material

#include <catch2/catch_test_macros.hpp>

#include "util/draw_key.hpp"

using cairns::BuildDrawKey;

SCENARIO("viewport outranks material and depth", "[spec][draw_key]") {
    // vp0 with the worst possible material/depth still sorts before vp1 with
    // the best -- the property the side-by-side scenario depends on.
    const auto vp0_worst = BuildDrawKey(0x3FFFFFFF, 0xFFFFFF, 0, 0);
    const auto vp1_best = BuildDrawKey(0, 0, 0, 1);
    REQUIRE(vp0_worst < vp1_best);
}

SCENARIO("within a viewport, opaque sorts front-to-back by depth",
         "[spec][draw_key]") {
    const auto near_key = BuildDrawKey(5, 100, 0, 0);
    const auto far_key  = BuildDrawKey(5, 900, 0, 0);
    REQUIRE(near_key < far_key);
}

SCENARIO("translucency sorts after all opaque in the same viewport",
         "[spec][draw_key]") {
    const auto opaque      = BuildDrawKey(9, 0xFFFFFF, 0, 0);
    const auto translucent = BuildDrawKey(0, 0, 1, 0);
    REQUIRE(opaque < translucent);
}

SCENARIO("fullscreen layer outranks everything", "[spec][draw_key]") {
    const auto base = BuildDrawKey(0x3FFFFFFF, 0xFFFFFF, 3, 7, 7, 0);
    const auto top  = BuildDrawKey(0, 0, 0, 0, 0, 1);
    REQUIRE(base < top);
}

SCENARIO("fields are masked and cannot bleed into neighbors",
         "[spec][draw_key]") {
    // material is 30 bits; passing material with high bits set must not perturb
    // the depth field above it.
    const auto a = BuildDrawKey(0x3FFFFFFF, 1, 0, 0);
    const auto b = BuildDrawKey(0xFFFFFFFF, 1, 0, 0);
    REQUIRE(a == b);
}
