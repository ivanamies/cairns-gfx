// cairns_serve -- headless NDJSON control plane host.
//
// P0c: CommandRegistry + stdio transport + lifecycle ops (app.ping,
// app.version, app.quit, tools.list).
//
// P1B (in progress): Engine::GreaterInit({.surfaceless=true,...}) attempted
// once but blocked at the shader-load + asset-load paths -- both use
// SDL_GetBasePath() which on cairns_serve returns the cairns_serve binary's
// dir rather than an .app bundle's Resources dir. The shaders are still in
// sdl-min.app/Contents/Resources/. Resolutions to land in a follow-up:
//
//   a) Either expose an "assets_root" / "shaders_root" field on InitConfig
//      that overrides SDL_GetBasePath() inside Engine + Pipelines, and
//      have cairns_serve point at sdl-min.app/Contents/Resources/, or
//
//   b) CMake: have cairns_serve run a POST_BUILD copy_helper for shaders
//      into build/<backend>/Debug/ so SDL_GetBasePath() finds them
//      adjacent to the binary (same trick the non-Apple desktop path uses
//      already in CMakeLists.txt).
//
// (a) is what the plan's P2 wants long-term (asset paths as ops); (b) is
// the quickest path to a smoke test. The remaining P1 work
// (final_target_ retarget + io.dumpTexture + render.frame + parity gate)
// lands after.
//
// stdout = protocol only (JSON responses). stderr = logs.

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
