// src/shell/sdl_rhi_glue_webgpu.cpp -- WebGPU window glue (the sdl3webgpu
// pattern): SDL owns the window/canvas + input + main loop; this mints the
// WGPUSurface from it and acquires the adapter/device. Browser fork is live;
// the native (macOS metal-layer) fork is M5.

#include "util/define.hpp"

#if CAIRNS_WEBGPU

#include "shell/sdl_rhi_glue.hpp"

#include <SDL3/SDL.h>
#include <webgpu/webgpu.h>

#include "rhi/init_config.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace cairns::shell {

SDL_WindowFlags BackendWindowFlag() {
#ifdef __EMSCRIPTEN__
    // Plain canvas window: SDL's emscripten driver only creates a WebGL context
    // under SDL_WINDOW_OPENGL, so the canvas stays free for WebGPU.
    return 0;
#else
    return SDL_WINDOW_METAL;  // M5: native windowed surface via the metal layer
#endif
}

const char* BackendWindowTitle() { return "SDL + WebGPU"; }

#ifdef __EMSCRIPTEN__
namespace {

struct AdapterReq {
    WGPUAdapter adapter = nullptr;
    bool done = false;
};

struct DeviceReq {
    WGPUDevice device = nullptr;
    bool done = false;
};

}  // namespace
#endif

void* AttachWindow(SDL_Window* window, rhi::InitConfig& cfg) {
#ifdef __EMSCRIPTEN__
    // The browser cannot busy-wait the async adapter/device requests -- their
    // callbacks fire only when control returns to the JS event loop. SDL_AppInit
    // runs inside main() under -sASYNCIFY, so emscripten_sleep here suspends the
    // whole stack until each callback lands, acquiring them inline.
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (!instance) {
        return nullptr;
    }
    const char* selector = SDL_GetStringProperty(
        SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING, "#canvas");
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src = {};
    canvas_src.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas_src.selector = WGPUStringView{selector, WGPU_STRLEN};
    WGPUSurfaceDescriptor surf_desc = {};
    surf_desc.nextInChain = &canvas_src.chain;
    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surf_desc);

    AdapterReq areq;
    WGPURequestAdapterCallbackInfo acb = {};
    acb.mode = WGPUCallbackMode_AllowSpontaneous;
    acb.callback = [](WGPURequestAdapterStatus, WGPUAdapter ad, WGPUStringView,
                      void* u1, void*) {
        AdapterReq* r = static_cast<AdapterReq*>(u1);
        r->adapter = ad;
        r->done = true;
    };
    acb.userdata1 = &areq;
    wgpuInstanceRequestAdapter(instance, nullptr, acb);
    while (!areq.done) {
        emscripten_sleep(1);
    }
    if (!areq.adapter) {
        return nullptr;
    }

    WGPULimits limits = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(areq.adapter, &limits);
    WGPUDeviceDescriptor dd = {};
    dd.requiredLimits = &limits;
    DeviceReq dreq;
    WGPURequestDeviceCallbackInfo dcb = {};
    dcb.mode = WGPUCallbackMode_AllowSpontaneous;
    dcb.callback = [](WGPURequestDeviceStatus, WGPUDevice dev, WGPUStringView,
                      void* u1, void*) {
        DeviceReq* r = static_cast<DeviceReq*>(u1);
        r->device = dev;
        r->done = true;
    };
    dcb.userdata1 = &dreq;
    wgpuAdapterRequestDevice(areq.adapter, &dd, dcb);
    while (!dreq.done) {
        emscripten_sleep(1);
    }
    if (!dreq.device) {
        return nullptr;
    }

    cfg.plat.instance = instance;
    cfg.plat.adapter = areq.adapter;
    cfg.plat.device = dreq.device;
    cfg.plat.queue = wgpuDeviceGetQueue(dreq.device);
    cfg.plat.surface = surface;
    cfg.plat.surface_format = WGPUTextureFormat_BGRA8Unorm;
    // WebGPU presents by copying the offscreen final_target into the surface
    // (Frames::Present): browser-forced (emdawnwebgpu auto-presents on rAF
    // return + the final_target must stay readable for pick/dump), and native
    // matches so one webgpu present model exists.
    cfg.surfaceless = true;
    return nullptr;
#else
    (void)window;
    (void)cfg;
    return nullptr;  // M5: SDL_Metal_GetLayer -> WGPUSurfaceSourceMetalLayer
#endif
}

void DetachWindow(void* shell_handle) {
    (void)shell_handle;  // web owns nothing; M5 destroys the native SDL_MetalView
}

}  // namespace cairns::shell

#endif  // CAIRNS_WEBGPU
