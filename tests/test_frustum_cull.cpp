// tests/test_frustum_cull.cpp
//
// SPEC: cairns::ExtractFrustumPlanes / AabbOutsideFrustum / WorldAabb
//       (src/util/frustum.hpp)
// TAGS: [spec][cameras][frustum]   implementation-order: cameras (4)
// BUILDING BLOCK OF: Tier G scenario 5 (frustum cull draw-call counter).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

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
