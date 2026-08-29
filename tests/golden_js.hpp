// tests/golden_js.hpp
//
// JS-driven golden scenarios: the SCENE COMPOSITION (which glbs, how many
// viewports, which scene each binds to) is a JS snippet run through the real
// command registry + QuickJS host -- the same surface the shipping editor uses,
// NOT a bespoke C++ seam. The capture protocol (advance to a settled frame,
// hash the final target, compare the per-platform ref) stays in C++.
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <string>

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

namespace cairns::golden {

// Clear the shared registry, register every op group against THIS scenario's
// engine, install a fresh JS context, then eval the composition `js`. (The
// registry singleton is a known wart -- TODO "delete all singletons" -- so we
// Clear()+re-register per scenario to rebind the captured Engine&.)
inline void DriveJs(cairns::Engine& engine, const std::string& js) {
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
    const json resp = reg.Dispatch(
        {{"op", "cairns.script.eval"}, {"args", {{"code", js}}}});
    INFO("scenario JS response: " << resp.dump());
    REQUIRE(resp.value("ok", false) == true);
}

}  // namespace cairns::golden
