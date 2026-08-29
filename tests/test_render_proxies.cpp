// tests/test_render_proxies.cpp
//
// SPEC: src/render/render_proxy.hpp + render_proxy_arrays.hpp +
//        frame_packet.hpp.
// TAGS: [spec][render][proxy]
//
// PODs for the per-frame scene-to-draw extract. LightProxy is skipped per
// user direction (no dynamic lights). What we pin: every proxy default
// shape, the ProxyFlags bit positions, ProxyArrays arena reset/push
// behavior, and FramePacket POD invariants.

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>

#include "render/frame_packet.hpp"
#include "render/render_proxy.hpp"
#include "render/render_proxy_arrays.hpp"
#include "util/cpu_arena.hpp"

using namespace cairns;

SCENARIO("MeshProxy defaults", "[spec][render][proxy]") {
    MeshProxy m{};
    REQUIRE(m.world_matrix == glm::mat4(1.0f));
    REQUIRE(m.first_primitive == 0u);
    REQUIRE(m.primitive_count == 0u);
    REQUIRE(m.skin == kInvalidSkin);
    REQUIRE(m.skin == 0xFFFFFFFFu);
    REQUIRE(m.layer_mask == 0xFFFFFFFFu);
    REQUIRE(m.flags == kProxyVisible);
    REQUIRE(m.entity_id == 0u);
    REQUIRE(m.pos.IsNull());
    REQUIRE(m.attr.IsNull());
    REQUIRE(m.index.IsNull());
}

SCENARIO("PrimitiveProxy defaults", "[spec][render][proxy]") {
    PrimitiveProxy p{};
    REQUIRE(p.first_index == 0u);
    REQUIRE(p.index_count == 0u);
    REQUIRE(p.vertex_offset == 0);
    REQUIRE(p.material_id.IsNull());
}

SCENARIO("ProxyFlags bit positions are pinned",
         "[spec][render][proxy][layout][regression]") {
    REQUIRE(kProxyVisible == (1u << 0));
    REQUIRE(kProxyCastShadow == (1u << 1));
    REQUIRE(kProxyLit == (1u << 2));
}

SCENARIO("LineProxy + PointProxy defaults", "[spec][render][proxy]") {
    LineProxy l{};
    REQUIRE(l.a == glm::vec3(0.0f));
    REQUIRE(l.b == glm::vec3(0.0f));
    REQUIRE(l.color == glm::vec4(1.0f));
    REQUIRE(l.layer_mask == 0xFFFFFFFFu);
    REQUIRE(l.flags == kProxyVisible);
    REQUIRE(l.is_2d_overlay == 0u);

    PointProxy pt{};
    REQUIRE(pt.position == glm::vec3(0.0f));
    REQUIRE(pt.color == glm::vec4(1.0f));
    REQUIRE(pt.size == 1.0f);
    REQUIRE(pt.is_2d_overlay == 0u);
}

SCENARIO("SkinnedAttachment Hot/Cold defaults", "[spec][render][proxy]") {
    SkinnedAttachment::Hot h{};
    REQUIRE(h.slice_offset == 0u);
    REQUIRE(h.joint_count == 0u);
    REQUIRE(h.time_offset == 0.0f);
    REQUIRE(h.time_scale == 1.0f);
    REQUIRE(h.gpu_prefab_header_idx == UINT32_MAX);
    REQUIRE(h.gpu_clip_duration == 1.0f);
    REQUIRE(h.pos_stream.IsNull());
}

SCENARIO("RenderProxyArrays Reset uses arena memory + cap enforcement",
         "[spec][render][proxy][arena]") {
    std::vector<uint8_t> slab(4 * 1024 * 1024);
    BumpArena arena;
    arena.Init(slab.data(), slab.size());
    RenderProxyArrays arr{};
    arr.Reset(arena, /*cap_meshes=*/8, /*cap_prims=*/16);
    REQUIRE(arr.meshes.size() == 0u);
    REQUIRE(arr.primitives.size() == 0u);
    MeshProxy m{};
    for (int i = 0; i < 8; ++i) {
        arr.meshes.push_back(m);
    }
    REQUIRE(arr.meshes.size() == 8u);
    // After Reset on the same arena, count goes back to 0.
    arr.Reset(arena, 4, 8);
    REQUIRE(arr.meshes.size() == 0u);
}

SCENARIO("FramePacket POD defaults", "[spec][render][proxy][frame_packet]") {
    FramePacket pkt{};
    REQUIRE(pkt.frame_idx == 0u);
    REQUIRE(pkt.slot == 0u);
    REQUIRE(pkt.view == glm::mat4(1.0f));
    REQUIRE(pkt.proj == glm::mat4(1.0f));
    REQUIRE(pkt.camera_pos == glm::vec3(0.0f));
    REQUIRE(pkt.camera_dir == glm::vec3(0.0f, 0.0f, -1.0f));
    REQUIRE(pkt.near_z == 0.1f);
    REQUIRE(pkt.far_z == 100.0f);
    REQUIRE(pkt.draws.empty());
    REQUIRE(pkt.sorted.empty());
    REQUIRE(pkt.resident_textures.empty());
}

SCENARIO("SkinBatchGpu POD defaults", "[spec][render][proxy][skin_batch]") {
    SkinBatchGpu b{};
    REQUIRE(b.mesh_set.IsNull());
    REQUIRE(b.mesh.IsNull());
    REQUIRE(b.first_palette_mat4 == 0u);
    REQUIRE(b.first_meta == 0u);
    REQUIRE(b.instance_count == 0u);
    REQUIRE(b.vertex_count == 0u);
    REQUIRE(b.joint_count == 0u);
    REQUIRE(b.workgroups == 0u);
}
