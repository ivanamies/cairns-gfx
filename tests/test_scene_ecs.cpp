// tests/test_scene_ecs.cpp
//
// SPEC: src/scene/* PODs (G.1 of the test tech tree).
// TAGS: [spec][scene]
//
// Pure-CPU contract on the data-oriented core: Scene::Hot/Cold POD shapes,
// every component's default layout + invariants, EntityRef anti-singleton
// contract.

#include <catch2/catch_test_macros.hpp>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "scene/components.hpp"
#include "scene/entity_ref.hpp"
#include "scene/world.hpp"

using namespace cairns;

SCENARIO("Scene Hot/Cold default state", "[spec][scene][world]") {
    Scene::Hot h{};
    Scene::Cold c{};
    REQUIRE(h.root_transform == glm::mat4(1.0f));
    REQUIRE(h.dirty == true);
    REQUIRE(h.proxy_slot == 0u);
    REQUIRE(c.registry.storage<entt::entity>().size() == 0u);
}

SCENARIO("Transform component defaults to identity TRS",
         "[spec][scene][components]") {
    Transform t{};
    REQUIRE(t.t == glm::vec3(0.0f));
    REQUIRE(t.r.w == 1.0f);
    REQUIRE(t.r.x == 0.0f);
    REQUIRE(t.r.y == 0.0f);
    REQUIRE(t.r.z == 0.0f);
    REQUIRE(t.s == glm::vec3(1.0f));
}

SCENARIO("WorldTransform defaults to identity matrix",
         "[spec][scene][components]") {
    WorldTransform w{};
    REQUIRE(w.world == glm::mat4(1.0f));
}

SCENARIO("Renderable defaults to all-layers-visible + zero flags",
         "[spec][scene][components]") {
    Renderable r{};
    REQUIRE(r.layer_mask == 0xFFFFFFFFu);
    REQUIRE(r.flags == 0u);
}

SCENARIO("Parent default is entt::null", "[spec][scene][components]") {
    Parent p{};
    REQUIRE(static_cast<bool>(p.value == entt::null));
}

SCENARIO("CameraComponent defaults", "[spec][scene][components]") {
    CameraComponent c{};
    // Default fov_y = 90 degrees in radians.
    REQUIRE(c.fov_y_rad > 1.57f);
    REQUIRE(c.fov_y_rad < 1.58f);
    REQUIRE(c.near_z == 0.1f);
    REQUIRE(c.far_z == 100.0f);
    REQUIRE(c.is_main == false);
}

SCENARIO("AssetRef carries an AssetId by value",
         "[spec][scene][components]") {
    AssetRef a{};
    // AssetId is a Handle<...> -- default null.
    REQUIRE(a.asset.IsNull());
}

SCENARIO("EntityRef anti-singleton contract", "[spec][scene][entity_ref]") {
    // Default is invalid: null scene + entt::null.
    EntityRef ref{};
    REQUIRE_FALSE(ref.valid());
    REQUIRE(static_cast<bool>(ref.entity == entt::null));
    REQUIRE(ref.scene.IsNull());
    // Populated EntityRef rejects null on either axis.
    EntityRef good{};
    good.scene = SceneId{0, 1};  // a real handle
    REQUIRE_FALSE(good.valid());  // entity still null
    good.entity = static_cast<entt::entity>(0);
    REQUIRE(good.valid());
}

// Layout regression: bit-stable component sizes. A reimplementer that
// chose different storage would change these.
SCENARIO("scene component sizes are bit-stable",
         "[spec][scene][components][regression]") {
    REQUIRE(sizeof(WorldTransform) == sizeof(glm::mat4));
    REQUIRE(sizeof(Renderable) == 8u);  // 2 x uint32
    REQUIRE(sizeof(Parent) == sizeof(entt::entity));
    // Transform: vec3 + quat + vec3 = 12 + 16 + 12 = 40 (with padding may
    // align up to 48 on some ABIs).
    REQUIRE((sizeof(Transform) == 40u || sizeof(Transform) == 48u));
}
