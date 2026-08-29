#define SDL_MAIN_USE_CALLBACKS  // This is necessary for the new callbacks API. To use the legacy API, don't define this.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_init.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3_image/SDL_image.h>

#if CAIRNS_VULKAN
#include <SDL3/SDL_vulkan.h>
#endif
#if CAIRNS_METAL
#include <SDL3/SDL_metal.h>
#endif

#include <vector>
#include <string>
#include <memory>
#include <set>

#include <glm/glm.hpp>

#include "util/define.hpp"
#include "util/gltf_loader.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"

#include "engine.hpp"
#include "rhi/init_config.hpp"
#include "util/task_guard.hpp"

#include <iostream>
#include "control/agent_stdin_drain.hpp"
#include "control/command_registry.hpp"
#include "control/handlers/lifecycle_ops.hpp"
#include "control/handlers/perf_ops.hpp"
#include "control/handlers/render_ops.hpp"
#include "control/handlers/scene_ops.hpp"

namespace cairns {


} // namespace cairns


struct AppContext {
    SDL_Window* window = nullptr;
#if CAIRNS_METAL
    SDL_MetalView metal_view = nullptr;
#endif

    // Audio
    SDL_AudioDeviceID audioDevice;
    MIX_Track* track = nullptr;

    cairns::Engine* engine = nullptr;

    // Live agent transport: a stdin reader thread + drain on each
    // SDL_AppIterate. Disabled unless CAIRNS_AGENT_STDIN=1.
    cairns::control::AgentStdinDrain agent_drain;
    bool agent_quit = false;

    SDL_AppResult app_quit = SDL_APP_CONTINUE;

    // P1 fly-cam: RMB held => mouse-look + WASD/hjkl/QE/Shift drive
    // engine->ApplyFlyMovement each iterate. last_iter_ns_ is the timestamp
    // of the previous SDL_AppIterate so the per-frame dt is wall-time.
    bool rmb_look = false;
    uint64_t last_iter_ns_ = 0;

    ~AppContext() {
        delete engine;
        engine = nullptr;
    }
};

#if CAIRNS_VULKAN
// Shell-side VkSurfaceKHR creation -- invoked by rhi::Device::Init after the
// VkInstance is up. user is the SDL_Window*.
static bool ShellVkCreateSurface(void* user, VkInstance instance,
                                 VkSurfaceKHR* out_surface) {
    SDL_Window* window = static_cast<SDL_Window*>(user);
    return SDL_Vulkan_CreateSurface(window, instance, nullptr, out_surface);
}

// Shell-side window pixel-size getter for SwapChain resize handling.
static void ShellVkWindowSize(void* user, int* w, int* h) {
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(user), w, h);
}
#endif

SDL_AppResult SDL_Fail(){
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Error %s", SDL_GetError());
    return SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppInit(void** appstate, [[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {

    constexpr uint32_t kWindowStartWidth = 1280;
    constexpr uint32_t kWindowStartHeight = 720;

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)){
        return SDL_Fail();
    }

    SDL_Window* window = nullptr;
    cairns::Engine* engine = nullptr;

#if CAIRNS_METAL
    constexpr SDL_WindowFlags kBackendWindowFlag = SDL_WINDOW_METAL;
    const char* kWindowTitle = "SDL + Metal-cpp Sample";
#elif CAIRNS_VULKAN
    constexpr SDL_WindowFlags kBackendWindowFlag = SDL_WINDOW_VULKAN;
    const char* kWindowTitle = "SDL + Vulkan Sample";
#endif
    window = SDL_CreateWindow(kWindowTitle, kWindowStartWidth, kWindowStartHeight,
                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | kBackendWindowFlag);
    if (!window) {
        return SDL_Fail();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL3_InitForOther(window)) {
        return SDL_Fail();
    }

    // Build the platform-handle InitConfig the RHI consumes.
    cairns::rhi::InitConfig rhi_cfg{};
    rhi_cfg.surfaceless = false;
    rhi_cfg.width = kWindowStartWidth;
    rhi_cfg.height = kWindowStartHeight;

#if CAIRNS_VULKAN
    uint32_t sdl_ext_count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    rhi_cfg.vk_instance_extensions = sdl_exts;
    rhi_cfg.vk_instance_extension_count = sdl_ext_count;
    rhi_cfg.vk_create_surface = &ShellVkCreateSurface;
    rhi_cfg.vk_create_surface_user = window;
    rhi_cfg.vk_window_size = &ShellVkWindowSize;
    rhi_cfg.vk_window_size_user = window;
#endif

#if CAIRNS_METAL
    SDL_MetalView metal_view = SDL_Metal_CreateView(window);
    if (!metal_view) {
        return SDL_Fail();
    }
    rhi_cfg.metal_layer =
        static_cast<CA::MetalLayer*>(SDL_Metal_GetLayer(metal_view));
    if (!rhi_cfg.metal_layer) {
        return SDL_Fail();
    }
#endif

    engine = new cairns::Engine;
    if (!engine->GreaterInit(rhi_cfg)) {
        return SDL_Fail();
    }

    // Setup App State
    *appstate = new AppContext{
        .window = window,
#if CAIRNS_METAL
        .metal_view = metal_view,
#endif
        .audioDevice = 0,
        .track = nullptr,
        .engine = engine,
    };

    // Live agent transport setup (no-op unless CAIRNS_AGENT_STDIN is set).
    AppContext* app_ctx = static_cast<AppContext*>(*appstate);
    auto& registry = cairns::control::CommandRegistry::Instance();
    cairns::control::RegisterLifecycleOps(registry, &app_ctx->agent_quit);
    cairns::control::RegisterPerfOps(registry, engine);
    // Live agent surface (target="window" path on io.dumpTexture). render.frame
    // returns an error in windowed mode (windowed has its own draw loop;
    // there's nothing to "render once" through the registry).
    cairns::control::RegisterRenderOps(registry, engine);
    cairns::control::RegisterSceneOps(registry, engine);
    app_ctx->agent_drain.Start();
    if (app_ctx->agent_drain.Enabled()) {
        std::fprintf(stderr,
                     "[Agent] stdin transport on -- send NDJSON to drive "
                     "this window.\n");
    }

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
        if (event->key.scancode == SDL_SCANCODE_D && app->engine) {
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
            // P2 click-to-focus: which half of the swap target did the
            // click land in? Subsequent WASD/RMB-look drives that viewport.
            app->engine->SetActiveViewportFromClickX(event->button.x);
        }
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
            const float dyaw = event->motion.xrel * kMouseSensitivity;
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
    app->agent_drain.Drain(cairns::control::CommandRegistry::Instance(),
                           std::cout);
    if (app->agent_quit) {
        app->app_quit = SDL_APP_SUCCESS;
    }

    // P1 fly-cam: sample keyboard state once per iterate and drive the
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
        app->agent_drain.Stop();
        if ( app->engine ) {
            app->engine->deinit();
        }
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
#if CAIRNS_METAL
        if (app->metal_view) {
            SDL_Metal_DestroyView(app->metal_view);
        }
#endif
        if (app->window) {
            SDL_DestroyWindow(app->window);
        }
        delete app;
    }

    SDL_Quit();
}
