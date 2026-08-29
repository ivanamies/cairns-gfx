// control/handlers/script_ops.hpp
//
// P5 stub: registers script.eval against an embedded QuickJS context.
// Every previously-registered CommandRegistry op is bound as a JS function
// on a `cairns` global so a snippet can call e.g. `cairns.app_ping()`
// instead of issuing one NDJSON line per op (matters for scenes that need
// thousands of instantiate calls -- one script.eval roundtrip vs 3200 lines).

#pragma once

namespace cairns::control {

class CommandRegistry;

// Call AFTER all other ops are registered: the binder snapshots the current
// registry contents and exposes each name as `cairns.<name_safe>` in JS.
void RegisterScriptOps(CommandRegistry& registry);

}  // namespace cairns::control
