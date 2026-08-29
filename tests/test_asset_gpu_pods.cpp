// tests/test_asset_gpu_pods.cpp
//
// SPEC: the pure-CPU asset/upload PODs (gpu_anim_types,
// material_gpu, sampler enums). gltf_loader + scene_gpu + animation_runtime
// require the engine + glm gltf paths; those go in cairns_golden_tests.
// TAGS: [spec][asset][gpu]
//
// What this pins: byte sizes (shader layout regression), default values, enum
// stability. A reimplementer that changed any struct's layout would break
// the GPU compute kernels that consume them.

#include <catch2/catch_test_macros.hpp>

// NOTE: sampler.hpp pulls gfx_api.hpp which pulls metal-cpp Foundation
// headers -- can't compile in the pure-CPU spec target. The sampler
// enum-stability test moves to cairns_golden_tests instead.
#include "util/gpu_anim_types.hpp"
#include "util/material_gpu.hpp"

using namespace cairns;

SCENARIO("GpuTRS default = neutral pose", "[spec][asset][gpu]") {
    GpuTRS trs{};
    REQUIRE(trs.T == glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
    REQUIRE(trs.R == glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));  // identity quat
    REQUIRE(trs.S == glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
}

SCENARIO("GpuTRS size matches three vec4s",
         "[spec][asset][gpu][layout][regression]") {
    REQUIRE(sizeof(GpuTRS) == 3 * sizeof(glm::vec4));
}

SCENARIO("GpuSceneHeader has the documented field count + size",
         "[spec][asset][gpu][layout][regression]") {
    // 16 uint32-equivalent slots = 64 bytes. The compute kernel
    // (anim_eval.comp) reads exactly this; a layout drift breaks animation.
    REQUIRE(sizeof(GpuSceneHeader) == 64u);
    GpuSceneHeader h{};
    REQUIRE(h.node_count == 0u);
    REQUIRE(h.joint_count == 0u);
    REQUIRE(h.mesh_node == -1);  // sentinel "no mesh node"
    REQUIRE(h.duration == 0.0f);
}

SCENARIO("GpuChannel + GpuSampler + GpuActorRecord layouts",
         "[spec][asset][gpu][layout][regression]") {
    REQUIRE(sizeof(GpuChannel) == 16u);
    REQUIRE(sizeof(GpuSampler) == 16u);
    REQUIRE(sizeof(GpuActorRecord) == 16u);

    GpuChannel ch{};
    REQUIRE(ch.node_idx == -1);
    REQUIRE(ch.sampler_idx == -1);

    GpuSampler s{};
    REQUIRE(s.count == 0u);
    REQUIRE(s.interp == 0u);

    GpuActorRecord ar{};
    REQUIRE(ar.time == 0.0f);
}

SCENARIO("MaterialGpu is a std140 64-byte block with sane defaults",
         "[spec][asset][gpu][material]") {
    rhi::MaterialGpu m{};
    // ids.x/.y carry the tex_color/sampler slots; invalid by default.
    REQUIRE(m.ids.x == 0xFFFFFFFFu);
    REQUIRE(m.ids.y == 0xFFFFFFFFu);
    REQUIRE(m.base_color.r == 1.0f);
    REQUIRE(m.base_color.a == 1.0f);
    REQUIRE(m.params0.x == 0.0f);
    // Four vec4-aligned rows: uploadable as a UBO on all three backends.
    REQUIRE(sizeof(rhi::MaterialGpu) == 64u);
}

SCENARIO("DrawTmp default entity_id is 0 (background marker)",
         "[spec][asset][gpu][material][regression]") {
    rhi::DrawTmp d{};
    REQUIRE(d.entity_id == 0u);  // 0 means "background" in the R32U pass
    REQUIRE(d.mesh_id == 0xFFFFFFFFu);
}

// sampler enum spec deferred -- see comment at top of file.
