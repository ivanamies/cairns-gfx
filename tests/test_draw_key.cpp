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

// Phase B.13: pin EXACT bit positions and widths. The previous tests only
// asserted *relative* ordering, so a wrong-layout impl that swapped two
// fields could pass the precedence tests. These assertions pin the layout
// per the audit-named spec:
//   material[0-29] depth[30-53] translucency[54-55]
//   viewport_layer[56-58] viewport[59-61] fullscreen_layer[62-63]
SCENARIO("draw_key layout: each field lives at its exact bit range",
         "[spec][draw_key][layout][regression]") {
    GIVEN("a single bit set in each field") {
        WHEN("material = 1, others 0") {
            REQUIRE(BuildDrawKey(1u, 0, 0, 0, 0, 0) == (1ull << 0));
        }
        WHEN("depth = 1, others 0") {
            REQUIRE(BuildDrawKey(0, 1u, 0, 0, 0, 0) == (1ull << 30));
        }
        WHEN("translucency = 1, others 0") {
            REQUIRE(BuildDrawKey(0, 0, 1u, 0, 0, 0) == (1ull << 54));
        }
        WHEN("viewport_layer = 1, others 0") {
            REQUIRE(BuildDrawKey(0, 0, 0, 0, 1u, 0) == (1ull << 56));
        }
        WHEN("viewport = 1, others 0") {
            REQUIRE(BuildDrawKey(0, 0, 0, 1u, 0, 0) == (1ull << 59));
        }
        WHEN("fullscreen_layer = 1, others 0") {
            REQUIRE(BuildDrawKey(0, 0, 0, 0, 0, 1u) == (1ull << 62));
        }
    }
}

SCENARIO("draw_key field widths: max value of each field stays in its range",
         "[spec][draw_key][layout][regression]") {
    // material is 30 bits (mask 0x3FFFFFFF). Setting it to its max should
    // fill bits 0-29 and NOT spill into depth (bit 30+).
    GIVEN("each field set to its max masked value") {
        const auto k_material = BuildDrawKey(0x3FFFFFFFu, 0, 0, 0, 0, 0);
        REQUIRE(k_material == 0x3FFFFFFFull);
        REQUIRE((k_material & ~0x3FFFFFFFull) == 0ull);

        const auto k_depth = BuildDrawKey(0, 0xFFFFFFu, 0, 0, 0, 0);
        REQUIRE(k_depth == (0xFFFFFFull << 30));
        REQUIRE((k_depth & ((1ull << 30) - 1)) == 0ull);  // material bits clean
        REQUIRE((k_depth & ~(0xFFFFFFull << 30)) == 0ull); // no spill

        const auto k_tr = BuildDrawKey(0, 0, 0x3u, 0, 0, 0);
        REQUIRE(k_tr == (0x3ull << 54));

        const auto k_vpl = BuildDrawKey(0, 0, 0, 0, 0x7u, 0);
        REQUIRE(k_vpl == (0x7ull << 56));

        const auto k_vp = BuildDrawKey(0, 0, 0, 0x7u, 0, 0);
        REQUIRE(k_vp == (0x7ull << 59));

        const auto k_fs = BuildDrawKey(0, 0, 0, 0, 0, 0x3u);
        REQUIRE(k_fs == (0x3ull << 62));
    }
}

SCENARIO("viewport_layer outranks translucency",
         "[spec][draw_key][regression]") {
    // The audit named this exact gap: existing tests proved fullscreen >
    // viewport and depth > material, but nothing pitted viewport_layer
    // directly against translucency. A wrong-impl that swapped the two
    // would still pass the others.
    const auto trans_max = BuildDrawKey(0, 0, 0x3u, 0, 0, 0);
    const auto vpl_min = BuildDrawKey(0, 0, 0, 0, 1u, 0);
    REQUIRE(trans_max < vpl_min);  // viewport_layer always outranks
}
