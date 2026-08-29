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
#include "util/hud_stats.hpp"

namespace seam = cairns::test_seams;
namespace refs = cairns::test_refs;

// 1) PARTICLES -- VK-tutorial style compute particles.
//    Building block: [spec][particles][determinism] (deterministic emitter).
SCENARIO("particles render deterministically across platforms",
         "[golden][scenarios][particles]") {
    cairns::Engine e;
    REQUIRE(seam::BootHeadless(e, 512, 512));
    REQUIRE(seam::AdvanceToGoldenFrame(e));

    SECTION("golden image (per-platform)") {
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
        const std::string observed = seam::Md5Hex(rgba);
        const std::string ref = refs::LoadImageRef("particles", seam::PlatformKey(), observed);
        if (ref.empty()) {
            SKIP("bake particles image ref");
        }
        REQUIRE(observed == ref);
    }
    SECTION("cross-platform particle buffer (shared ref)") {
        std::vector<uint8_t> buf;
        if (!seam::ReadParticleBuffer(e, buf)) {
            SKIP("particle buffer diff requires the deterministic emitter "
                 "(replace std::rand) -- see [spec][particles][determinism]");
        }
        const std::string observed = seam::Md5Hex(buf);
        const std::string ref = refs::LoadSkinRef("particles.buf", observed);
        if (ref.empty()) {
            SKIP("bake shared particle buffer ref");
        }
        REQUIRE(observed == ref);
    }
}

// 2) HOT LOAD / RELOAD -- spawn 4, check; spawn 5 different, check; clear, check.
//    Building block: [spec][core][handle] (#228 reuse-not-reset) + asset dedup.
SCENARIO("hot reload: spawn, replace, and clear stay correct",
         "[golden][scenarios][hot_reload]") {
    const std::vector<std::string> first  = {"lol_a.glb","lol_b.glb","lol_c.glb","lol_d.glb"};
    const std::vector<std::string> second = {"lol_e.glb","lol_f.glb","lol_g.glb","lol_h.glb","lol_i.glb"};
    if (!seam::AssetsPresent(first) || !seam::AssetsPresent(second)) {
        SKIP("hot-reload assets absent");
    }

    cairns::Engine e;
    REQUIRE(seam::BootHeadless(e, 512, 512));

    auto checkpoint = [&](const char* tag) {
        REQUIRE(seam::AdvanceToGoldenFrame(e));
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
        const std::string observed = seam::Md5Hex(rgba);
        const std::string ref = refs::LoadImageRef(tag, seam::PlatformKey(), observed);
        if (ref.empty()) {
            SKIP("bake ref " << tag);
        }
        REQUIRE(observed == ref);
    };

    REQUIRE(seam::SpawnGlbs(e, first, false));
    checkpoint("hot_reload.four");
    REQUIRE(seam::ClearSpawned(e));
    REQUIRE(seam::SpawnGlbs(e, second, false));
    checkpoint("hot_reload.five");
    REQUIRE(seam::ClearSpawned(e));
    checkpoint("hot_reload.empty");
}

// 3) RENDER GRAPH 1 -- two framebuffers side by side. LEFT glb has NO particles;
//    RIGHT glb HAS particles.
//    Building block: [spec][draw_key] (viewport ordering) + [spec][schedule]
//                    + [spec][particles].
SCENARIO("two viewports: left plain, right with particles",
         "[golden][scenarios][render_graph]") {
    if (!seam::AssetsPresent({"lol_a.glb","lol_b.glb"})) {
        SKIP("assets absent");
    }
    cairns::Engine e;
    REQUIRE(seam::BootHeadless(e, 1024, 512));
    REQUIRE(seam::SpawnGlbs(e, {"lol_a.glb"}, false));
    REQUIRE(seam::OpenSecondViewport(e, "lol_b.glb", 0.0f, true));
    REQUIRE(seam::AdvanceToGoldenFrame(e));

    std::vector<uint8_t> rgba;
    uint32_t w = 0;
    uint32_t h = 0;
    REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
    REQUIRE(w == 1024);
    const std::string observed = seam::Md5Hex(rgba);
    const std::string ref = refs::LoadImageRef("rg_side_by_side", seam::PlatformKey(), observed);
    if (ref.empty()) {
        SKIP("bake side-by-side ref");
    }
    REQUIRE(observed == ref);
}

// 4) RENDER GRAPH 2 -- one framebuffer of 100 animated glbs; nested inside it a
//    single depth buffer (depth AFTER resolve); and a nested third framebuffer
//    with a different camera angle.
//    Building block: [spec][schedule] (depth-after-resolve + 3rd-camera survives)
//    + [spec][frustum].
SCENARIO("nested graph: color + resolved depth + third camera",
         "[golden][scenarios][render_graph][nested]") {
    if (!seam::AssetsPresent({"lol_a.glb","lol_b.glb","lol_c.glb"})) {
        SKIP("assets absent");
    }
    cairns::Engine e;
    REQUIRE(seam::BootHeadless(e, 512, 512));
    REQUIRE(seam::SpawnGlbs(e, {"lol_a.glb","lol_b.glb","lol_c.glb"}, true));
    REQUIRE(seam::ConfigureNestedGraph(e));
    REQUIRE(seam::AdvanceToGoldenFrame(e));

    SECTION("main color (per-platform image)") {
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
        const std::string observed = seam::Md5Hex(rgba);
        const std::string ref = refs::LoadImageRef("nested.color", seam::PlatformKey(), observed);
        if (ref.empty()) {
            SKIP("bake nested.color");
        }
        REQUIRE(observed == ref);
    }
    SECTION("resolved depth (per-platform)") {
        std::vector<uint8_t> depth;
        if (!seam::ReadResolvedDepth(e, depth)) {
            SKIP("resolved depth readback not wired yet");
        }
        const std::string observed = seam::Md5Hex(depth);
        const std::string ref = refs::LoadImageRef("nested.depth_resolved", seam::PlatformKey(), observed);
        if (ref.empty()) {
            SKIP("bake nested.depth_resolved");
        }
        REQUIRE(observed == ref);
    }
}

// 5) FRUSTUM CULL -- counters; not a pixel test.
//    Building block: [spec][frustum].
SCENARIO("actors outside the frustum are culled from the counters",
         "[golden][scenarios][frustum]") {
    if (!seam::AssetsPresent({"lol_a.glb"})) {
        SKIP("assets absent");
    }
    cairns::Engine e;
    REQUIRE(seam::BootHeadless(e, 512, 512));
    constexpr uint32_t kInside = 3;
    constexpr uint32_t kOutside = 5;
    REQUIRE(seam::SpawnInsideOutsideSplit(e, "lol_a.glb", kInside, kOutside));
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
    cairns::Engine e;
    REQUIRE(seam::BootHeadless(e, 512, 512));
    REQUIRE(seam::SpawnGlbs(e, {}, false));
    REQUIRE(seam::EnableImguiOverlay(e, true));
    REQUIRE(seam::InjectHudStats(e, cairns::HudStats::Mock()));
    REQUIRE(seam::AdvanceToGoldenFrame(e));

    SECTION("captured screen matches the per-platform overlay reference") {
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
        const std::string observed = seam::Md5Hex(rgba);
        const std::string ref = refs::LoadImageRef("imgui.overlay", seam::PlatformKey(), observed);
        if (ref.empty()) {
            SKIP("bake imgui overlay ref (per platform: font atlas differs across backends)");
        }
        REQUIRE(observed == ref);
    }
    SECTION("two captures with the same mocked stats are bit-identical") {
        std::vector<uint8_t> a;
        std::vector<uint8_t> b;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, a, w, h));
        REQUIRE(seam::AdvanceToGoldenFrame(e));
        REQUIRE(seam::ReadFinalTargetRgba(e, b, w, h));
        REQUIRE(seam::Md5Hex(a) == seam::Md5Hex(b));
    }
}
