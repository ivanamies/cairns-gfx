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

#include "metal_engine.hpp"

#include "vulkan_engine.hpp"

namespace cairns {


} // namespace cairns

struct AppContext {
    SDL_Window* window = nullptr;
    
    // Audio
    SDL_AudioDeviceID audioDevice;
    MIX_Track* track = nullptr;
    
#if CAIRNS_METAL
    cairns::Engine* engine = nullptr;
#endif
#if CAIRNS_VULKAN
    cairns::Engine2* engine2 = nullptr;
#endif

    SDL_AppResult app_quit = SDL_APP_CONTINUE;

    ~AppContext() {
#if CAIRNS_METAL
        delete engine;
        engine = nullptr;
#endif
#if CAIRNS_VULKAN
        delete engine2;
        engine2 = nullptr;
#endif
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
#if CAIRNS_METAL
    cairns::Engine* engine = nullptr;
#endif
#if CAIRNS_VULKAN
    cairns::Engine2* engine2 = nullptr;
#endif

#if CAIRNS_METAL
    window = SDL_CreateWindow("SDL + Metal-cpp Sample", kWindowStartWidth, kWindowStartHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL);
    if (!window){
        return SDL_Fail();
    }
    engine = new cairns::Engine;
    if ( !engine->GreaterInit(window)) {
        return SDL_Fail();
    }
#elif CAIRNS_VULKAN
    window = SDL_CreateWindow("SDL + Vulkan Sample", kWindowStartWidth, kWindowStartHeight, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN);
    if ( !window ) {
        return SDL_Fail();
    }
    engine2 = new cairns::Engine2;
    if ( !engine2->GreaterInit(window)) {
        return SDL_Fail();
    }
#endif

    // Setup App State
    *appstate = new AppContext{
        .window = window,
        .track = nullptr,
#if CAIRNS_METAL
        .engine = engine,
#endif
#if CAIRNS_VULKAN
        .engine2 = engine2,
#endif
    };
    
    SDL_ShowWindow(window);
    SDL_Log("Metal Application started successfully!");
    
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event* event) {
    auto* app = (AppContext*)appstate;
    
    if (event->type == SDL_EVENT_QUIT) {
        app->app_quit = SDL_APP_SUCCESS;
    }
    else if (event->type == SDL_EVENT_KEY_DOWN) {
#if CAIRNS_METAL
        if (event->key.scancode == SDL_SCANCODE_D && app->engine) {
            app->engine->RequestViewportDump("/tmp/cairns_dump.png");
        }
#endif
    }
    else if ( event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        const int newWidth = event->window.data1;
        const int newHeight = event->window.data2;
#if CAIRNS_METAL
        if ( app->engine) {
            app->engine->requestResizeFrameBuffer(newWidth, newHeight);
        }
#endif
#if CAIRNS_VULKAN
        if ( app->engine2) {
            app->engine2->RequestResizeFrameBuffer(newWidth, newHeight);
        }
#endif
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
    auto* app = (AppContext*)appstate;
    
    // --- Metal Render Loop ---
    
#if CAIRNS_METAL
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    if ( app->engine) {
        if ( !app->engine->draw()) {
            return SDL_APP_CONTINUE;
        }
    }
    pool->release();
#endif
#if CAIRNS_VULKAN
    if (app->engine2 ) {
        if ( !app->engine2->Draw()) {
            return SDL_APP_CONTINUE;
        }
    }
#endif
    
    return app->app_quit;
}

void SDL_AppQuit(void* appstate, [[maybe_unused]] SDL_AppResult result) {
    auto* app = (AppContext*)appstate;
    if (app) {
#if CAIRNS_METAL
        if ( app->engine ) {
            app->engine->deinit();
        }
#endif
#if CAIRNS_VULKAN
        if ( app->engine2 ) {
            app->engine2->Deinit();
        }
#endif
        delete app;
    }
    if (app->window) SDL_DestroyWindow(app->window);

    SDL_Quit();
}
