// control/handlers/lifecycle_ops.hpp
//
// Register the always-available ops on a CommandRegistry: app.ping,
// app.version, app.quit, tools.list. None of these touch GPU state; they're
// safe to register before Engine::Init runs.
//
// `quit_flag` is the destination written by app.quit; the transport's Run
// loop watches it. Caller owns the bool.

#pragma once

namespace cairns::control {

class CommandRegistry;

void RegisterLifecycleOps(CommandRegistry& registry, bool& quit_flag);

}  // namespace cairns::control
