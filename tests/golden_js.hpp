// tests/golden_js.hpp
//
// JS-driven golden scenarios: the SCENE COMPOSITION (which glbs, how many
// viewports, which scene each binds to) is a JS snippet run through the real
// command registry + QuickJS host -- the same surface the shipping editor uses,
// NOT a bespoke C++ seam. The capture protocol (advance to a settled frame,
// hash the final target, compare the per-platform ref) stays in C++.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "engine.hpp"
#include "rhi/init_config.hpp"
#include "util/misc.hpp"  // GetStaticResourceFilepath -- shared scenario scripts

#include "control/command_registry.hpp"
#include "control/handlers/lifecycle_ops.hpp"
#include "control/handlers/perf_ops.hpp"
#include "control/handlers/render_ops.hpp"
#include "control/handlers/scene_ops.hpp"
#include "control/handlers/entity_ops.hpp"
#include "control/handlers/script_ops.hpp"
#include "control/handlers/selection_ops.hpp"
#include "util/json.hpp"

#include "test_seams.hpp"
#include "test_refs.hpp"

#if defined(__APPLE__) && CAIRNS_METAL
#include <Metal/Metal.hpp>
#include <Foundation/Foundation.hpp>
#include <cstdlib>
#include <filesystem>
#endif

namespace cairns::golden {

#if defined(__APPLE__) && CAIRNS_METAL
// Headless GPU frame capture (throwaway, for tmp/). Env-gated: when
// CAIRNS_CAPTURE_SUBJECT matches a subject name, RunJsSubject wraps that
// subject's frame-9 GPU work in an MTL .gputrace under CAIRNS_CAPTURE_DIR.
// Targets the default device -- the same one the engine's
// CreateSystemDefaultDevice() returns -- so the engine's command buffers land
// in the trace. Needs MTL_CAPTURE_ENABLED=1 in the environment. Used to inspect
// the bistable three_champ_static GPU flake (FLAKY_TESTS.md #2).
namespace capture_detail {
inline MTL::CaptureManager* g_active = nullptr;
inline bool BeginGpuCapture(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);  // Metal refuses to overwrite
    MTL::Device* dev = MTL::CreateSystemDefaultDevice();
    if (dev == nullptr) {
        return false;
    }
    MTL::CaptureManager* mgr = MTL::CaptureManager::sharedCaptureManager();
    if (!mgr->supportsDestination(MTL::CaptureDestinationGPUTraceDocument)) {
        return false;  // MTL_CAPTURE_ENABLED=1 not set
    }
    MTL::CaptureDescriptor* desc = MTL::CaptureDescriptor::alloc()->init();
    desc->setCaptureObject(dev);
    desc->setDestination(MTL::CaptureDestinationGPUTraceDocument);
    NS::String* p = NS::String::string(path.c_str(), NS::UTF8StringEncoding);
    desc->setOutputURL(NS::URL::fileURLWithPath(p));
    NS::Error* err = nullptr;
    const bool ok = mgr->startCapture(desc, &err);
    desc->release();
    g_active = ok ? mgr : nullptr;
    return ok;
}
inline void EndGpuCapture() {
    if (g_active != nullptr) {
        g_active->stopCapture();
        g_active = nullptr;
    }
}
}  // namespace capture_detail
#endif

// Clear the shared registry, register every op group against THIS scenario's
// engine, install a fresh JS context, then eval the composition `js`.
inline cairns::control::CommandRegistry& SetupJs(cairns::Engine& engine) {
    // Test-harness scaffolding: a stable-address registry reused across
    // scenarios (Clear()+re-register rebinds the captured Engine&). The
    // QuickJS runtime (process-static in script_ops) holds a pointer to this,
    // so the address must be stable -- a function-local static, not a
    // per-scenario local.
    static cairns::control::ScriptHost script_host;
    static cairns::control::CommandRegistry reg;
    reg.Clear();
    static bool quit = false;
    cairns::control::RegisterLifecycleOps(reg, quit);
    cairns::control::RegisterRenderOps(reg, engine);
    cairns::control::RegisterSceneOps(reg, engine);
    cairns::control::RegisterEntityOps(reg, engine);
    cairns::control::RegisterPerfOps(reg, engine);
    cairns::control::RegisterSelectionOps(reg, engine);
    cairns::control::RegisterScriptOps(reg, script_host);  // binds cairns.dispatch
    reg.Dispatch({{"op", "cairns.script.reload"}});  // fresh JS globals
    return reg;
}

inline void EvalJs(cairns::control::CommandRegistry& reg,
                   const std::string& js) {
    const json resp = reg.Dispatch(
        {{"op", "cairns.script.eval"}, {"args", {{"code", js}}}});
    INFO("scenario JS response: " << resp.dump());
    REQUIRE(resp.value("ok", false) == true);
}

inline void DriveJs(cairns::Engine& engine, const std::string& js) {
    EvalJs(SetupJs(engine), js);
}

// Compose a cairns.scene.spawnFitted dispatch from a glb-name list.
inline std::string SpawnFittedJs(const std::vector<std::string>& glbs,
                                 uint32_t instances, bool animated) {
    std::string js = "cairns.dispatch(\"cairns.scene.spawnFitted\", { glbs: [";
    for (const std::string& g : glbs) {
        js += "\"";
        js += g;
        js += "\", ";
    }
    js += "], instances: ";
    js += std::to_string(instances);
    js += ", animated: ";
    js += (animated ? "true" : "false");
    js += " });";
    return js;
}

// Boot a w x h headless engine, run the JS composition `setup_js`, then capture
// the final target at frame 9 + frame 55 and compare per-platform refs. `assets`
// gates a SKIP when the glbs aren't in this build.
inline void RunJsSubject(const char* name, uint32_t w, uint32_t h,
                         const std::vector<std::string>& assets,
                         const std::string& setup_js) {
    namespace seam = cairns::test_seams;
    namespace refs = cairns::test_refs;
    if (!seam::AssetsPresent(assets)) {
        SKIP("assets for '" << name << "' not present in this build");
    }
    seam::EnsureImguiContext();
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = w;
    icfg.height = h;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    cairns::Engine e;
    REQUIRE(e.GreaterInit(icfg, ecfg));
    DriveJs(e, setup_js);
    auto capture = [&](const char* tag, uint32_t advance) {
        REQUIRE(seam::AdvanceFrames(e, advance));
        std::vector<uint8_t> rgba;
        uint32_t gw = 0;
        uint32_t gh = 0;
        REQUIRE(seam::ReadFinalTargetRgba(e, rgba, gw, gh));
        const std::string label = std::string(name) + ".f" + tag;
        seam::DumpFinalTargetPng(e, label);
        const std::string obs = seam::Md5Hex(rgba);
        const std::string ref =
            refs::LoadImageRef(label, seam::PlatformKey(), obs);
        if (ref.empty()) {
            SKIP("no ref for " << label << " -- bake one");
        }
        REQUIRE(obs == ref);
    };
    {
        uint32_t f9_advance = 9;
#if defined(__APPLE__) && CAIRNS_METAL
        const char* cap_subj = std::getenv("CAIRNS_CAPTURE_SUBJECT");
        const bool do_cap =
            (cap_subj != nullptr) && (std::string(cap_subj) == std::string(name));
        // RAII so a REQUIRE(obs==ref) throw on the bad render still flushes the
        // trace to disk before the exception unwinds out of RunJsSubject.
        struct CapGuard {
            bool active;
            ~CapGuard() {
                if (active) {
                    capture_detail::EndGpuCapture();
                }
            }
        };
        if (do_cap) {
            // Advance to frame 8 OUTSIDE the capture so the .gputrace holds only
            // frame 9's GPU work (one frame, not nine -- far smaller on disk).
            REQUIRE(seam::AdvanceFrames(e, 8));
            f9_advance = 1;
            const char* cap_dir = std::getenv("CAIRNS_CAPTURE_DIR");
            const std::string dir =
                (cap_dir != nullptr && cap_dir[0] != '\0') ? cap_dir : ".";
            capture_detail::BeginGpuCapture(dir + "/" + std::string(name) +
                                            ".f09.gputrace");
        }
        CapGuard cap_guard{do_cap};
#endif
        capture("09", f9_advance);
    }
    capture("55", 46);
}

}  // namespace cairns::golden
