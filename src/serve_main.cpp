// cairns_serve -- headless NDJSON control plane host.
//
// P0c: CommandRegistry + stdio transport + lifecycle ops (app.ping,
// app.version, app.quit, tools.list). Engine is NOT instantiated yet --
// surfaceless device init lands in P1, then this main() also calls
// Engine::Init({.surfaceless=true,...}) before the transport loop and
// engine.deinit() after.
//
// stdout = protocol only (JSON responses). stderr = logs (CAIRNS_PRINT,
// [Timer], [FLAKE], etc.). See plan risk #1.

#include <iostream>

#include "control/command_registry.hpp"
#include "control/handlers/lifecycle_ops.hpp"
#include "control/transport_stdio.hpp"

int main() {
    auto& registry = cairns::control::CommandRegistry::Instance();
    bool quit = false;
    cairns::control::RegisterLifecycleOps(registry, &quit);
    cairns::control::StdioTransport::Run(registry, std::cin, std::cout, &quit);
    return 0;
}
