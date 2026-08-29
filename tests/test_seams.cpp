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

bool ClearSpawned(cairns::Engine& /*engine*/) {
    // The scene-clear path is the AssetRegistry::ClearAll / SceneWorld reset
    // that production hot-reload uses. Until wired into Engine as a single
    // public call, return false so G2 SKIPs gracefully. Not load-bearing for
    // L1..L7 ladder; gates only G2 (hot reload).
    return false;
}

bool OpenSecondViewport(cairns::Engine& /*engine*/, const char* /*glb*/,
                        float /*yaw_rad*/, bool /*with_particles*/) {
    return false;  // G3 only; gated until viewport-1 spawn path exposed.
}

bool ConfigureNestedGraph(cairns::Engine& /*engine*/) {
    return false;  // G4 only; gated until nested graph configurator exposed.
}

bool SpawnInsideOutsideSplit(cairns::Engine& /*engine*/, const char* /*glb*/,
                             uint32_t /*inside*/, uint32_t /*outside*/) {
    return false;  // G5 only; gated until placement seam exposed.
}

bool LastFrameStats(cairns::Engine& /*engine*/, FrameStats& /*out*/) {
    return false;  // G5 only; gated until BSF counter accessor exposed.
}

bool ReadParticleBuffer(cairns::Engine& /*engine*/,
                        std::vector<uint8_t>& /*out*/) {
    return false;  // G1 buffer SECTION; gated until production particle init
                   // switches to ParticleRng/SeedParticles.
}

bool EnableImguiOverlay(cairns::Engine& /*engine*/, bool /*on*/) {
    return false;  // G6 image SECTION; gated until imgui-during-golden seam
                   // exposed (golden_=true skips imgui today).
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
    // GLBs may live in the dev assets/ tree, the binary-adjacent copy (the
    // engine's CMake copies glbs into $<CONFIGURATION>/ via add_resource),
    // or the build dir itself. The engine's static resource lookup checks
    // all of these too, so the test mirrors it.
    std::vector<fs::path> roots = {
        fs::current_path(),
        fs::current_path() / "assets",
        fs::current_path() / "..",
        fs::current_path() / "../assets",
        fs::path("tests/assets"),
    };
    for (const std::string& name : glbs) {
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
