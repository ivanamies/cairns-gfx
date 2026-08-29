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
//
// **W1 watchdog (#228)**: a background thread monitors a heartbeat the
// transport loop ticks before/after each op-dispatch. If the gap
// exceeds kWatchdogTimeoutSec, the engine is wedged (e.g. metal's
// dispatch_semaphore_wait blocking forever on a stalled completion
// handler) and the watchdog calls std::abort to dump a crash report
// instead of waiting for an external SIGKILL. Tunable via the
// CAIRNS_SERVE_WATCHDOG_SEC env var (0 disables).

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
#include "control/handlers/script_ops.hpp"
#include "control/handlers/selection_ops.hpp"
#include "control/transport_stdio.hpp"
#include "engine.hpp"
#include "rhi/init_config.hpp"
#include "shell/env_config.hpp"

int main() {
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
        cairns::control::RegisterPerfOps(registry, *engine);
        cairns::control::RegisterSelectionOps(registry, *engine);
    }
    // Script ops must come LAST so tools.list inside script.eval reflects
    // every other op already registered.
    cairns::control::RegisterScriptOps(registry);
    // Bundled boot script. Aborts if assets/run.js isn't in the bundle.
    if (engine_ok) {
        cairns::control::RunBootScript(registry);
    }

    // W1 watchdog: monitor the transport's heartbeat. The transport
    // stamps steady_clock ns BEFORE Dispatch and AFTER the response is
    // flushed. If the gap exceeds the threshold while an op is in
    // flight, the engine is wedged and we abort -- noisier than a
    // SIGKILL, leaves a coredump for debugging.
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
