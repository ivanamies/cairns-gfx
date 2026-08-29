// cairns_serve -- headless NDJSON control plane host.
//
// P0c: CommandRegistry + stdio transport + lifecycle ops.
// P1B: Engine::GreaterInit({.surfaceless=true,...}) attempted before the
// transport loop. If it succeeds, the engine is owned for the session and
// torn down on quit. If it fails, an [Engine] line goes to stderr and we
// continue (lifecycle ops still work; engine-bound ops would return
// "engine_not_initialized" once they exist).
//
// Asset paths: SDL_GetBasePath() returns the binary's parent dir on Apple
// for an unbundled executable. CMake POST_BUILD copies the shaders +
// smoke-test GLB next to cairns_serve so the existing
// GetStaticResourceFilepath path resolves.
//
// stdout = protocol only (JSON responses). stderr = logs.

#include <iostream>

#include "imgui.h"

#include "control/command_registry.hpp"
#include "control/handlers/lifecycle_ops.hpp"
#include "control/handlers/perf_ops.hpp"
#include "control/handlers/render_ops.hpp"
#include "control/handlers/script_ops.hpp"
#include "control/transport_stdio.hpp"
#include "engine.hpp"
#include "rhi/init_config.hpp"

int main() {
    auto& registry = cairns::control::CommandRegistry::Instance();
    bool quit = false;
    cairns::control::RegisterLifecycleOps(registry, &quit);

    // ImGui context is required by Engine::initRenderPipeline (font atlas
    // sizing reads ImGui::GetIO()). No SDL platform backend in headless mode
    // -- only the renderer half is exercised.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    cairns::Engine* engine = new cairns::Engine;
    cairns::rhi::InitConfig cfg{};
    cfg.surfaceless = true;
    cfg.width = 1280;
    cfg.height = 720;
    const bool engine_ok = engine->GreaterInit(cfg);
    if (!engine_ok) {
        std::fprintf(stderr,
                     "[Engine] surfaceless GreaterInit failed -- lifecycle "
                     "ops only.\n");
    } else {
        std::fprintf(stderr, "[Engine] surfaceless GreaterInit ok.\n");
    }
    cairns::control::RegisterRenderOps(registry, engine_ok ? engine : nullptr);
    cairns::control::RegisterPerfOps(registry, engine_ok ? engine : nullptr);
    cairns::control::RegisterScriptOps(registry);

    cairns::control::StdioTransport::Run(registry, std::cin, std::cout, &quit);

    if (engine_ok) {
        engine->deinit();
    }
    delete engine;
    ImGui::DestroyContext();
    return 0;
}
