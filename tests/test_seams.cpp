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
// GreaterInit; the test harness needs an equivalent. ResetImguiContext()
// destroys any existing context and creates a fresh one -- BootHeadless
// calls it so each SCENARIO starts from a clean ImGui state (fixes the
// G1/G6 flake under [scenarios] full run, modularization-notes #9b).
namespace {
void EnsureImguiContextImpl() {
    if (ImGui::GetCurrentContext() == nullptr) {
        ImGui::CreateContext();
        // Never touch imgui.ini in golden runs. With it enabled, the HUD
        // window's SetNextWindowPos(..., FirstUseEver) defers to whatever
        // position a prior run saved on disk, so G6 imgui.overlay hashed
        // differently run-to-run. nullptr => FirstUseEver lands at (20,20)
        // and the imgui render is run-to-run deterministic (#9b). Shared
        // (not per-scenario) context: see ResetImguiContextImpl below.
        ImGui::GetIO().IniFilename = nullptr;
    }
}
// ResetImguiContextImpl (destroy+recreate per SCENARIO) DOES make G6 pass both
// solo and full-suite -- it no longer aborts (engine teardown since releases
// its imgui handles before the next scenario). The shared context's font atlas
// (built by the FIRST scenario's engine, not G6's) is the only cross-scenario
// imgui state that changes G6's hash -- only G6 ever opens the "cairns" window.
// Not wired yet: it needs G6 re-baked to the clean a9e932d1 hash, AND the full
// [scenarios] gate still flakes ~1/6 on the three_champ_static GPU bug
// (FLAKY_TESTS.md #2) regardless, so robust G6 alone won't make the gate green.
// Fix #2 first, then wire this + re-bake as the finishing step. (Measured: the
// per-scenario atlas re-upload does NOT change the three_champ flake rate.)
void ResetImguiContextImpl() {
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui::DestroyContext();
    }
    ImGui::CreateContext();
}
}  // namespace

void EnsureImguiContext() { EnsureImguiContextImpl(); }

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
#    elif CAIRNS_WEBGPU
    return "macos-webgpu";
#    else
    return "macos-unknown";
#    endif
#  endif
#else
    return "unknown";
#endif
}

bool BootHeadless(cairns::Engine& engine, uint32_t width, uint32_t height) {
    EnsureImguiContextImpl();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = width;
    icfg.height = height;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    return engine.GreaterInit(icfg, ecfg);
}

bool AdvanceToGoldenFrame(cairns::Engine& engine) {
    // Back-compat: kGoldenDumpFrame + 1 ticks. C.18 prefers AdvanceFrames(N)
    // for the multi-sample capture pattern (frame 9 + frame 55).
    return AdvanceFrames(engine, cairns::kGoldenDumpFrame + 1);
}

bool AdvanceFrames(cairns::Engine& engine, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        if (!engine.RenderHeadlessFrame()) {
            return false;
        }
    }
    return true;
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

bool ReadFinalTargetRgba(cairns::Engine& engine,
                         std::vector<uint8_t>& rgba, uint32_t& w,
                         uint32_t& h) {
    return engine.ReadFinalTargetRgba(rgba, w, h);
}

void DumpFinalTargetPng(cairns::Engine& engine, const std::string& name) {
    const char* dump_dir = std::getenv("CAIRNS_DUMP_PNGS");
    if (!dump_dir || !dump_dir[0]) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);
    const std::string path =
        std::string(dump_dir) + "/" + name + "." + PlatformKey() + ".png";
    (void)engine.DumpFinalTarget(path);
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
