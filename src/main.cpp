#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_init.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <cstdlib>
#include <cstring>
#endif

#include <vector>
#include <string>
#include <memory>
#include <set>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <glm/glm.hpp>

#include "util/define.hpp"
#include "util/gltf_loader.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include "shell/scenario_launcher.hpp"

#include "control/boot_run.hpp"
#include "engine.hpp"
#include "rhi/init_config.hpp"
#include "shell/env_config.hpp"
#include "shell/sdl_rhi_glue.hpp"
#include "util/task_guard.hpp"

#include <cstdio>
#include "control/agent_stdin_drain.hpp"
#include "control/command_registry.hpp"
#include "control/handlers/lifecycle_ops.hpp"
#include "control/handlers/perf_ops.hpp"
#include "control/handlers/render_ops.hpp"
#include "control/handlers/scene_ops.hpp"
#include "control/handlers/entity_ops.hpp"
#include "control/handlers/script_ops.hpp"
#include "control/handlers/selection_ops.hpp"
#include "util/json.hpp"
#include "util/misc.hpp"  // GetBasePathSafe

#ifndef __EMSCRIPTEN__
// SDL_mixer is not vendored in third_party/, so MIX_Track only reaches this TU
// transitively on platforms whose SDL headers pull it in. `AppContext::track`
// is a placeholder that is never dereferenced, so an opaque forward
// declaration is enough; a repeated typedef of the same type is legal, making
// this a no-op where SDL_mixer.h is already in the include graph.
typedef struct MIX_Track MIX_Track;
#endif

struct AppContext {
    SDL_Window* window = nullptr;
    // Backend-specific shell handle (metal: SDL_MetalView; vk: nullptr).
    // Released via cairns::shell::DetachWindow on shutdown.
    void* shell_handle = nullptr;

#ifndef __EMSCRIPTEN__
    // Audio (native only; the web build has no audio path).
    SDL_AudioDeviceID audioDevice;
    MIX_Track* track = nullptr;
#endif

    cairns::Engine* engine = nullptr;

    // QuickJS host, declared BEFORE the registry so it outlives it (the
    // registry's script ops capture &script_host).
    cairns::control::ScriptHost script_host;
    // Op registry owned by the app (no process singleton). Register*Ops bind
    // against it; the agent drain + scenario launcher dispatch through it.
    cairns::control::CommandRegistry registry;

#ifndef __EMSCRIPTEN__
    // Live agent transport: a stdin reader thread + drain on each
    // SDL_AppIterate. Disabled unless CAIRNS_AGENT_STDIN=1. Native-only -- a
    // browser has no stdin; the web build is driven via window.cairns.dispatch.
    cairns::control::AgentStdinDrain agent_drain;
#endif
    bool agent_quit = false;

    // Imgui scenario picker (boots blank; user clicks to run a scripts/*.js).
    cairns::ScenarioLauncher launcher;

    SDL_AppResult app_quit = SDL_APP_CONTINUE;

    // Fly-cam: RMB held => mouse-look + WASD/hjkl/QE/Shift drive
    // engine->ApplyFlyMovement each iterate. last_iter_ns_ is the timestamp
    // of the previous SDL_AppIterate so the per-frame dt is wall-time.
    bool rmb_look = false;
    uint64_t last_iter_ns_ = 0;

    ~AppContext() {
        delete engine;
        engine = nullptr;
    }
};

SDL_AppResult SDL_Fail(){
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Error %s", SDL_GetError());
    return SDL_APP_FAILURE;
}

#ifdef __EMSCRIPTEN__
// window.cairns.dispatch bridge. The registry lives in the heap-owned AppContext;
// this file-scope pointer is set once SDL_AppInit builds it. Emscripten's C-ABI
// export forces a single reachable pointer -- framework-forced, not a singleton.
namespace {
cairns::control::CommandRegistry* g_web_registry = nullptr;
}  // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE
char* cairns_dispatch(const char* op, const char* args_json) {
    // The bridge is published to JS (shell.html onRuntimeInitialized) BEFORE
    // SDL_AppInit wires g_web_registry, and AppInit yields to the JS loop
    // mid-init (ASYNCIFY device bootstrap). A dispatch in that window must not
    // deref null -- that aborts the WASM and crashes the tab.
    if (g_web_registry == nullptr) {
        const char* kBooting = "{\"ok\":false,\"error\":\"engine still booting\"}";
        const size_t n = std::strlen(kBooting) + 1;
        char* out = static_cast<char*>(std::malloc(n));
        std::memcpy(out, kBooting, n);
        return out;
    }
    cairns::json req;
    req["op"] = op ? op : "";
    if (args_json && args_json[0]) {
        req["args"] = cairns::json::parse(args_json, nullptr, false);
    }
    const cairns::json resp = g_web_registry->Dispatch(req);
    const std::string s = resp.dump();
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

EMSCRIPTEN_KEEPALIVE
void cairns_free(char* p) { std::free(p); }

}  // extern "C"
#endif  // __EMSCRIPTEN__

SDL_AppResult SDL_AppInit(void** appstate, [[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {

#ifdef __ANDROID__
    // Android launches via the SDLActivity wrapper -- no shell to pass env
    // vars. Default to a small entity count so the Adreno tile budget isn't
    // blown by the 3300-hero benchmark.
    setenv("CAIRNS_N", "500", 0);
#endif

    constexpr uint32_t kWindowStartWidth = 1280;
    constexpr uint32_t kWindowStartHeight = 720;

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    SDL_InitFlags init_flags = SDL_INIT_VIDEO;
#ifndef __EMSCRIPTEN__
    init_flags |= SDL_INIT_AUDIO;
#endif
    if (!SDL_Init(init_flags)) {
        return SDL_Fail();
    }

    SDL_Window* window = nullptr;
    cairns::Engine* engine = nullptr;

    // Fixed 1280x720, no HIGH_PIXEL_DENSITY on BOTH webgpu shells (web canvas +
    // native metal-layer): they present by copying the 1280x720 final_target
    // into the surface, so a dpr-scaled window would blit-scale (blurry). Only
    // metal/vk (direct swapchain) take the window's native hi-dpi backing.
    SDL_WindowFlags win_flags = cairns::shell::BackendWindowFlag();
#if !defined(__EMSCRIPTEN__) && !CAIRNS_WEBGPU
    win_flags |= SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    window = SDL_CreateWindow(
        cairns::shell::BackendWindowTitle(), kWindowStartWidth, kWindowStartHeight,
        win_flags);
    if (!window) {
        return SDL_Fail();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForOther(window)) {
        return SDL_Fail();
    }

    cairns::rhi::InitConfig rhi_cfg{};
    rhi_cfg.surfaceless = false;
    rhi_cfg.width = kWindowStartWidth;
    rhi_cfg.height = kWindowStartHeight;

    void* shell_handle = cairns::shell::AttachWindow(window, rhi_cfg);

    engine = new cairns::Engine;
    const cairns::EngineConfig ecfg =
        cairns::shell::LoadEngineConfigFromEnv();
    if (!engine->GreaterInit(rhi_cfg, ecfg)) {
        return SDL_Fail();
    }
    // Blank boot: no particles until a scenario asks for them (the perf
    // smokes do). The launcher reset also disables them between scenarios.
    engine->EnableParticles(false);
    // Surfaceless webgpu (web) renders offscreen then copies to the canvas, so it
    // must opt into imgui explicitly (windowed metal/vk get it via !surfaceless;
    // cairns_serve stays imgui-free). Harmless on native.
    engine->SetImguiEnabled(true);

    *appstate = new AppContext{
        .window = window,
        .shell_handle = shell_handle,
#ifndef __EMSCRIPTEN__
        .audioDevice = 0,
        .track = nullptr,
#endif
        .engine = engine,
    };

    AppContext* app_ctx = static_cast<AppContext*>(*appstate);
    auto& registry = app_ctx->registry;
    cairns::control::RegisterLifecycleOps(registry, app_ctx->agent_quit);
    cairns::control::RegisterPerfOps(registry, *engine);
    // Live agent surface (target="window" path on io.dumpTexture). render.frame
    // returns an error in windowed mode (windowed has its own draw loop;
    // there's nothing to "render once" through the registry).
    cairns::control::RegisterRenderOps(registry, *engine);
    cairns::control::RegisterSceneOps(registry, *engine);
    cairns::control::RegisterEntityOps(registry, *engine);
    cairns::control::RegisterSelectionOps(registry, *engine);
    // Script ops LAST so tools.list inside script.eval reflects every
    // other op already registered.
    cairns::control::RegisterScriptOps(registry, app_ctx->script_host);
#ifdef __EMSCRIPTEN__
    g_web_registry = &registry;
#endif
    // Boot blank -- no script auto-load. Everything starts empty except the
    // perf HUD + the scenario picker; the user clicks to run a scripts/*.js.
    app_ctx->launcher.Enumerate();
    engine->SetImguiPanel(&cairns::DrawScenarioPanel, &app_ctx->launcher);
#ifndef __EMSCRIPTEN__
    app_ctx->agent_drain.Start(cairns::shell::AgentStdinEnabledFromEnv());
    if (app_ctx->agent_drain.Enabled()) {
        std::fprintf(stderr,
                     "[Agent] stdin transport on -- send NDJSON to drive "
                     "this window.\n");
    }
#endif

    SDL_ShowWindow(window);
    SDL_Log("cairns Application started successfully!");

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event* event) {
    auto* app = (AppContext*)appstate;

    ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == SDL_EVENT_QUIT) {
        app->app_quit = SDL_APP_SUCCESS;
    }
    else if (event->type == SDL_EVENT_KEY_DOWN) {
        // Z dumps the current swap frame to disk. D is RIGHT in WASD nav --
        // do NOT bind dump there.
        if (event->key.scancode == SDL_SCANCODE_Z && app->engine) {
            app->engine->RequestViewportDump("/tmp/cairns_dump.png");
        }
    }
    // RMB-held → relative mouse mode + mouse-look. Skipped while ImGui has
    // mouse focus (e.g. cursor over the perf panel) so dragging widgets
    // doesn't also rotate the camera.
    else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event->button.button == SDL_BUTTON_RIGHT &&
            !ImGui::GetIO().WantCaptureMouse) {
            SDL_SetWindowRelativeMouseMode(app->window, true);
            app->rmb_look = true;
        } else if (event->button.button == SDL_BUTTON_LEFT &&
                    !ImGui::GetIO().WantCaptureMouse && app->engine) {
            // Plain LMB both picks the viewport for input routing AND
            // records the pick intent.
            // SDL3 mouse events are in window units (points); with
            // SDL_WINDOW_HIGH_PIXEL_DENSITY the framebuffer / id_target_
            // is in pixels (2x on Retina). Scale to pixel space.
            const float density = SDL_GetWindowPixelDensity(app->window);
            const float pix_x = event->button.x * density;
            const float pix_y = event->button.y * density;
            app->engine->SetActiveViewportFromClickX(pix_x);
            const int vp = app->engine->ActiveViewport();
            const uint32_t fb_w = app->engine->FrameWidth();
            const int n_live =
                app->engine->ActiveViewportCount() <= 0
                    ? 1
                    : app->engine->ActiveViewportCount();
            const uint32_t vp_w = fb_w / static_cast<uint32_t>(n_live);
            const uint32_t local_x = static_cast<uint32_t>(
                pix_x - static_cast<float>(vp) *
                        static_cast<float>(vp_w));
            // No Y-flip: forward pass's negative-height viewport maps
            // NDC y=+1 (screen top) -> fragcoord_y=0, so screen y =
            // texel y directly.
            const uint32_t local_y = static_cast<uint32_t>(pix_y);
            app->engine->RequestPick(vp, local_x, local_y);
        }
    }
    // Android touch -> pick. SDL_EVENT_FINGER_DOWN.x/y are normalized
    // [0..1] of the window; convert to pixel space against the engine's
    // current framebuffer dims (same target as the mouse path above).
    else if (event->type == SDL_EVENT_FINGER_DOWN && app->engine) {
        const uint32_t fb_w = app->engine->FrameWidth();
        const uint32_t fb_h = app->engine->FrameHeight();
        const float pix_x = event->tfinger.x * static_cast<float>(fb_w);
        const float pix_y = event->tfinger.y * static_cast<float>(fb_h);
        app->engine->SetActiveViewportFromClickX(pix_x);
        const int vp = app->engine->ActiveViewport();
        const int n_live =
            app->engine->ActiveViewportCount() <= 0
                ? 1
                : app->engine->ActiveViewportCount();
        const uint32_t vp_w = fb_w / static_cast<uint32_t>(n_live);
        const uint32_t local_x = static_cast<uint32_t>(
            pix_x - static_cast<float>(vp) *
                    static_cast<float>(vp_w));
        const uint32_t local_y = static_cast<uint32_t>(pix_y);
        app->engine->RequestPick(vp, local_x, local_y);
    }
    else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event->button.button == SDL_BUTTON_RIGHT && app->rmb_look) {
            SDL_SetWindowRelativeMouseMode(app->window, false);
            app->rmb_look = false;
        }
    }
    else if (event->type == SDL_EVENT_MOUSE_MOTION) {
        if (app->rmb_look && app->engine) {
            constexpr float kMouseSensitivity = 0.0025f;  // rad / pixel
            // SDL: +xrel = mouse right. With camera_dir = (-cp*sy, sp, -cp*cy),
            // positive yaw rotates the look direction toward -X (CCW from
            // above) -- so mouse-right wants dyaw < 0 to swing the view +X.
            // +yrel = mouse down; pitch convention is "positive = look up",
            // and looking down = pitch decreases, so -yrel keeps it natural.
            const float dyaw = -event->motion.xrel * kMouseSensitivity;
            const float dpitch = -event->motion.yrel * kMouseSensitivity;
            app->engine->ApplyMouseLook(dyaw, dpitch);
        }
    }
    else if ( event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        const int newWidth = event->window.data1;
        const int newHeight = event->window.data2;
        if ( app->engine) {
            app->engine->requestResizeFrameBuffer(newWidth, newHeight);
        }
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    auto* app = (AppContext*)appstate;

    [[maybe_unused]] cairns::TaskGuard task_guard;

    // Drain any pending agent commands BEFORE the frame so the agent's
    // mutations land on this frame's render. Responses go to stdout; logs
    // / [Timer] / [FLAKE] stay on stderr per the protocol contract.
#ifndef __EMSCRIPTEN__
    app->agent_drain.Drain(app->registry, stdout);
#endif
    if (app->agent_quit) {
        app->app_quit = SDL_APP_SUCCESS;
    }

    // Scenario picker: if the user clicked a scenario last frame, reset to
    // blank and eval its script -- same safe pre-draw point as the agent drain.
    // The reset is fixed (scene + prefabs + render modes), not per-scenario.
    if (app->launcher.pending >= 0) {
        const int idx = app->launcher.pending;
        app->launcher.pending = -1;
        app->launcher.current = idx;
        auto& reg = app->registry;
        reg.Dispatch(cairns::json{{"op", "cairns.scene.clear"}});
        reg.Dispatch(cairns::json{{"op", "cairns.prefab.unloadAll"}});
        reg.Dispatch(cairns::json{{"op", "cairns.render.nestedGraph"},
                                  {"args", {{"on", false}}}});
        reg.Dispatch(cairns::json{{"op", "cairns.particles.enable"},
                                  {"args", {{"on", false}}}});
        // ReadAsset (SDL_LoadFile) reads APK assets on Android; std::ifstream
        // can't. Desktop reads the file the same way.
        std::string code;
        if (cairns::platform::ReadAsset(app->launcher.scripts[idx].path, code)) {
            reg.Dispatch(cairns::json{{"op", "cairns.script.eval"},
                                      {"args", {{"code", code}}}});
        }
    }

    // Fly-cam: sample keyboard state once per iterate and drive the
    // active viewport's FlyController. Skipped under CAIRNS_CAM_POSE (the
    // engine bails inside ApplyFlyMovement) so byte-gate dumps stay
    // deterministic regardless of any held keys. WASD + vim hjkl share a
    // single resolved (right, up, forward) vector.
    if (app->engine && !app->engine->CamPoseOverridden()) {
        const uint64_t now_ns = SDL_GetTicksNS();
        const float dt = app->last_iter_ns_ == 0
            ? 0.0f
            : std::min(0.05f, static_cast<float>((now_ns - app->last_iter_ns_) * 1e-9));
        app->last_iter_ns_ = now_ns;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        glm::vec3 input{0.0f};
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_L]) { input.x += 1.0f; }
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_H]) { input.x -= 1.0f; }
        if (keys[SDL_SCANCODE_E])                          { input.y += 1.0f; }
        if (keys[SDL_SCANCODE_Q])                          { input.y -= 1.0f; }
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_K]) { input.z += 1.0f; }
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_J]) { input.z -= 1.0f; }
        if (glm::dot(input, input) > 0.0f) {
            const float speed = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT])
                ? 12.0f : 4.0f;
            app->engine->ApplyFlyMovement(input * (dt * speed));
        }
    }

    if ( app->engine) {
        if ( !app->engine->draw()) {
            return SDL_APP_CONTINUE;
        }
    }

    return app->app_quit;
}

void SDL_AppQuit(void* appstate, [[maybe_unused]] SDL_AppResult result) {
    auto* app = (AppContext*)appstate;
    if (app) {
#ifndef __EMSCRIPTEN__
        app->agent_drain.Stop();
#endif
        if ( app->engine ) {
            app->engine->deinit();
        }
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        cairns::shell::DetachWindow(app->shell_handle);
        if (app->window) {
            SDL_DestroyWindow(app->window);
        }
        delete app;
    }

    SDL_Quit();
}
