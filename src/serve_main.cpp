// cairns_serve -- headless NDJSON control-plane host (R1: headless drive).
//  - Engine boots surfaceless before the transport loop; on failure only
//    lifecycle ops are served and the session continues.
//  - Asset paths: SDL_GetBasePath() is the binary's parent dir for an
//    unbundled Apple executable; CMake copies shaders + smoke GLB next to
//    the binary so GetStaticResourceFilepath resolves.
//  - stdout = protocol only (JSON responses). stderr = logs.
//  - Watchdog: a background thread monitors a heartbeat the transport ticks
//    around each dispatch; a stalled op (e.g. metal dispatch_semaphore_wait
//    wedged on a dead completion handler) aborts for a crash report instead
//    of waiting on an external SIGKILL. CAIRNS_SERVE_WATCHDOG_SEC tunes it
//    (0 disables).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <thread>

#include "imgui.h"

#include "control/boot_run.hpp"
#include "control/command_registry.hpp"
#include "control/handlers/lifecycle_ops.hpp"
#include "control/handlers/perf_ops.hpp"
#include "control/handlers/render_ops.hpp"
#include "control/handlers/scene_ops.hpp"
#include "control/handlers/entity_ops.hpp"
#include "control/handlers/script_ops.hpp"
#include "control/handlers/selection_ops.hpp"
#include "control/transport_stdio.hpp"
#include "engine.hpp"
#include "rhi/init_config.hpp"
#include "shell/env_config.hpp"

int main() {
    // Declared before the registry so it outlives it (the registry's script
    // ops capture &script_host; teardown runs registry dtor first).
    cairns::control::ScriptHost script_host;
    cairns::control::CommandRegistry registry;
    bool quit = false;
    cairns::control::RegisterLifecycleOps(registry, quit);

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
    const cairns::EngineConfig ecfg =
        cairns::shell::LoadEngineConfigFromEnv();
    const bool engine_ok = engine->GreaterInit(cfg, ecfg);
    if (!engine_ok) {
        std::fprintf(stderr,
                     "[Engine] surfaceless GreaterInit failed -- lifecycle "
                     "ops only.\n");
    } else {
        std::fprintf(stderr, "[Engine] surfaceless GreaterInit ok.\n");
    }
    // Engine-bound op groups are only registered when init succeeded; on
    // failure tools.list / tools.search omit them, callers get unknown_op
    // (more informative than registering stubs that throw at call time).
    if (engine_ok) {
        cairns::control::RegisterRenderOps(registry, *engine);
        cairns::control::RegisterSceneOps(registry, *engine);
        cairns::control::RegisterEntityOps(registry, *engine);
        cairns::control::RegisterPerfOps(registry, *engine);
        cairns::control::RegisterSelectionOps(registry, *engine);
    }
    // Script ops must come LAST so tools.list inside script.eval reflects
    // every other op already registered.
    cairns::control::RegisterScriptOps(registry, script_host);
    // Bundled boot script. Aborts if assets/run.js isn't in the bundle.
    if (engine_ok) {
        cairns::control::RunBootScript(registry);
    }

    // Watchdog: the transport stamps steady_clock ns BEFORE Dispatch and
    // AFTER the response is flushed. If the gap exceeds the threshold while
    // an op is in flight, the engine is wedged and we abort -- noisier than
    // a SIGKILL, leaves a coredump for debugging.
    double timeout_sec = 30.0;
    if (const char* s = std::getenv("CAIRNS_SERVE_WATCHDOG_SEC")) {
        timeout_sec = std::atof(s);
    }
    std::atomic<int64_t> heartbeat_ns{0};
    std::atomic<bool> watchdog_stop{false};
    std::thread watchdog;
    if (timeout_sec > 0.0) {
        const int64_t timeout_ns =
            static_cast<int64_t>(timeout_sec * 1.0e9);
        watchdog = std::thread([&heartbeat_ns, &watchdog_stop, timeout_ns,
                                timeout_sec]() {
            while (!watchdog_stop.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(250));
                const int64_t hb =
                    heartbeat_ns.load(std::memory_order_acquire);
                if (hb == 0) {
                    continue;  // no op has run yet
                }
                const int64_t now =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
                if (now - hb > timeout_ns) {
                    std::fprintf(stderr,
                        "[Watchdog] op stalled > %.1fs, aborting "
                        "cairns_serve\n", timeout_sec);
                    std::fflush(stderr);
                    std::abort();
                }
            }
        });
    }

    cairns::control::StdioTransport::Run(registry, stdin, stdout, quit,
                                          &heartbeat_ns);

    watchdog_stop.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
        watchdog.join();
    }

    if (engine_ok) {
        engine->deinit();
    }
    delete engine;
    ImGui::DestroyContext();
    return 0;
}
