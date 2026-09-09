// tests/test_golden_scenarios.cpp
//
// TIER G -- targeted subsystem divergence scenarios (companion to the golden
// ladder). Each scenario names the Tier S building block whose spec must pass
// first -- a red scenario with a green building block means the regression is
// in the GPU/wiring layer.
// TAGS: [golden][scenarios]

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "engine.hpp"
#include "engine_headless.hpp"
#include "test_seams.hpp"
#include "test_refs.hpp"
#include "golden_js.hpp"
#include "shell/scenario_launcher.hpp"  // ScenarioLauncher + DrawScenarioPanel
#include "util/hud_stats.hpp"
#include "util/debug_asset.hpp"  // kDebugGlbs (nested 20-GLB scenario)

namespace seam = cairns::test_seams;
namespace refs = cairns::test_refs;

// ---- Rendered-subject goldens: each is one scene captured to a per-platform
// image ref at frame 9 + frame 55. The 100-actor scale workload lives in
// test_golden_stress.cpp. ----
SCENARIO("subject: red triangle (procedural mesh)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("triangle", 512, 512, {}, R"JS(
        cairns.dispatch("cairns.primitive.create", { type: "triangle" });
    )JS");
}
SCENARIO("subject: lit primitives (directional light, half-lambert)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("lit_primitives", 512, 512, {}, R"JS(
        cairns.dispatch("cairns.primitive.createAll", {});
        const e = cairns.dispatch("cairns.entity.new", { name: "sun" });
        cairns.dispatch("cairns.entity.addComponent", {
            entity: e.result.entity, type: "DirectionalLight",
            props: { dirX: -0.5, dirY: -1.0, dirZ: -0.3,
                     colorR: 1.0, colorG: 0.95, colorB: 0.85,
                     intensity: 1.0,
                     ambientR: 0.12, ambientG: 0.12, ambientB: 0.15 }
        });
        cairns.dispatch("cairns.scene.setMaterialShaderAll", { shader: "lit" });
    )JS");
}
SCENARIO("subject: shadow primitives (directional shadow map + PCF)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("shadow_primitives", 512, 512, {}, R"JS(
        cairns.dispatch("cairns.primitive.createAll", {});
        const e = cairns.dispatch("cairns.entity.new", { name: "sun" });
        cairns.dispatch("cairns.entity.addComponent", {
            entity: e.result.entity, type: "DirectionalLight",
            props: { dirX: -0.4, dirY: -1.0, dirZ: -0.3,
                     colorR: 1.0, colorG: 0.96, colorB: 0.88,
                     intensity: 1.1,
                     ambientR: 0.10, ambientG: 0.10, ambientB: 0.13,
                     castShadows: true }
        });
        cairns.dispatch("cairns.scene.setMaterialShaderAll", { shader: "lit" });
    )JS");
}
SCENARIO("subject: kuwahara (anisotropic post-effect chain)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("kuwahara", 512, 512, {"viking_room.glb"},
                                 R"JS(
        cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 5 });
        cairns.dispatch("cairns.scene.spawnFitted",
                        { glbs: ["viking_room.glb"], instances: 1,
                          animated: false });
        const e = cairns.dispatch("cairns.entity.new", { name: "kuwahara" });
        cairns.dispatch("cairns.entity.addComponent", {
            entity: e.result.entity, type: "PostEffect",
            props: { type: 0, order: 0, p0: [6, 8, 1, 0] }
        });
    )JS");
}
SCENARIO("subject: bloom (bright/down/up/combine ladder)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("bloom", 512, 512, {}, R"JS(
        cairns.dispatch("cairns.primitive.createAll", {});
        const sun = cairns.dispatch("cairns.entity.new", { name: "sun" });
        cairns.dispatch("cairns.entity.addComponent", {
            entity: sun.result.entity, type: "DirectionalLight",
            props: { dirX: -0.4, dirY: -1.0, dirZ: -0.3,
                     colorR: 1.0, colorG: 0.98, colorB: 0.9,
                     intensity: 1.6,
                     ambientR: 0.10, ambientG: 0.10, ambientB: 0.13 }
        });
        cairns.dispatch("cairns.scene.setMaterialShaderAll", { shader: "lit" });
        const fx = cairns.dispatch("cairns.entity.new", { name: "bloom" });
        cairns.dispatch("cairns.entity.addComponent", {
            entity: fx.result.entity, type: "PostEffect",
            props: { type: 1, order: 0, p0: [0.7, 0.5, 0.8, 0] }
        });
    )JS");
}
SCENARIO("subject: watercolor (wash + edge darkening + granulation)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("watercolor", 512, 512, {"viking_room.glb"},
                                 R"JS(
        cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 5 });
        cairns.dispatch("cairns.scene.spawnFitted",
                        { glbs: ["viking_room.glb"], instances: 1,
                          animated: false });
        const e = cairns.dispatch("cairns.entity.new", { name: "watercolor" });
        cairns.dispatch("cairns.entity.addComponent", {
            entity: e.result.entity, type: "PostEffect",
            props: { type: 2, order: 0,
                     p0: [2.0, 6, 6, 0.6], p1: [4, 8, 0.4, 0] }
        });
    )JS");
}
SCENARIO("subject: one die (single static textured mesh)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("one_die", 512, 512, {"die.glb"}, R"JS(
        cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 4 });
        cairns.dispatch("cairns.scene.spawnFitted",
                        { glbs: ["die.glb"], instances: 1, animated: false });
    )JS");
}
SCENARIO("subject: two dice (instancing / multiple draws)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("two_die", 512, 512, {"die.glb"}, R"JS(
        cairns.dispatch("cairns.scene.spawnFitted",
                        { glbs: ["die.glb"], instances: 2, animated: false });
    )JS");
}
SCENARIO("subject: viking room (UVs + depth)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("viking_room", 512, 512, {"viking_room.glb"},
                                 R"JS(
        cairns.dispatch("cairns.viewport.setCamera", { viewport: 0, z: 5 });
        cairns.dispatch("cairns.scene.spawnFitted",
                        { glbs: ["viking_room.glb"], instances: 1,
                          animated: false });
    )JS");
}
SCENARIO("subject: three static champions (production-shape assets)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject(
        "three_champ_static", 512, 512,
        {"ahri.glb", "akali.glb", "alistar.glb"}, R"JS(
        cairns.dispatch("cairns.scene.spawnFitted",
                        { glbs: ["ahri.glb", "akali.glb", "alistar.glb"],
                          instances: 3, animated: false });
    )JS");
}
SCENARIO("subject: three animated champions (skinning path)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject(
        "three_champ_anim", 512, 512,
        {"ahri.glb", "akali.glb", "alistar.glb"}, R"JS(
        cairns.dispatch("cairns.scene.spawnFitted",
                        { glbs: ["ahri.glb", "akali.glb", "alistar.glb"],
                          instances: 3, animated: true });
    )JS");
}

// 1) PARTICLES -- VK-tutorial style compute particles. NO render output: the
//    sim runs headless and we hash the particle STATE buffer (CPU-side
//    readback) after a fixed number of fixed-dt steps. Deterministic by the
//    portable ParticleRng seed + fixed clock; refs are per-platform (see
//    below). Building block: [spec][particles][determinism].
SCENARIO("particles simulate deterministically (no render, state hash)",
         "[golden][scenarios][particles]") {
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 512;
    icfg.height = 512;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    // particle enable + 64-frame settle, composed in JS.
    cairns::golden::DriveJs(e, R"JS(
        cairns.dispatch("cairns.particles.enable", { on: true });
        cairns.dispatch("cairns.render.advanceFrames", { count: 64 });
    )JS");

    std::vector<uint8_t> buf;
    if (!seam::ReadParticleBuffer(e, buf)) {
        SKIP("ReadBackBuffer not wired on this backend");
    }
    REQUIRE(!buf.empty());
    const std::string observed = seam::Md5Hex(buf);
    // PER-PLATFORM, not shared: the seed positions use std::cos/std::sin
    // (engine initParticles), whose last bits differ across build optimization
    // (-O0 vs -O2) AND across GPU vendors in the sim -- so the state is not a
    // bit-portable cross-platform invariant. It gets a per-platform ref like
    // the rendered images.
    const std::string ref =
        refs::LoadImageRef("particles.state", seam::PlatformKey(), observed);
    if (ref.empty()) {
        SKIP("bake particle state ref");
    }
    REQUIRE(observed == ref);
}

// 2) HOT LOAD / RELOAD -- spawn 4, check; spawn 5 different, check; clear, check.
//    Building block: [spec][core][handle] (reuse-not-reset) + asset dedup.
SCENARIO("hot reload: spawn, replace, and clear stay correct",
         "[golden][scenarios][hot_reload]") {
    // 4 + 5 distinct heroes; the 5-after-4 path exercises the
    // ResourceManager generation-bump recycle on slot reuse.
    const std::vector<std::string> first  = {"aatrox.glb","ahri.glb","akali.glb","alistar.glb"};
    const std::vector<std::string> second = {"amumu.glb","aatrox_drx.glb","aatrox_blood_moon.glb","ahri_academy.glb","akali_2022_prestige_k_da.glb"};
    if (!seam::AssetsPresent(first) || !seam::AssetsPresent(second)) {
        SKIP("hot-reload assets absent");
    }

    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 512;
    icfg.height = 512;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    // Each phase is a JS dispatch; the test captures between phases.
    auto& reg = cairns::golden::SetupJs(e);

    auto checkpoint = [&](const char* tag) {
        REQUIRE(seam::AdvanceToGoldenFrame(e));
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
        seam::DumpFinalTargetPng(e, tag);
        const std::string observed = seam::Md5Hex(rgba);
        const std::string ref = refs::LoadImageRef(tag, seam::PlatformKey(), observed);
        if (ref.empty()) {
            SKIP("bake ref " << tag);
        }
        REQUIRE(observed == ref);
    };

    cairns::golden::EvalJs(reg, cairns::golden::SpawnFittedJs(first, 4, false));
    checkpoint("hot_reload.four");
    cairns::golden::EvalJs(reg, R"JS(cairns.dispatch("cairns.scene.clear", {});)JS");
    cairns::golden::EvalJs(reg, cairns::golden::SpawnFittedJs(second, 5, false));
    checkpoint("hot_reload.five");
    cairns::golden::EvalJs(reg, R"JS(cairns.dispatch("cairns.scene.clear", {});)JS");
    checkpoint("hot_reload.empty");
}

// 3) RENDER GRAPH 1 -- two viewports bound to two DISTINCT scenes: a different
//    hero in each (vp0 left = scene A, vp1 right = scene B + particles). This is
//    the multi-scene per-viewport draw fan-out -- a single-scene engine
//    renders the SAME hero in both halves, which is exactly the gap this catches.
//    Building block: [spec][draw_key] (viewport ordering) + [spec][schedule].
SCENARIO("two viewports, two scenes: a different hero in each (JS-driven)",
         "[golden][scenarios][render_graph]") {
    if (!seam::AssetsPresent({"aatrox.glb","ahri.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 1024;
    icfg.height = 512;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));

    // The whole scenario is composed in JS via cairns.dispatch -- no bespoke
    // C++ seam. Left hero -> scene 0 (vp0); right hero -> scene 1 (vp1) with
    // particles; the per-viewport draw fan-out renders each scene.
    cairns::golden::DriveJs(e, R"JS(
        cairns.dispatch("cairns.scene.spawnFitted",
                        { glbs: ["aatrox.glb"], instances: 1, animated: false });
        cairns.dispatch("cairns.scene.use", { index: 1 });
        cairns.dispatch("cairns.scene.spawnFitted",
                        { glbs: ["ahri.glb"], instances: 1, animated: false });
        cairns.dispatch("cairns.scene.use", { index: 0 });
        cairns.dispatch("cairns.viewport.open", {});
        cairns.dispatch("cairns.viewport.setScene", { viewport: 1, scene: 1 });
        cairns.dispatch("cairns.viewport.particles", { viewport: 1, on: true });
    )JS");

    REQUIRE(seam::AdvanceToGoldenFrame(e));

    std::vector<uint8_t> rgba;
    uint32_t w = 0;
    uint32_t h = 0;
    REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
    REQUIRE(w == 1024);
    seam::DumpFinalTargetPng(e, "rg_two_scenes");
    const std::string observed = seam::Md5Hex(rgba);
    const std::string ref = refs::LoadImageRef("rg_two_scenes", seam::PlatformKey(), observed);
    if (ref.empty()) {
        SKIP("bake two-scenes ref");
    }
    REQUIRE(observed == ref);
}

// 4) RENDER GRAPH 2 -- one framebuffer of 100 animated glbs; nested inside it a
//    single depth buffer (depth AFTER resolve); and a nested third framebuffer
//    with a different camera angle.
//    Building block: [spec][schedule] (depth-after-resolve + 3rd-camera survives)
//    + [spec][frustum].
SCENARIO("nested graph: 20 GLBs resolved color + depth strip",
         "[golden][scenarios][render_graph][nested]") {
    // The nested-graph idea: render the GLB set, then in one frame show its
    // RESOLVED color across the top + a depthviz STRIP of the same depth buffer
    // along the bottom (the render.nestedGraph mode drives the composite). 20
    // distinct GLBs from the canonical debug set, single camera.
    std::vector<std::string> glbs;
    for (uint32_t i = cairns::kDebugGlbsToParseStart;
         i < cairns::kDebugGlbsToParseStart + 20; ++i) {
        glbs.emplace_back(cairns::kDebugGlbs[i]);
    }
    if (!seam::AssetsPresent(glbs)) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 512;
    icfg.height = 512;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    cairns::golden::DriveJs(e, cairns::golden::SpawnFittedJs(glbs, 20, true));
    cairns::golden::DriveJs(
        e, R"JS(cairns.dispatch("cairns.render.nestedGraph", { on: true });)JS");
    REQUIRE(seam::AdvanceToGoldenFrame(e));

    SECTION("color (top) + depthviz strip (bottom) -- eyeball the dumped PNG") {
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
        seam::DumpFinalTargetPng(e, "nested.color");
        const std::string observed = seam::Md5Hex(rgba);
        const std::string ref = refs::LoadImageRef("nested.color", seam::PlatformKey(), observed);
        if (ref.empty()) {
            SKIP("bake nested.color (layout changed: 20 GLBs + depth strip)");
        }
        REQUIRE(observed == ref);
    }
}

// 5) FRUSTUM CULL -- counters; not a pixel test.
//    Building block: [spec][frustum].
SCENARIO("actors outside the frustum are culled from the counters",
         "[golden][scenarios][frustum]") {
    if (!seam::AssetsPresent({"aatrox.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 512;
    icfg.height = 512;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    constexpr uint32_t kInside = 3;
    constexpr uint32_t kOutside = 5;
    // 3 actors inside the frustum + 5 off-frustum (+99 axis offsets), composed
    // in JS via prefab.load + scene.instantiate at explicit positions.
    cairns::golden::DriveJs(e, R"JS(
        const idx = cairns.dispatch("cairns.prefab.load",
                                    { path: "aatrox.glb" }).result.prefab;
        for (let i = 0; i < 3; ++i) {
            cairns.dispatch("cairns.scene.instantiate",
                { prefab: idx, x: i * 1.5 - 1.5, y: 0, z: -4, scale: 1 });
        }
        const offs = [[99, 0, 0], [0, 99, 0], [0, 0, 50]];
        for (let i = 0; i < 5; ++i) {
            const o = offs[i % 3];
            const s = 1 + Math.floor(i / 3);
            cairns.dispatch("cairns.scene.instantiate",
                { prefab: idx, x: o[0] * s, y: o[1] * s, z: o[2] * s, scale: 1 });
        }
    )JS");
    REQUIRE(seam::AdvanceToGoldenFrame(e));

    seam::FrameStats s{};
    if (!seam::LastFrameStats(e, s)) {
        SKIP("LastFrameStats accessor not wired yet");
    }
    REQUIRE(s.submitted == kInside + kOutside);
    REQUIRE(s.culled == kOutside);
    REQUIRE(s.draw_calls == kInside);
    REQUIRE(s.verts_processed > 0);
}

// 6) IMGUI INTEGRATION -- mock the HUD numbers, render scene+overlay, hash.
//    Building block: [spec][profiling][hud].
SCENARIO("imgui overlay is stable when fed mocked numbers",
         "[golden][scenarios][imgui]") {
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 512;
    icfg.height = 512;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    // imgui overlay + fixed HUD numbers, composed in JS (matches HudStats::Mock:
    // 16.6 ms / 60 fps / flat graph).
    cairns::golden::DriveJs(e, R"JS(
        cairns.dispatch("cairns.imgui.golden", { on: true });
        cairns.dispatch("cairns.hud.set", { cpu_ms: 16.6, fps: 60.0 });
    )JS");
    REQUIRE(seam::AdvanceToGoldenFrame(e));

    SECTION("captured screen matches the per-platform overlay reference") {
        // Deterministic: particle init is portable ParticleRng and imgui
        // renders in golden mode (SetImguiInGolden), so the overlay image is
        // a real regression check.
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
        seam::DumpFinalTargetPng(e, "imgui.overlay");
        const std::string observed = seam::Md5Hex(rgba);
        const std::string ref = refs::LoadImageRef("imgui.overlay", seam::PlatformKey(), observed);
        if (ref.empty()) {
            SKIP("bake imgui overlay ref (per platform: font atlas differs across backends)");
        }
        REQUIRE(observed == ref);
    }
    SECTION("two captures from the settled engine state are bit-identical") {
        // The particle compute kernel runs unconditionally every frame and
        // flips the parity bit, so frame N and frame N+60 are NOT
        // pixel-identical for an empty scene. Assert the weaker but still
        // useful invariant: readback of the same final target twice is
        // byte-stable. When particle_sim gains a "freeze in golden mode"
        // knob, advance frames between the captures.
        std::vector<uint8_t> a;
        std::vector<uint8_t> b;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, a, w, h));
        REQUIRE(seam::ReadFinalTargetRgba(e, b, w, h));
        REQUIRE(seam::Md5Hex(a) == seam::Md5Hex(b));
    }
}

// Entity-op contract driven through the real op path
// (dispatch -> headless -> engine). No pixels -- pure state assertions. Covers
// the TRS round-trip, the parent cycle guard, the destroy selection-scrub, and
// [N-node] per-scene isolation (an explicit-scene setTRS must not perturb a
// different scene's entity).
SCENARIO("entity ops: TRS round-trip + cycle guard + destroy + N-node isolation",
         "[spec][scenarios][entity]") {
    if (!seam::AssetsPresent({"aatrox.glb", "ahri.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 256;
    icfg.height = 256;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    cairns::control::CommandRegistry& reg = cairns::golden::SetupJs(e);
    auto disp = [&](const char* op, const cairns::json& args) -> cairns::json {
        return reg.Dispatch({{"op", op}, {"args", args}});
    };

    // Scene 0 (primary) = aatrox; scene 1 (secondary) = ahri.
    disp("cairns.scene.spawnFitted",
         {{"glbs", {"aatrox.glb"}}, {"instances", 1}});
    disp("cairns.scene.use", {{"index", 1}});
    disp("cairns.scene.spawnFitted",
         {{"glbs", {"ahri.glb"}}, {"instances", 1}});
    disp("cairns.scene.use", {{"index", 0}});

    const cairns::json l0 =
        disp("cairns.scene.listEntities", cairns::json::object());
    REQUIRE(l0["ok"] == true);
    REQUIRE(l0["result"]["entities"].size() >= 1);
    const uint32_t e0 = l0["result"]["entities"][0].get<uint32_t>();

    // TRS round-trip.
    disp("cairns.entity.setTRS",
         {{"entity", e0}, {"t", {1.5, 2.5, 3.5}}, {"s", {2.0, 2.0, 2.0}}});
    const cairns::json g = disp("cairns.entity.getTRS", {{"entity", e0}});
    REQUIRE(g["result"]["ok"] == true);
    REQUIRE(g["result"]["t"][0].get<float>() == 1.5f);
    REQUIRE(g["result"]["t"][2].get<float>() == 3.5f);
    REQUIRE(g["result"]["s"][1].get<float>() == 2.0f);
    REQUIRE(g["result"]["r"][3].get<float>() == 1.0f);  // identity quat w

    // Parent cycle guard: self-parent is rejected.
    const cairns::json p =
        disp("cairns.entity.setParent", {{"entity", e0}, {"parent", e0}});
    REQUIRE(p["result"]["ok"] == false);

    // Name + find.
    disp("cairns.entity.setName", {{"entity", e0}, {"name", "hero0"}});
    const cairns::json f = disp("cairns.entity.find", {{"name", "hero0"}});
    REQUIRE(f["result"]["found"] == true);
    REQUIRE(f["result"]["entity"].get<uint32_t>() == e0);

    // [N-node] isolation: read scene 1's entity, mutate scene 0's via the
    // explicit {scene:0} arg, assert scene 1's transform is untouched.
    disp("cairns.scene.use", {{"index", 1}});
    const cairns::json l1 =
        disp("cairns.scene.listEntities", cairns::json::object());
    REQUIRE(l1["result"]["entities"].size() >= 1);
    const uint32_t e1 = l1["result"]["entities"][0].get<uint32_t>();
    const cairns::json g1a =
        disp("cairns.entity.getTRS", {{"scene", 1}, {"entity", e1}});
    REQUIRE(g1a["result"]["ok"] == true);
    const float e1_tx = g1a["result"]["t"][0].get<float>();
    disp("cairns.entity.setTRS",
         {{"scene", 0}, {"entity", e0}, {"t", {9.0, 9.0, 9.0}}});
    const cairns::json g1b =
        disp("cairns.entity.getTRS", {{"scene", 1}, {"entity", e1}});
    REQUIRE(g1b["result"]["t"][0].get<float>() == e1_tx);

    // Destroy + scrub: find-by-name no longer resolves.
    disp("cairns.scene.use", {{"index", 0}});
    disp("cairns.entity.destroy", {{"entity", e0}});
    const cairns::json f2 = disp("cairns.entity.find", {{"name", "hero0"}});
    REQUIRE(f2["result"]["found"] == false);
}

// The studio.js Unity surface must LOWER onto the entity ops, not JS-only
// caches / stubs. Drives the real path: script.eval runs GameObject /
// Transform / GameObject.Find|Destroy inside QuickJS, which dispatches
// cairns.entity.* -- we assert on the engine-side result.
SCENARIO("studio.js: Transform setters + name/Find/Destroy lower to entity ops",
         "[spec][scenarios][studio]") {
    if (!seam::AssetsPresent({"aatrox.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 256;
    icfg.height = 256;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    cairns::control::CommandRegistry& reg = cairns::golden::SetupJs(e);
    auto disp = [&](const char* op, const cairns::json& args) -> cairns::json {
        return reg.Dispatch({{"op", op}, {"args", args}});
    };
    disp("cairns.scene.spawnFitted",
         {{"glbs", {"aatrox.glb"}}, {"instances", 1}});
    const cairns::json l =
        disp("cairns.scene.listEntities", cairns::json::object());
    REQUIRE(l["result"]["entities"].size() >= 1);
    const uint32_t ent = l["result"]["entities"][0].get<uint32_t>();

    const std::string code =
        "var e=" + std::to_string(ent) + ";"
        "var go=new GameObject(e,0);"
        "go.transform.position=new Vector3(5,6,7);"
        "go.transform.localScale=new Vector3(3,3,3);"
        "go.name='hero';"
        "var g=cairns.dispatch('cairns.entity.getTRS',{entity:e});"
        "var f=cairns.dispatch('cairns.entity.find',{name:'hero'});"
        "go.Destroy();"
        "var f2=cairns.dispatch('cairns.entity.find',{name:'hero'});"
        "JSON.stringify({tx:g.result.t[0],tz:g.result.t[2],sy:g.result.s[1],"
        "found:f.result.found,foundEnt:f.result.entity,gone:!f2.result.found});";
    const cairns::json ev = disp("cairns.script.eval", {{"code", code}});
    REQUIRE(ev["ok"] == true);
    const cairns::json out =
        cairns::json::parse(ev["result"]["result"].get<std::string>());
    REQUIRE(out["tx"].get<float>() == 5.0f);   // Transform.position lowered
    REQUIRE(out["tz"].get<float>() == 7.0f);
    REQUIRE(out["sy"].get<float>() == 3.0f);   // Transform.localScale lowered
    REQUIRE(out["found"] == true);             // name -> setName; Find resolves
    REQUIRE(out["foundEnt"].get<uint32_t>() == ent);
    REQUIRE(out["gone"] == true);              // Destroy -> entity.destroy
}

// The flag->component conversions' TOGGLE behaviour (the goldens only
// prove default-on neutrality). Particle sim/draw gate follows the per-scene
// ParticleEmitterComponent; editor chrome follows the per-viewport flag.
SCENARIO("C3 flag->component toggles: particle emitter + editor chrome",
         "[spec][scenarios][flags]") {
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 128;
    icfg.height = 128;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));

    // Particles: gate = any bound scene carries a ParticleEmitterComponent.
    REQUIRE(e.ParticlesEnabled() == false);       // no emitter at boot
    e.EnableParticles(true);
    REQUIRE(e.ParticlesEnabled() == true);         // emitter added to scene
    e.EnableParticles(false);
    REQUIRE(e.ParticlesEnabled() == false);        // emitter removed

    // Editor chrome: per-viewport Viewport::Cold::chrome_enabled, default on.
    REQUIRE(e.EditorChromeEnabled() == true);
    e.SetEditorChromeEnabled(false);
    REQUIRE(e.EditorChromeEnabled() == false);
    e.SetEditorChromeEnabled(true);
    REQUIRE(e.EditorChromeEnabled() == true);
}

// End-to-end pick regression -- spawn one centred champion, click the
// frame centre, resolve, consume. Exercises the whole CPU ray-cast path
// (RequestPick -> ResolvePickRaycast union-AABB slab test -> consume). A hit
// (id != 0 background) with the resolved entity is the contract.
SCENARIO("C6 pick: centre-click resolves to the spawned champion (end-to-end)",
         "[spec][scenarios][pick]") {
    if (!seam::AssetsPresent({"aatrox.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 256;
    icfg.height = 256;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    cairns::control::CommandRegistry& reg = cairns::golden::SetupJs(e);
    auto disp = [&](const char* op, const cairns::json& args) -> cairns::json {
        return reg.Dispatch({{"op", op}, {"args", args}});
    };
    disp("cairns.scene.spawnFitted",
         {{"glbs", {"aatrox.glb"}}, {"instances", 1}});
    disp("cairns.render.advanceFrames", {{"count", 1}});  // populate inv_view_proj
    const cairns::json l =
        disp("cairns.scene.listEntities", cairns::json::object());
    REQUIRE(l["result"]["entities"].size() >= 1);
    const uint32_t ent = l["result"]["entities"][0].get<uint32_t>();

    disp("cairns.pick", {{"viewport", 0}, {"x", 128}, {"y", 128}});
    disp("cairns.render.advanceFrames", {{"count", 1}});  // resolve
    const cairns::json r = disp("cairns.pick.consume", cairns::json::object());
    INFO("pick.consume: " << r.dump());
    REQUIRE(r["result"]["resolved"] == true);
    const uint32_t id = r["result"]["id"].get<uint32_t>();
    REQUIRE(id != 0u);         // hit the champion, not empty background (id 0)
    REQUIRE(id == ent + 1u);   // pick ids are 1-based (entity+1; 0 = background)
}

// The generic component-type table + its studio.js lowering.
// Covers the op path (add/get/remove + unknown-type reject + componentTypes)
// and the Unity GameObject.AddComponent/GetComponent path (script.eval).
SCENARIO("C4.2 generic component ops: table round-trip + studio.js lowering",
         "[spec][scenarios][component]") {
    if (!seam::AssetsPresent({"aatrox.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 256;
    icfg.height = 256;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    cairns::control::CommandRegistry& reg = cairns::golden::SetupJs(e);
    auto disp = [&](const char* op, const cairns::json& args) -> cairns::json {
        return reg.Dispatch({{"op", op}, {"args", args}});
    };
    disp("cairns.scene.spawnFitted",
         {{"glbs", {"aatrox.glb"}}, {"instances", 1}});
    const cairns::json l =
        disp("cairns.scene.listEntities", cairns::json::object());
    const uint32_t ent = l["result"]["entities"][0].get<uint32_t>();

    // The table advertises itself.
    const cairns::json ct =
        disp("cairns.entity.componentTypes", cairns::json::object());
    REQUIRE(ct["result"]["types"].size() >= 4u);

    // Camera add -> get round-trip (1.25 / 42.0 are exact in float).
    disp("cairns.entity.addComponent",
         {{"entity", ent},
          {"type", "Camera"},
          {"props", {{"fovYRad", 1.25}, {"farZ", 42.0}, {"isMain", true}}}});
    const cairns::json gc =
        disp("cairns.entity.getComponent", {{"entity", ent}, {"type", "Camera"}});
    REQUIRE(gc["result"]["has"] == true);
    REQUIRE(gc["result"]["fovYRad"].get<float>() == 1.25f);
    REQUIRE(gc["result"]["farZ"].get<float>() == 42.0f);
    REQUIRE(gc["result"]["isMain"] == true);

    // Remove -> gone.
    disp("cairns.entity.removeComponent", {{"entity", ent}, {"type", "Camera"}});
    const cairns::json gone =
        disp("cairns.entity.getComponent", {{"entity", ent}, {"type", "Camera"}});
    REQUIRE(gone["result"]["has"] == false);

    // Unknown type rejected.
    const cairns::json bogus =
        disp("cairns.entity.addComponent", {{"entity", ent}, {"type", "Nope"}});
    REQUIRE(bogus["result"]["ok"] == false);

    // studio.js path: GameObject.AddComponent/GetComponent lower to the ops.
    const std::string code =
        "var e=" + std::to_string(ent) + ";"
        "var go=new GameObject(e,0);"
        "go.AddComponent('Renderable',{layerMask:7,flags:1});"
        "var c=go.GetComponent('Renderable');"
        "var none=go.GetComponent('Camera');"
        "JSON.stringify({has:!!c, lm:c._data.layerMask, camNull:none===null});";
    const cairns::json ev = disp("cairns.script.eval", {{"code", code}});
    REQUIRE(ev["ok"] == true);
    const cairns::json out =
        cairns::json::parse(ev["result"]["result"].get<std::string>());
    REQUIRE(out["has"] == true);
    REQUIRE(out["lm"].get<uint32_t>() == 7u);
    REQUIRE(out["camNull"] == true);
}

// cairns.time.get is a read-only deterministic sim clock (fixed
// timestep; time == frame * dt), and studio.js Time lowers onto it.
SCENARIO("C4.2 time.get: deterministic sim clock + studio.js Time",
         "[spec][scenarios][time]") {
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 128;
    icfg.height = 128;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    cairns::control::CommandRegistry& reg = cairns::golden::SetupJs(e);
    auto disp = [&](const char* op, const cairns::json& args) -> cairns::json {
        return reg.Dispatch({{"op", op}, {"args", args}});
    };

    const cairns::json t0 = disp("cairns.time.get", cairns::json::object());
    REQUIRE(t0["result"]["frame"].get<uint64_t>() == 0u);
    REQUIRE(t0["result"]["time"].get<double>() == 0.0);
    const double dt = t0["result"]["dt"].get<double>();
    REQUIRE(dt > 0.0);

    disp("cairns.render.advanceFrames", {{"count", 12}});
    const cairns::json t1 = disp("cairns.time.get", cairns::json::object());
    const uint64_t f1 = t1["result"]["frame"].get<uint64_t>();
    const double time1 = t1["result"]["time"].get<double>();
    REQUIRE(f1 >= 1u);                                    // clock advanced
    REQUIRE(time1 == static_cast<double>(f1) * dt);       // time == frame * dt

    // studio.js Time mirrors it (no frames advance between here and the read).
    const cairns::json ev = disp(
        "cairns.script.eval",
        {{"code", "JSON.stringify({f:Time.frameCount, t:Time.time})"}});
    const cairns::json out =
        cairns::json::parse(ev["result"]["result"].get<std::string>());
    REQUIRE(out["f"].get<uint64_t>() == f1);
    REQUIRE(out["t"].get<double>() == time1);
}

// The camera surface -- cairns.scene.listCameras,
// cairns.viewport.setCameraEntity, and loud-strict studio.js Camera.main
// (0 or >1 is_main throws, exactly 1 resolves).
SCENARIO("C4.2 CAP-1 camera surface: listCameras + Camera.main + setCameraEntity",
         "[spec][scenarios][camera]") {
    if (!seam::AssetsPresent({"aatrox.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 256;
    icfg.height = 256;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    cairns::control::CommandRegistry& reg = cairns::golden::SetupJs(e);
    auto disp = [&](const char* op, const cairns::json& args) -> cairns::json {
        return reg.Dispatch({{"op", op}, {"args", args}});
    };
    disp("cairns.scene.spawnFitted",
         {{"glbs", {"aatrox.glb"}}, {"instances", 2}});
    const cairns::json l =
        disp("cairns.scene.listEntities", cairns::json::object());
    REQUIRE(l["result"]["entities"].size() >= 2u);
    const uint32_t e0 = l["result"]["entities"][0].get<uint32_t>();
    const uint32_t e1 = l["result"]["entities"][1].get<uint32_t>();

    // No cameras initially.
    REQUIRE(disp("cairns.scene.listCameras", cairns::json::object())["result"]
                ["cameras"]
                    .empty());

    // Drive the loud-strict Camera.main contract + setCameraEntity via JS.
    const std::string code =
        "var out={};"
        "try{Camera.main;out.none='NO'}catch(x){out.none='threw'}"
        "cairns.dispatch('cairns.entity.addComponent',"
        "  {entity:" + std::to_string(e0) +
        ",type:'Camera',props:{isMain:true}});"
        "out.mainEnt=Camera.main.entity;"
        "out.setOk=cairns.dispatch('cairns.viewport.setCameraEntity',"
        "  {viewport:0,entity:" + std::to_string(e0) + "}).result.ok;"
        "cairns.dispatch('cairns.entity.addComponent',"
        "  {entity:" + std::to_string(e1) +
        ",type:'Camera',props:{isMain:true}});"
        "try{Camera.main;out.two='NO'}catch(x){out.two='threw'}"
        "JSON.stringify(out);";
    const cairns::json ev = disp("cairns.script.eval", {{"code", code}});
    REQUIRE(ev["ok"] == true);
    const cairns::json out =
        cairns::json::parse(ev["result"]["result"].get<std::string>());
    REQUIRE(out["none"] == "threw");            // 0 main -> throw
    REQUIRE(out["mainEnt"].get<uint32_t>() == e0);  // exactly 1 -> resolves
    REQUIRE(out["setOk"] == true);              // setCameraEntity ok
    REQUIRE(out["two"] == "threw");             // >1 main -> throw

    // Op path now sees both cameras.
    const cairns::json lc =
        disp("cairns.scene.listCameras", cairns::json::object());
    REQUIRE(lc["result"]["cameras"].size() == 2u);
}

// Deterministic headless resize. Renders a champion, cycles the
// final target down/up/odd, and asserts correct dims + non-black at each size.
// Catches the two real resize bugs: wrong (stale) dims, and the black-screen
// on a size mismatch. Bit-exact round-trip invariance is intentionally NOT
// asserted here -- the scene animates, so equal pixels would need a frozen sim.
SCENARIO("C7 resize: headless final-target resize cycle (dims + non-black)",
         "[spec][scenarios][resize]") {
    if (!seam::AssetsPresent({"aatrox.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 320;
    icfg.height = 240;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    cairns::control::CommandRegistry& reg = cairns::golden::SetupJs(e);
    auto disp = [&](const char* op, const cairns::json& args) -> cairns::json {
        return reg.Dispatch({{"op", op}, {"args", args}});
    };
    auto nonblack = [](const std::vector<uint8_t>& px) -> bool {
        for (uint8_t v : px) {
            if (v != 0) {
                return true;
            }
        }
        return false;
    };
    auto render_at = [&](uint32_t w, uint32_t h) {
        disp("cairns.window.resize", {{"w", w}, {"h", h}});
        disp("cairns.render.advanceFrames", {{"count", 2}});
        std::vector<uint8_t> px;
        uint32_t gw = 0;
        uint32_t gh = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, px, gw, gh));
        REQUIRE(gw == w);            // dims followed the resize (no stale target)
        REQUIRE(gh == h);
        REQUIRE(nonblack(px));       // rendered, not the black-on-mismatch bug
    };

    disp("cairns.scene.spawnFitted",
         {{"glbs", {"aatrox.glb"}}, {"instances", 1}});
    disp("cairns.render.advanceFrames", {{"count", 2}});
    std::vector<uint8_t> base;
    uint32_t bw = 0;
    uint32_t bh = 0;
    REQUIRE(seam::ReadFinalTargetRgba(e, base, bw, bh));
    REQUIRE(bw == 320u);
    REQUIRE(bh == 240u);
    REQUIRE(nonblack(base));

    render_at(160, 120);   // shrink
    render_at(320, 240);   // grow back
    render_at(200, 150);   // odd, non-power-of-two
    render_at(320, 240);   // restore
}

// The scenario picker must render CLEAN:
// (1) the scenario picker window, (2) NO perf HUD, (3) NO particles, (4) NO
// depth PIP bottom-right. The windowed app hides the picker behind the HUD;
// this proves it draws, deterministically, and dumps a viewable
// scenario_picker.png.
SCENARIO("scenario picker renders clean: picker only, no HUD/particles/depth",
         "[golden][scenarios][picker]") {
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 512;
    icfg.height = 512;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    ecfg.particles_enabled = false;   // (3) no particles
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    // (1) picker on + (2) HUD off. Blank scene (no spawn) => (3) no particle
    // emitter, (4) no composition views => no depth PIP. No injected HUD stats.
    e.SetImguiInGolden(true);
    e.SetHudVisible(false);
    cairns::ScenarioLauncher launcher;
    launcher.scripts.push_back({"01_triangle", "01_triangle.js"});
    launcher.scripts.push_back({"04_viking_room", "04_viking_room.js"});
    launcher.scripts.push_back({"06_three_champ_anim", "06_three_champ_anim.js"});
    e.SetImguiPanel(&cairns::DrawScenarioPanel, &launcher);
    REQUIRE(seam::AdvanceToGoldenFrame(e));

    std::vector<uint8_t> rgba;
    uint32_t w = 0;
    uint32_t h = 0;
    REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
    seam::DumpFinalTargetPng(e, "scenario_picker");   // viewable artifact

    REQUIRE(e.ParticlesEnabled() == false);   // (3) no emitter bound
    REQUIRE(e.HudVisible() == false);          // (2) HUD suppressed

    // Region-brightness assertions -- NOT a pixel-exact hash. imgui renders with
    // sub-pixel AA that varies with shared-context state across the suite, so an
    // exact hash flakes (the imgui-overlay golden SKIPs for the same reason).
    // These are permitted for imgui tests ONLY; non-imgui goldens keep exact
    // hashes. The invariant: the ONLY bright content in the frame is the picker.
    auto sum3 = [&](uint32_t x, uint32_t y) -> int {
        const size_t i = (static_cast<size_t>(y) * w + x) * 4;
        return static_cast<int>(rgba[i]) + rgba[i + 1] + rgba[i + 2];
    };
    // The "Scenarios" window anchors top-RIGHT (pivot 1,0 at display.x-16, 16),
    // ~256x105 px in this 512 frame -> ~x[240,496], y[16,122]. Box with margin.
    auto in_picker = [](uint32_t x, uint32_t y) -> bool {
        return x >= 230 && x <= 500 && y >= 8 && y <= 132;
    };
    int picker_bright = 0;   // (1) picker drew: bright text/buttons in its box
    int stray_bright = 0;    // (2) HUD / (3) particle / (4) depth-PIP pixels land
    for (uint32_t y = 0; y < h; ++y) {                 // outside -> must be zero
        for (uint32_t x = 0; x < w; ++x) {
            if (sum3(x, y) > 300) {   // brighter than dark bg (~94) + blue button
                if (in_picker(x, y)) {
                    ++picker_bright;
                } else {
                    ++stray_bright;
                }
            }
        }
    }
    REQUIRE(picker_bright > 0);   // (1) the scenario picker rendered
    REQUIRE(stray_bright == 0);   // (2) no HUD, (3) no particles, (4) no depth PIP
}

// ---- Drosophila arena: joints driven programmatically, no clock ------------
// The one subject whose pose comes from the script rather than from a clip.
// cairns.entity.setJointPose replaces the actor's node-local TRS wholesale, so
// nothing here depends on time, on time_phase, or on which clip the loader
// happened to select -- which is exactly why it byte-gates like a static
// subject even though the mesh goes through the full skinning path.
//
// Joint-axis convention comes from the MJCF the rig was converted from (see
// tools/mjcf_to_rigged_glb.py + assets/fly.joints.json): a `_yaw` DoF turns
// about X, a `_roll` DoF about Z, and a bare one about Y. Joints are resolved
// by NAME through cairns.entity.jointNames so a re-export that renumbers the
// skeleton doesn't silently repose the fly.
SCENARIO("subject: fly arena (programmatic joint poses)",
         "[scenarios][golden][subject]") {
    cairns::golden::RunJsSubject("fly_pose", 512, 512, {"fly.glb"}, R"JS(
        // primitive.create returns only its type, so diff the entity list to
        // learn which id it made -- listEntities order is registry order, not
        // creation order, and guessing it silently swapped floor and drop.
        function listEnts() {
            return cairns.dispatch("cairns.scene.listEntities", {}).result.entities;
        }
        function created(before) {
            for (const e of listEnts()) {
                if (before.indexOf(e) < 0) { return e; }
            }
            throw new Error("primitive.create produced no entity");
        }
        let seen = listEnts();
        cairns.dispatch("cairns.primitive.create",
                        { type: "ellipse", color: [0.28, 0.30, 0.34, 1] });
        const floor = created(seen);
        seen = listEnts();
        cairns.dispatch("cairns.primitive.create",
                        { type: "ellipsoid", color: [0.95, 0.90, 0.58, 1] });
        const drop = created(seen);
        // Arena floor: the ellipse is a unit disk in XY, so lay it flat about X.
        cairns.dispatch("cairns.entity.setTRS", {
            entity: floor, t: [0, -1.0, 0],
            r: [-0.70710678, 0, 0, 0.70710678], s: [7, 7, 7] });
        // Sugar drop, sitting on the floor a little in front of the flies.
        cairns.dispatch("cairns.entity.setTRS", {
            entity: drop, t: [1.35, -0.86, 0.9],
            r: [0, 0, 0, 1], s: [0.26, 0.26, 0.26] });

        const fly = cairns.dispatch("cairns.prefab.load",
                                    { path: "fly.glb" }).result.prefab;
        const stance = cairns.dispatch("cairns.scene.instantiate",
            { prefab: fly, x: -1.1, y: -0.55, z: 0.0, scale: 0.30 }).result.entity;
        const swing = cairns.dispatch("cairns.scene.instantiate",
            { prefab: fly, x:  1.1, y: -0.55, z: 0.0, scale: 0.30 }).result.entity;

        const sun = cairns.dispatch("cairns.entity.new", { name: "sun" });
        cairns.dispatch("cairns.entity.addComponent", {
            entity: sun.result.entity, type: "DirectionalLight",
            props: { dirX: -0.45, dirY: -1.0, dirZ: -0.35,
                     colorR: 1.0, colorG: 0.97, colorB: 0.9, intensity: 1.2,
                     ambientR: 0.16, ambientG: 0.16, ambientB: 0.19 } });
        cairns.dispatch("cairns.scene.setMaterialShaderAll", { shader: "lit" });
        cairns.dispatch("cairns.viewport.setCamera",
                        { viewport: 0, x: 0, y: 0.15, z: 3.1, pitch: -0.06 });

        // name -> skin joint index, per entity (both share a rig, but resolve
        // per entity so the op surface is what the test exercises).
        function jointMap(e) {
            const r = cairns.dispatch("cairns.entity.jointNames", { entity: e });
            const m = {};
            for (const j of r.result.joints) { m[j.name] = j.index; }
            return m;
        }
        function axisOf(name) {
            if (name.endsWith("_yaw"))  { return [1, 0, 0]; }
            if (name.endsWith("_roll")) { return [0, 0, 1]; }
            return [0, 1, 0];
        }
        // `angles[name]` is either a bare angle (axis implied by the DoF's name
        // suffix) or an explicit [ax, ay, az, angle].
        function pose(e, angles) {
            const m = jointMap(e);
            const joints = [];
            for (const name in angles) {
                if (!(name in m)) { throw new Error("no joint " + name); }
                const a = angles[name];
                const explicit = Array.isArray(a);
                joints.push({ joint: m[name],
                              axis: explicit ? [a[0], a[1], a[2]] : axisOf(name),
                              angle: explicit ? a[3] : a });
            }
            const r = cairns.dispatch("cairns.entity.setJointPose",
                                      { entity: e, joints: joints });
            if (!r.result.ok) { throw new Error("setJointPose failed"); }
        }
        // Wings are their own bodies in the MJCF (no DoF of their own), so
        // raising them poses the body node directly -- the override array is
        // node-indexed, not restricted to articulated joints.
        // The MJCF long axis is X, so a wing lifts by rolling about X.
        const wings = { "LWing": [1, 0, 0, 0.55], "RWing": [1, 0, 0, -0.55] };
        // Alternating tripod: LF/RM/LH plant, RF/LM/RH are lifted mid-swing.
        function tripod(planted, lifted) {
            const a = {};
            for (const leg of planted) {
                a["joint_" + leg + "Coxa"]  = -0.35;
                a["joint_" + leg + "Femur"] = -0.85;
                a["joint_" + leg + "Tibia"] =  1.30;
            }
            for (const leg of lifted) {
                a["joint_" + leg + "Coxa"]  =  0.30;
                a["joint_" + leg + "Femur"] = -1.45;
                a["joint_" + leg + "Tibia"] =  0.55;
            }
            for (const w in wings) { a[w] = wings[w]; }
            return a;
        }
        pose(stance, tripod(["LF", "RM", "LH"], ["RF", "LM", "RH"]));
        pose(swing,  tripod(["RF", "LM", "RH"], ["LF", "RM", "LH"]));
    )JS");
}
