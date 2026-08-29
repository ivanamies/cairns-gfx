// tests/golden_subject.hpp
//
// Shared golden capture for a single rendered subject. Boots a headless engine,
// builds a scene from `glbs`, settles to frame 9 then frame 55, hashes each
// capture, and compares to the per-platform ref (auto-bakes when missing).
// Used by both [scenarios] (light correctness) and [stress] (scale).
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "engine.hpp"
#include "test_seams.hpp"
#include "test_refs.hpp"

namespace cairns::golden {

inline void RunSubject(const char* name, const std::vector<std::string>& glbs,
                       uint32_t instances, bool animated,
                       cairns::EngineConfig::CamPose cam) {
    namespace seam = cairns::test_seams;
    namespace refs = cairns::test_refs;
    if (!seam::AssetsPresent(glbs)) {
        SKIP("assets for '" << name << "' not present in this build");
    }
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 512;
    icfg.height = 512;
    cairns::EngineConfig ecfg{};
    ecfg.tiny_quad = glbs.empty();
    ecfg.glb_overrides = glbs;
    ecfg.cam_pose = cam;
    ecfg.use_fixed_clock = true;
    seam::EnsureImguiContext();
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    REQUIRE(seam::BuildScene(e, glbs, instances, animated));

    // Two settled frames catch animation-continuity bugs a single capture
    // would miss. The second advance is 46 so the engine lands on frame 55.
    auto capture = [&](const char* tag, uint32_t advance) {
        REQUIRE(seam::AdvanceFrames(e, advance));
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, rgba, w, h));
        REQUIRE(w == 512);
        REQUIRE(h == 512);
        const std::string label = std::string(name) + ".f" + tag;
        seam::DumpFinalTargetPng(e, label);
        const std::string observed = seam::Md5Hex(rgba);
        const std::string ref =
            refs::LoadImageRef(label, seam::PlatformKey(), observed);
        if (ref.empty()) {
            SKIP("no ref for " << label << " -- bake one");
        }
        REQUIRE(observed == ref);
    };
    capture("09", 9);
    capture("55", 46);
}

}  // namespace cairns::golden
