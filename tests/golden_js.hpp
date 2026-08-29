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
#include <string>
#include <vector>

#include "engine.hpp"
#include "rhi/init_config.hpp"

#include "control/command_registry.hpp"
#include "control/handlers/lifecycle_ops.hpp"
#include "control/handlers/perf_ops.hpp"
#include "control/handlers/render_ops.hpp"
#include "control/handlers/scene_ops.hpp"
#include "control/handlers/script_ops.hpp"
#include "control/handlers/selection_ops.hpp"
#include "util/json.hpp"

#include "test_seams.hpp"
#include "test_refs.hpp"

namespace cairns::golden {

// Clear the shared registry, register every op group against THIS scenario's
// engine, install a fresh JS context, then eval the composition `js`. (The
// registry singleton is a known wart -- TODO "delete all singletons" -- so we
// Clear()+re-register per scenario to rebind the captured Engine&.)
inline cairns::control::CommandRegistry& SetupJs(cairns::Engine& engine) {
    auto& reg = cairns::control::CommandRegistry::Instance();
    reg.Clear();
    static bool quit = false;
    cairns::control::RegisterLifecycleOps(reg, quit);
    cairns::control::RegisterRenderOps(reg, engine);
    cairns::control::RegisterSceneOps(reg, engine);
    cairns::control::RegisterPerfOps(reg, engine);
    cairns::control::RegisterSelectionOps(reg, engine);
    cairns::control::RegisterScriptOps(reg);  // last; binds cairns.dispatch
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
    capture("09", 9);
    capture("55", 46);
}

}  // namespace cairns::golden
