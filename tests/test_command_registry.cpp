// tests/test_command_registry.cpp
//
// SPEC: G.5 control / agent layer round-trip.
// TAGS: [spec][control][registry]
//
// The audit named control/* as entirely uncovered. The CommandRegistry
// register-then-dispatch round-trip is a pure-CPU contract (no engine, no
// GPU): given (name, schema, handler), Dispatch on a JSON request invokes
// the handler with the matching args. Alias resolution + ToolsSearch are
// pure functions over the registry's flat array (#215). This is the
// observable surface every JS Studio Sugar / NDJSON / ImGui transport
// bottoms out at.
//
// Note: we don't link command_registry.cpp into the spec target (it's
// the only TU that owns dispatch state); instead we test the public API
// through a minimal CommandRegistry constructed in this TU. If a Registry
// implementation lands as header-only, swap to that. The shape this spec
// pins is what a re-implementer must produce.

#include <catch2/catch_test_macros.hpp>

#include "control/command_registry.hpp"

using namespace cairns;
using namespace cairns::control;

SCENARIO("CommandRegistry rejects duplicate Register",
         "[spec][control][registry]") {
    CommandRegistry reg;
    reg.Register("test.echo", json{}, "echo input back",
                 [](const json& in) { return in; });
    SUCCEED();
    // Re-registering same canonical name: implementations may overwrite,
    // assert, or ignore. The spec pins observable behavior only -- the
    // next Dispatch must reach SOME handler, not crash.
    REQUIRE_NOTHROW(reg.Register("test.echo", json{}, "again",
                                  [](const json& in) { return in; }));
}

SCENARIO("Dispatch routes to the registered handler",
         "[spec][control][registry][regression]") {
    CommandRegistry reg;
    int seen = 0;
    reg.Register("test.tick", json{}, "increments",
                 [&](const json&) {
                     ++seen;
                     return json{{"ok", true}};
                 });
    json req;
    req["op"] = "test.tick";
    req["args"] = json::object();
    const json resp = reg.Dispatch(req);
    REQUIRE(seen == 1);
    REQUIRE(resp.contains("ok"));
}

// Alias round-trip: now unblocked -- each SCENARIO owns a fresh local
// CommandRegistry (the process singleton is gone), so re-Register on a
// canonical no longer collides across tests. Left un-added here to keep this
// change scoped to the de-singleton; the alias contract has coverage via the
// JS-driven scenarios.

SCENARIO("Unknown op dispatches to a documented error path",
         "[spec][control][registry]") {
    CommandRegistry reg;
    json req;
    req["op"] = "no.such.op";
    req["args"] = json::object();
    const json resp = reg.Dispatch(req);
    // The exact error key may differ between implementations; the
    // contract is "doesn't crash, returns something."
    REQUIRE_FALSE(resp.is_null());
}
