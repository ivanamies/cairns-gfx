// TEMP smoke: prove cairns_control links into the golden tests + JS eval works.
#include <catch2/catch_test_macros.hpp>

#include "control/command_registry.hpp"
#include "control/handlers/lifecycle_ops.hpp"
#include "control/handlers/script_ops.hpp"

SCENARIO("control links + JS dispatch + eval", "[jsmoke]") {
    cairns::control::ScriptHost script_host;
    cairns::control::CommandRegistry reg;
    bool quit = false;
    cairns::control::RegisterLifecycleOps(reg, quit);
    cairns::control::RegisterScriptOps(reg, script_host);

    const auto ev = reg.Dispatch(
        {{"op", "cairns.script.eval"}, {"args", {{"code", "1 + 2"}}}});
    REQUIRE(ev["ok"] == true);
    REQUIRE(ev["result"]["result"] == 3);

    // engine-op-via-JS path: dispatch a real registered op from inside JS.
    const auto disp = reg.Dispatch(
        {{"op", "cairns.script.eval"},
         {"args", {{"code", "cairns.dispatch('cairns.app.ping', {}).ok"}}}});
    REQUIRE(disp["ok"] == true);
    REQUIRE(disp["result"]["result"] == true);
}
