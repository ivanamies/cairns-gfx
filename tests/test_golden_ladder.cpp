// tests/test_golden_ladder.cpp
//
// TIER G -- golden / cross-platform divergence. Links cairns_core; backend is
// compile-selected via -DCAIRNS_GFX_BACKEND={metal,vulkan}.
// TAGS: [golden][ladder]
//
// THE LADDER. Each rung adds exactly one source of failure.
//   rung 1  triangle              pipeline + clear + one draw. No assets.
//   rung 2  one dye glb           single static mesh load + draw.
//   rung 3  two dye glbs          instancing / multiple draws.
//   rung 4  viking room           textured static mesh, UVs.
//   rung 5  3 static LoL          production-shape assets, still static.
//   rung 6  3 animated LoL        FIRST rung to light the skinning path.
//   rung 7  100 animated LoL      realistic max workload (100, NOT 500).
//
// CHECKS:
//   * Golden IMAGE (all rungs) -- readback the final target; hash RGBA; compare
//     to a PER-PLATFORM reference under tests/refs/{rung}.{platform}.imghash.
//   * Cross-platform BUFFER (rungs 6-7 only) -- readback skin_output_pool;
//     hash; compare to a SINGLE SHARED reference (skinhash, no platform suffix).
//     SKIPs until the ReadBackBuffer salvage from stash@{0} lands.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "engine.hpp"
#include "engine_headless.hpp"
#include "test_seams.hpp"
#include "test_refs.hpp"

namespace {

constexpr const char* kPlatformKey =
#if defined(__ANDROID__)
    "android-vk";
#elif defined(__APPLE__) && CAIRNS_GFX_BACKEND_METAL
#  if TARGET_OS_SIMULATOR
    "ios-sim-metal";
#  elif TARGET_OS_IOS
    "ios-metal";
#  else
    "macos-metal";
#  endif
#elif defined(__APPLE__) && CAIRNS_GFX_BACKEND_VULKAN
    "macos-vk";
#else
    "unknown";
#endif

struct Rung {
    const char* name;
    std::vector<std::string> glbs;
    uint32_t instances;
    bool animated;
    bool produces_skin_buffer;
    cairns::EngineConfig::CamPose cam;
};

// The amalgam's canonical asset names (dye/viking_room/lol_a..c) aren't in
// the cairns asset tree; substitute production glbs from kDebugGlbs that DO
// ship with the project so L2..L7 actually exercise the pipeline. Their
// `false` skinning intent is informational only -- InstantiatePrefab attaches
// a skin opportunistically (via TryCreateSkinForScene) when the glb has one,
// so the LoL champs are always animated in practice.
const std::vector<Rung> kLadder = {
    {"triangle",          {},                                              1,   false, false, {0,0,3, 0,0}},
    {"one_aatrox",        {"aatrox.glb"},                                  1,   false, false, {0,1,4, 0,0}},
    {"two_aatrox",        {"aatrox.glb"},                                  2,   false, false, {0,1,5, 0,0}},
    {"viking_room",       {"viking_room.glb"},                             1,   false, false, {2,2,2, 3.9f,-0.5f}},
    {"three_champ_static",{"ahri.glb","akali.glb","alistar.glb"},          3,   false, false, {0,1.5f,6, 0,0}},
    {"three_champ_anim",  {"ahri.glb","akali.glb","alistar.glb"},          3,   true,  true,  {0,1.5f,6, 0,0}},
    {"hundred_champ_anim",{"ahri.glb","akali.glb","alistar.glb"},          100, true,  true,  {0,8,22, 0,0}},
};

constexpr uint32_t kGoldenW = 512;
constexpr uint32_t kGoldenH = 512;

}  // namespace

TEST_CASE("golden ladder", "[golden][ladder]") {
    // 7 rungs (0..6). Catch2 v3.5.4 doesn't ship range<size_t>; an explicit
    // value list is the portable shape and reads as well.
    const std::size_t idx =
        GENERATE(std::size_t{0}, std::size_t{1}, std::size_t{2},
                 std::size_t{3}, std::size_t{4}, std::size_t{5},
                 std::size_t{6});
    const Rung& rung = kLadder[idx];

    INFO("rung: " << rung.name << "  platform: " << kPlatformKey);

    if (!cairns::test_seams::AssetsPresent(rung.glbs)) {
        SKIP("assets for rung '" << rung.name << "' not present in this build");
    }

    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = kGoldenW;
    icfg.height = kGoldenH;

    cairns::EngineConfig ecfg{};
    ecfg.tiny_quad = rung.glbs.empty();
    ecfg.glb_overrides = rung.glbs;
    ecfg.cam_pose = rung.cam;
    // dump_path EMPTY on purpose: a non-empty dump_path triggers the CLI
    // FixedClock+dump+exit(0) path. Tests must use the new use_fixed_clock
    // bool instead so the engine doesn't kill the test process.
    ecfg.use_fixed_clock = true;

    cairns::test_seams::EnsureImguiContext();
    cairns::Engine engine;
    REQUIRE(engine.GreaterInit(icfg, ecfg));
    REQUIRE(cairns::test_seams::BuildLadderScene(engine, rung.glbs,
                                                  rung.instances, rung.animated));
    REQUIRE(cairns::test_seams::AdvanceToGoldenFrame(engine));

    SECTION("golden image matches the per-platform reference") {
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        REQUIRE(cairns::test_seams::ReadFinalTargetRgba(engine, rgba, w, h));
        REQUIRE(w == kGoldenW);
        REQUIRE(h == kGoldenH);
        const std::string observed = cairns::test_seams::Md5Hex(rgba);
        const std::string ref = cairns::test_refs::LoadImageRef(rung.name, kPlatformKey, observed);
        if (ref.empty()) {
            SKIP("no image reference yet for {" << rung.name << ", "
                                                << kPlatformKey << "} -- bake one");
        }
        REQUIRE(observed == ref);
    }

    SECTION("skin output matches the shared cross-platform reference") {
        if (!rung.produces_skin_buffer) {
            SKIP("rung '" << rung.name << "' produces no skin buffer");
        }
        std::vector<uint8_t> skin;
        if (!cairns::test_seams::ReadSkinOutputUsedBytes(engine, skin)) {
            SKIP("ReadBackBuffer not wired yet -- apply stash@{0} salvage");
        }
        const std::string observed = cairns::test_seams::Md5Hex(skin);
        const std::string ref = cairns::test_refs::LoadSkinRef(rung.name, observed);
        if (ref.empty()) {
            SKIP("no shared skin reference yet for '" << rung.name << "'");
        }
        REQUIRE(observed == ref);
    }
}
