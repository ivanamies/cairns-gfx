// tests/test_seams.cpp
//
// Engine + headless wrappers exposed to the Tier G goldens. Each seam either
// (a) lands a real wrapper, or (b) returns false so the scenario SECTION
// SKIPs until the engine capability it needs is wired.

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
// GreaterInit; BootHeadless calls EnsureImguiContextImpl so tests get the
// same guarantee.
namespace {
void EnsureImguiContextImpl() {
    if (ImGui::GetCurrentContext() == nullptr) {
        ImGui::CreateContext();
        // Never touch imgui.ini in golden runs: with it enabled, the HUD
        // window's SetNextWindowPos(..., FirstUseEver) defers to whatever
        // position a prior run saved on disk, and the imgui-overlay golden
        // hashes differently run-to-run. nullptr => FirstUseEver lands at
        // (20,20), deterministically. Shared (not per-scenario) context:
        // see ResetImguiContextImpl below.
        ImGui::GetIO().IniFilename = nullptr;
    }
}
// Not wired: destroy+recreate per SCENARIO would isolate the shared font
// atlas, the only cross-scenario imgui state that shifts the imgui-overlay
// golden's hash. Wiring it needs that golden re-baked, and the full
// [scenarios] gate still flakes on the separate three_champ_static GPU bug
// (FLAKY_TESTS.md) -- fix that first.
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
    // kGoldenDumpFrame + 1 ticks. The multi-sample capture pattern
    // (frame 9 + frame 55) uses AdvanceFrames(N) directly.
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
    // Routes to Engine::LastFrameStats (submitted / draw_calls /
    // verts_processed / culled). When the engine reports no real cull stage,
    // return false so the frustum-cull golden SKIPs instead of passing
    // green-by-stub on culled=0.
    cairns::Engine::FrameStats fs{};
    if (!engine.LastFrameStats(fs)) {
        return false;
    }
    if (!fs.cull_stage_implemented) {
        return false;  // culled count is meaningless without a real cull stage
    }
    out.draw_calls = fs.draw_calls;
    out.verts_processed = fs.verts_processed;
    out.culled = fs.culled;
    out.submitted = fs.submitted;
    return true;
}

bool ReadParticleBuffer(cairns::Engine& engine, std::vector<uint8_t>& out) {
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
    return false;  // Append-only load acceptance buffer SECTION; SKIPs until
                   // Resources::ReadBackBuffer lands. Not load-bearing for
                   // the image goldens.
}

bool ReadResolvedDepth(cairns::Engine& /*engine*/,
                       std::vector<uint8_t>& /*out*/) {
    return false;  // Nested-graph resolved-depth SECTION; same readback gate.
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
