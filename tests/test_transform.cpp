// tests/test_transform.cpp
//
// SPEC: cairns::ComposeTRS / ComposeChild (src/scene/transform_math.hpp)
// TAGS: [spec][scene][transform]   implementation-order: scene data layer (8)
// BUILDING BLOCK OF: Tier G scenario 2 (hot reload placement), scenarios 3/4
//                    (multi-instance / multi-camera placement), ladder rungs 2+.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "scene/transform_math.hpp"

using cairns::ComposeChild;
using cairns::ComposeTRS;

SCENARIO("identity TRS is the identity matrix", "[spec][transform]") {
    const glm::mat4 m = ComposeTRS(glm::vec3(0), glm::quat(1, 0, 0, 0),
                                   glm::vec3(1));
    REQUIRE(m == glm::mat4(1.0f));
}

SCENARIO("translation lands in column 3", "[spec][transform]") {
    const glm::mat4 m = ComposeTRS(glm::vec3(3, 4, 5), glm::quat(1, 0, 0, 0),
                                   glm::vec3(1));
    REQUIRE(m[3] == glm::vec4(3, 4, 5, 1));
}

SCENARIO("scale multiplies the basis columns", "[spec][transform]") {
    const glm::mat4 m = ComposeTRS(glm::vec3(0), glm::quat(1, 0, 0, 0),
                                   glm::vec3(2, 3, 4));
    REQUIRE(m[0].x == Catch::Approx(2.0f));
    REQUIRE(m[1].y == Catch::Approx(3.0f));
    REQUIRE(m[2].z == Catch::Approx(4.0f));
}

SCENARIO("a transformed point matches T*R*S applied to it", "[spec][transform]") {
    const glm::quat r = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
    const glm::mat4 m = ComposeTRS(glm::vec3(10, 0, 0), r, glm::vec3(1));
    const glm::vec4 p = m * glm::vec4(1, 0, 0, 1);
    REQUIRE(p.x == Catch::Approx(10.0f));
    REQUIRE(p.y == Catch::Approx(1.0f));
}

SCENARIO("child world composes through the parent", "[spec][transform]") {
    const glm::mat4 parent =
        ComposeTRS(glm::vec3(10, 0, 0), glm::quat(1, 0, 0, 0), glm::vec3(1));
    const glm::mat4 child =
        ComposeChild(parent, glm::vec3(0, 1, 0), glm::quat(1, 0, 0, 0),
                     glm::vec3(1));
    const glm::vec4 origin = child * glm::vec4(0, 0, 0, 1);
    REQUIRE(origin.x == Catch::Approx(10.0f));
    REQUIRE(origin.y == Catch::Approx(1.0f));
}
