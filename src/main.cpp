#define SDL_MAIN_USE_CALLBACKS  // This is necessary for the new callbacks API. To use the legacy API, don't define this.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_init.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_mixer/SDL_mixer.h>
#include <SDL3_image/SDL_image.h>

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

namespace cairns {


} // namespace cairns

struct AppContext {
    SDL_Window* window = nullptr;
    
    // Audio
    SDL_AudioDeviceID audioDevice;
    MIX_Track* track = nullptr;
    
    cairns::Engine* engine = nullptr;

    SDL_AppResult app_quit = SDL_APP_CONTINUE;

    ~AppContext() {
        delete engine;
        engine = nullptr;
    }
};

SDL_AppResult SDL_Fail(){
    SDL_LogError(SDL_LOG_CATEGORY_CUSTOM, "Error %s", SDL_GetError());
    return SDL_APP_FAILURE;
}

SDL_AppResult SDL_AppInit(void** appstate, [[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    
    constexpr uint32_t kWindowStartWidth = 720;
    constexpr uint32_t kWindowStartHeight = 1280;

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Portrait");
    
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

    engine = new cairns::Engine;
    if ( !engine->GreaterInit(window)) {
        return SDL_Fail();
    }

    // Setup App State
    *appstate = new AppContext{
        .window = window,
        .audioDevice = 0,
        .track = nullptr,
        .engine = engine,
    };

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

#if CAIRNS_METAL
    // metal-cpp returns autoreleased objects per frame; drain them each iterate.
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
#endif
    if ( app->engine) {
        if ( !app->engine->draw()) {
#if CAIRNS_METAL
            pool->release();
#endif
            return SDL_APP_CONTINUE;
        }
    }
#if CAIRNS_METAL
    pool->release();
#endif

    return app->app_quit;
}

void SDL_AppQuit(void* appstate, [[maybe_unused]] SDL_AppResult result) {
    auto* app = (AppContext*)appstate;
    if (app) {
        if ( app->engine ) {
            app->engine->deinit();
        }
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        if (app->window) {
            SDL_DestroyWindow(app->window);
        }
        delete app;
    }

    SDL_Quit();
}
