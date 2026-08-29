// tests/test_seams.cpp
//
// Engine + headless wrappers exposed to the Tier G goldens. Each seam either
// (a) lands a real wrapper, or (b) returns false so the scenario SECTION
// SKIPs -- the doc anticipates a progressive wiring as Phase 3 capabilities
// land. The decision per seam is annotated below.

#include "test_seams.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine.hpp"
#include "engine_headless.hpp"
#include "rhi/init_config.hpp"
#include "util/hud_stats.hpp"
#include "util/misc.hpp"  // cairns::GetStaticResourceFilepath

#include "imgui.h"

namespace cairns::test_seams {

// Engine::GreaterInit reads ImGui::GetIO() during initImguiPipeline. The CLI
// shells (sdl-min / cairns_serve) call ImGui::CreateContext() before
// GreaterInit; the test harness needs an equivalent. Created once on first
// touch, destroyed at process exit.
namespace {
struct ImguiContextGuard {
    ImguiContextGuard() {
        if (ImGui::GetCurrentContext() == nullptr) {
            ImGui::CreateContext();
        }
    }
    ~ImguiContextGuard() {
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
        }
    }
};
ImguiContextGuard& EnsureImguiContextImpl() {
    static ImguiContextGuard g;
    return g;
}
}  // namespace

void EnsureImguiContext() { (void)EnsureImguiContextImpl(); }

const char* PlatformKey() {
    // CAIRNS_PLATFORM_KEY env var always wins -- lets the harness pin a
    // specific key (e.g. "android-vk-emu" on AVD vs "android-vk" on a real
    // device) without relying on quirky property reads.
    if (const char* override_ = std::getenv("CAIRNS_PLATFORM_KEY")) {
        if (override_[0]) {
            static std::string s = override_;
            return s.c_str();
        }
    }
#if defined(__ANDROID__)
    return "android-vk";
#elif defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_SIMULATOR
    return "ios-sim-metal";
#  elif TARGET_IPHONE_SIMULATOR
    return "ios-sim-metal";
#  elif TARGET_OS_IPHONE
#    if CAIRNS_METAL
    return "ios-metal";
#    else
    return "ios-vk";
#    endif
#  else
#    if CAIRNS_METAL
    return "macos-metal";
#    elif CAIRNS_VULKAN
    return "macos-vk";
#    else
    return "macos-unknown";
#    endif
#  endif
#else
    return "unknown";
#endif
}

bool BootHeadless(cairns::Engine& engine, uint32_t width, uint32_t height) {
    (void)EnsureImguiContextImpl();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = width;
    icfg.height = height;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    return engine.GreaterInit(icfg, ecfg);
}

bool AdvanceToGoldenFrame(cairns::Engine& engine) {
    // Tick RenderHeadlessFrame until we cross cairns::kGoldenDumpFrame.
    // The engine increments sim_frame_ inside draw(); after kGoldenDumpFrame
    // draws the final target is settled and ready for readback.
    for (uint64_t i = 0; i < cairns::kGoldenDumpFrame + 1; ++i) {
        if (!engine.RenderHeadlessFrame()) {
            return false;
        }
    }
    return true;
}

bool BuildLadderScene(cairns::Engine& engine,
                      const std::vector<std::string>& glbs,
                      uint32_t instances, bool animated) {
    (void)EnsureImguiContextImpl();
    (void)animated;
    if (glbs.empty()) {
        // L1 triangle path: tiny_quad was set on EngineConfig at
        // GreaterInit. Nothing else to do.
        return true;
    }
    // L2..L7: load the glbs once, then spawn `instances` total entities.
    const cairns::headless::LoadBatchExport loaded =
        cairns::headless::RuntimeLoadGlbs(&engine, 0,
                                           static_cast<uint32_t>(glbs.size()));
    if (loaded.count == 0) {
        return false;
    }
    const uint32_t prefab_count = engine.NumPrefabs();
    if (prefab_count == 0) {
        return false;
    }
    for (uint32_t i = 0; i < instances; ++i) {
        const uint32_t scene_idx = i % prefab_count;
        const float x = static_cast<float>(i % 10) * 2.0f - 9.0f;
        const float z = static_cast<float>(i / 10) * 2.0f;
        const glm::mat4 world =
            glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, z));
        const uint32_t out =
            engine.InstantiatePrefab(scene_idx, world, /*time_phase=*/0.0f);
        if (out == UINT32_MAX) {
            return false;
        }
    }
    return true;
}

bool SpawnGlbs(cairns::Engine& engine,
               const std::vector<std::string>& glbs, bool animated) {
    return BuildLadderScene(engine, glbs,
                            static_cast<uint32_t>(glbs.size()), animated);
}

bool ClearSpawned(cairns::Engine& engine) {
    // A.4: Engine already exposes ClearActiveScene() (engine.hpp:1086).
    // Drains every entity in the active scene's entt::registry; asset slots
    // (prefab pool, materials, meshes) stay valid -- that's the #228 recycle
    // path G2 verifies. The return is the count of entities cleared (>= 0);
    // for the seam we just need success.
    (void)engine.ClearActiveScene();
    return true;
}

bool OpenSecondViewport(cairns::Engine& engine, const char* glb,
                        float yaw_rad, bool with_particles) {
    // A.5: routes to Engine::OpenSecondViewport (engine.hpp). Opens viewport
    // 1, places it at the right half (uniform tile), spawns one glb, sets
    // the per-viewport particle gate. Returns false on viewport-cap or
    // asset-resolve failure.
    return engine.OpenSecondViewport(glb ? glb : "", yaw_rad, with_particles);
}

bool ConfigureNestedGraph(cairns::Engine& engine) {
    // A.6: opens vp1 + vp2 at ±60° yaw so the existing render graph
    // composes 3 forward passes (G4's "third camera" + nested). The
    // resolved-depth SECTION still SKIPs until ReadBackBuffer salvage
    // (A.10) is applied.
    return engine.ConfigureNestedGraph();
}

bool SpawnInsideOutsideSplit(cairns::Engine& engine, const char* glb,
                             uint32_t inside, uint32_t outside) {
    // A.8: routes to Engine::SpawnInsideOutsideSplit. inside actors at
    // origin (visible), outside actors at +99 axis offsets (off-frustum).
    // G5 SECTION will still SKIP until the cull stage is real (A.7 note).
    return engine.SpawnInsideOutsideSplit(glb ? glb : "", inside, outside);
}

bool LastFrameStats(cairns::Engine& engine, FrameStats& out) {
    // A.7: routes to Engine::LastFrameStats which populates the existing
    // submitted / draw_calls / verts_processed counters. `culled` is 0
    // today -- the engine has no per-proxy frustum cull stage yet (see
    // dev/plans/2026-06-18_gfx_modularization-notes.md). G5 should SKIP
    // when cull isn't real so the test fails meaningfully later, not
    // green-by-stub now.
    cairns::Engine::FrameStats fs{};
    if (!engine.LastFrameStats(fs)) {
        return false;
    }
    if (!fs.cull_stage_implemented) {
        return false;  // G5 SKIPs: culled count is meaningless without cull
    }
    out.draw_calls = fs.draw_calls;
    out.verts_processed = fs.verts_processed;
    out.culled = fs.culled;
    out.submitted = fs.submitted;
    return true;
}

bool ReadParticleBuffer(cairns::Engine& engine, std::vector<uint8_t>& out) {
    // A.12: routes to Engine::ReadParticleBuffer. After A.1 the particle
    // init is portable (mt19937 + hand-rolled NextUnit), so a buffer diff
    // is now *meaningful*; the readback itself is gated on A.10
    // (Resources::ReadBackBuffer) which is deferred. Returns false today.
    return engine.ReadParticleBuffer(out);
}

bool EnableImguiOverlay(cairns::Engine& engine, bool on) {
    // A.9: routes to Engine::SetImguiInGolden, which lifts the "no imgui in
    // golden" guard. Combined with SetInjectedHudStats (HudStats::Mock()),
    // the rendered HUD is byte-stable so G6 image SECTION can compare an
    // actual overlay capture to its baked ref.
    engine.SetImguiInGolden(on);
    return true;
}

bool InjectHudStats(cairns::Engine& engine, const cairns::HudStats& s) {
    engine.SetInjectedHudStats(s);
    return true;
}

bool ReadFinalTargetRgba(cairns::Engine& engine,
                         std::vector<uint8_t>& rgba, uint32_t& w,
                         uint32_t& h) {
    return engine.ReadFinalTargetRgba(rgba, w, h);
}

bool ReadSkinOutputUsedBytes(cairns::Engine& /*engine*/,
                             std::vector<uint8_t>& /*out*/) {
    return false;  // L6/L7 buffer SECTION; gated until ReadBackBuffer salvage
                   // from stash@{0} lands. Not load-bearing for L1..L5 image
                   // goldens.
}

bool ReadResolvedDepth(cairns::Engine& /*engine*/,
                       std::vector<uint8_t>& /*out*/) {
    return false;  // G4 depth SECTION; same salvage gate as skin buffer.
}

bool AssetsPresent(const std::vector<std::string>& glbs) {
    if (glbs.empty()) {
        return true;
    }
    namespace fs = std::filesystem;
    // Defer to the engine's own resolver -- it knows where assets live on
    // every platform (SDL_GetBasePath on Apple = .app bundle root /
    // Resources, APK AssetManager on Android, $<CONFIGURATION>/ on
    // desktop). Falling back to cwd-based search would keep iOS sim /
    // Android golden tests SKIPping forever.
    for (const std::string& name : glbs) {
        fs::path resolved;
        if (cairns::GetStaticResourceFilepath(name, resolved)) {
            continue;
        }
        // Last-resort fallback for the desktop dev-build layout.
        const std::vector<fs::path> roots = {
            fs::current_path(),
            fs::current_path() / "assets",
            fs::current_path() / "..",
            fs::current_path() / "../assets",
            fs::path("tests/assets"),
        };
        bool found = false;
        for (const auto& r : roots) {
            if (fs::exists(r / name)) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

}  // namespace cairns::test_seams
