// tests/test_frustum_cull.cpp
//
// SPEC: cairns::ExtractFrustumPlanes / AabbOutsideFrustum / WorldAabb
//       (src/util/frustum.hpp)
// TAGS: [spec][cameras][frustum]   implementation-order: cameras (4)
// BUILDING BLOCK OF: Tier G scenario 5 (frustum cull draw-call counter).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "util/frustum.hpp"

namespace {
// A camera at +Z looking at the origin, 60deg fovy, 1:1, zero-to-one depth.
glm::mat4 TestViewProj() {
    const glm::mat4 view =
        glm::lookAt(glm::vec3(0, 0, 5), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    proj[1][1] *= -1.0f;  // GL->Vulkan Y flip, as the engine uses
    return proj * view;
}
cairns::Aabb UnitBoxAt(glm::vec3 center) {
    return {center - glm::vec3(0.5f), center + glm::vec3(0.5f)};
}
}  // namespace

SCENARIO("a box at the camera target is inside the frustum", "[spec][frustum]") {
    const auto pl = cairns::ExtractFrustumPlanes(TestViewProj());
    const auto box = UnitBoxAt({0, 0, 0});
    REQUIRE_FALSE(cairns::AabbOutsideFrustum(pl, box.min, box.max));
}

SCENARIO("a box far to the side is outside the frustum", "[spec][frustum]") {
    const auto pl = cairns::ExtractFrustumPlanes(TestViewProj());
    REQUIRE(cairns::AabbOutsideFrustum(pl, glm::vec3(99, 0, 0),
                                       glm::vec3(100, 1, 1)));
}

SCENARIO("a box behind the camera is outside", "[spec][frustum]") {
    const auto pl = cairns::ExtractFrustumPlanes(TestViewProj());
    const auto box = UnitBoxAt({0, 0, 50});  // behind eye at z=5 looking -z
    REQUIRE(cairns::AabbOutsideFrustum(pl, box.min, box.max));
}

SCENARIO("a degenerate bind-pose AABB is never culled (#222 S.3 sentinel)",
         "[spec][frustum][regression]") {
    const auto pl = cairns::ExtractFrustumPlanes(TestViewProj());
    // min > max => "unmeasured"; WorldAabb returns a huge box.
    const cairns::Aabb world =
        cairns::WorldAabb(glm::vec3(1), glm::vec3(-1), glm::mat4(1.0f));
    REQUIRE_FALSE(cairns::AabbOutsideFrustum(pl, world.min, world.max));
}

SCENARIO("WorldAabb pads about the center and follows the world transform",
         "[spec][frustum]") {
    const glm::vec3 mn(-1);
    const glm::vec3 mx(1);  // 2x2x2, center origin
    GIVEN("a 1.5x pad and a +10x translation") {
        const glm::mat4 world =
            glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
        const cairns::Aabb a = cairns::WorldAabb(mn, mx, world, 1.5f);
        THEN("the box is centered at the translated origin and grown by 1.5x") {
            REQUIRE(a.min.x == Catch::Approx(10.0f - 1.5f));
            REQUIRE(a.max.x == Catch::Approx(10.0f + 1.5f));
        }
    }
}

// Pin the Gribb-Hartmann plane math directly. In/out decisions on
// hand-picked points alone are also satisfied by a distance-from-eye
// sphere cull (entirely wrong math). With the identity view_proj the
// frustum should be the unit cube (planes at +/-1 on each axis with
// outward-pointing normals).
SCENARIO("ExtractFrustumPlanes on identity view_proj = unit cube",
         "[spec][frustum][layout][regression]") {
    const auto pl = cairns::ExtractFrustumPlanes(glm::mat4(1.0f));
    // Order: L, R, B, T, N, F (per FrustumPlanes typedef).
    // For identity view_proj after Gribb-Hartmann + normalize:
    //  left  : (1, 0, 0,  1)
    //  right : (-1,0, 0,  1)
    //  bottom: (0, 1, 0,  1)
    //  top   : (0,-1, 0,  1)
    //  near  : (0, 0, 1,  1)
    //  far   : (0, 0,-1,  1)
    auto eq = [](const glm::vec4& got, const glm::vec4& want) {
        const float tol = 1e-5f;
        return std::fabs(got.x - want.x) < tol &&
               std::fabs(got.y - want.y) < tol &&
               std::fabs(got.z - want.z) < tol &&
               std::fabs(got.w - want.w) < tol;
    };
    REQUIRE(eq(pl[0], glm::vec4( 1,  0,  0, 1)));   // left
    REQUIRE(eq(pl[1], glm::vec4(-1,  0,  0, 1)));   // right
    REQUIRE(eq(pl[2], glm::vec4( 0,  1,  0, 1)));   // bottom
    REQUIRE(eq(pl[3], glm::vec4( 0, -1,  0, 1)));   // top
    REQUIRE(eq(pl[4], glm::vec4( 0,  0,  1, 1)));   // near
    REQUIRE(eq(pl[5], glm::vec4( 0,  0, -1, 1)));   // far
}

SCENARIO("positive-vertex selection picks the correct AABB corner",
         "[spec][frustum][regression]") {
    // A center-only or sphere cull would pass the plain in/out
    // scenarios. This test exercises the positive-
    // vertex rule directly: a box straddling a frustum plane with the
    // "positive vertex" outside the plane SHOULD be culled; the same
    // box with positive vertex inside should NOT.
    //
    // Use identity view_proj. Left plane is (1,0,0,1) -- normal (+x),
    // d=1. A point p is inside iff dot(n,p)+d >= 0, i.e. x >= -1.
    const auto pl = cairns::ExtractFrustumPlanes(glm::mat4(1.0f));

    // AABB whose maxx = -2 -- entirely past the left plane.
    // positive vertex for left plane (n = +x) picks max.x. With max.x = -2,
    // dot(n,p)+d = -2+1 = -1 < 0 ⇒ outside. Culled.
    REQUIRE(cairns::AabbOutsideFrustum(pl, glm::vec3(-3, 0, 0),
                                            glm::vec3(-2, 1, 1)));

    // AABB whose maxx = 0, minx = -2 -- straddles the left plane. The
    // positive vertex for n=+x is max.x=0 ⇒ dot(n,p)+d = 0+1 = 1 >= 0.
    // Inside that plane. (Sphere-from-eye cull would still mark it
    // outside if it tested center against radius, so this discriminates.)
    REQUIRE_FALSE(cairns::AabbOutsideFrustum(pl, glm::vec3(-2, 0, 0),
                                                  glm::vec3(0, 1, 1)));

    // Same straddling box, BUT mirrored: minx=0, maxx=2. positive vertex
    // for the right plane (n = -x) is min.x = 0 ⇒ dot(n,p)+d =
    // -(0)+1 = 1 >= 0 inside. Confirms positive-vertex flip works on
    // both axes.
    REQUIRE_FALSE(cairns::AabbOutsideFrustum(pl, glm::vec3(0, 0, 0),
                                                  glm::vec3(2, 1, 1)));

    // The same box translated past the right plane: minx=2, maxx=3.
    // positive vertex for right (n=-x) = min.x=2 ⇒ -2+1 = -1 < 0 outside.
    REQUIRE(cairns::AabbOutsideFrustum(pl, glm::vec3(2, 0, 0),
                                            glm::vec3(3, 1, 1)));
}

SCENARIO("plane normalization: a scaled view_proj does not change cull",
         "[spec][frustum][regression]") {
    // If ExtractFrustumPlanes skipped normalization, scaling the matrix
    // would change cull decisions for boundary points. The spec says
    // normalize -- this test pins it.
    const glm::mat4 scaled = glm::mat4(7.5f);
    const auto pl_scaled = cairns::ExtractFrustumPlanes(scaled);
    const auto pl_id = cairns::ExtractFrustumPlanes(glm::mat4(1.0f));
    for (int i = 0; i < 6; ++i) {
        // Plane direction unchanged; d (w) likewise.
        REQUIRE(std::fabs(pl_scaled[i].x - pl_id[i].x) < 1e-5f);
        REQUIRE(std::fabs(pl_scaled[i].y - pl_id[i].y) < 1e-5f);
        REQUIRE(std::fabs(pl_scaled[i].z - pl_id[i].z) < 1e-5f);
    }
}

SCENARIO("cull decision sums to the expected drawn count",
         "[spec][frustum]") {
    const auto pl = cairns::ExtractFrustumPlanes(TestViewProj());
    const glm::vec3 inside_centers[] = {{0, 0, 0}, {0.5f, 0.5f, 0}, {-0.5f, 0, 1}};
    const glm::vec3 outside_centers[] = {{100, 0, 0}, {0, 100, 0}, {0, 0, 80}};
    uint32_t drawn = 0;
    for (auto c : inside_centers) {
        auto b = UnitBoxAt(c);
        if (!cairns::AabbOutsideFrustum(pl, b.min, b.max)) {
            ++drawn;
        }
    }
    for (auto c : outside_centers) {
        auto b = UnitBoxAt(c);
        if (!cairns::AabbOutsideFrustum(pl, b.min, b.max)) {
            ++drawn;
        }
    }
    REQUIRE(drawn == 3);
}
