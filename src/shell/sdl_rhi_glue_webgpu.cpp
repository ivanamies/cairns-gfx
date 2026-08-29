// src/shell/sdl_rhi_glue_webgpu.cpp -- WebGPU window glue (the sdl3webgpu
// pattern): SDL owns the window/canvas + input + main loop; this mints the
// WGPUSurface from it and acquires the adapter/device. Two forks: browser
// (canvas selector + emscripten_sleep) and native macOS (metal-layer surface +
// wgpuInstanceProcessEvents spin); both feed Device::Init's adopt path.

#include "util/define.hpp"

#if CAIRNS_WEBGPU

#include "shell/sdl_rhi_glue.hpp"

#include <SDL3/SDL.h>
#ifndef __EMSCRIPTEN__
#include <SDL3/SDL_metal.h>
#endif
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
    return SDL_WINDOW_METAL;  // native windowed surface via the metal layer
#endif
}

const char* BackendWindowTitle() { return "SDL + WebGPU"; }

// Shared by both forks: the browser spins emscripten_sleep, native spins
// wgpuInstanceProcessEvents, but both wait on the same request latch.
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
        wgpuSurfaceRelease(surface);
        wgpuInstanceRelease(instance);
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
        wgpuAdapterRelease(areq.adapter);
        wgpuSurfaceRelease(surface);
        wgpuInstanceRelease(instance);
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
    // Native (macOS): SDL mints a CAMetalLayer-backed view; wgpu-native wraps it
    // in a surface (the sdl3webgpu pattern). We CAN busy-wait the async
    // adapter/device here (unlike the browser) -- wgpuInstanceProcessEvents
    // drives the AllowProcessEvents callbacks -- so acquire the whole chain and
    // hand it to Device::Init's adopt path. Surface + present-by-copy identical
    // to web. NOTE: on the SUCCESS path Device::Init adopts with
    // owns_handles=false, so the handles + surface leak at process exit (no
    // webgpu windowed path releases them yet) -- TODO. The error paths below DO
    // release what they minted.
    SDL_MetalView view = SDL_Metal_CreateView(window);
    if (!view) {
        return nullptr;
    }
    // SDL_Metal_GetLayer returns the CAMetalLayer as void* -- exactly what
    // WGPUSurfaceSourceMetalLayer.layer wants (no metal-cpp cast, unlike the
    // metal backend whose init_config field is CA::MetalLayer*-typed).
    void* metal_layer = SDL_Metal_GetLayer(view);
    if (!metal_layer) {
        SDL_Metal_DestroyView(view);
        return nullptr;
    }
    WGPUInstance instance = wgpuCreateInstance(nullptr);
    if (!instance) {
        SDL_Metal_DestroyView(view);
        return nullptr;
    }
    WGPUSurfaceSourceMetalLayer metal_src = {};
    metal_src.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
    metal_src.layer = metal_layer;
    WGPUSurfaceDescriptor surf_desc = {};
    surf_desc.nextInChain = &metal_src.chain;
    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surf_desc);
    if (!surface) {
        wgpuInstanceRelease(instance);
        SDL_Metal_DestroyView(view);
        return nullptr;
    }

    AdapterReq areq;
    WGPURequestAdapterOptions aopts = {};
    aopts.compatibleSurface = surface;  // an adapter that can present here
    WGPURequestAdapterCallbackInfo acb = {};
    acb.mode = WGPUCallbackMode_AllowProcessEvents;
    acb.callback = [](WGPURequestAdapterStatus, WGPUAdapter ad, WGPUStringView,
                      void* u1, void*) {
        AdapterReq* r = static_cast<AdapterReq*>(u1);
        r->adapter = ad;
        r->done = true;
    };
    acb.userdata1 = &areq;
    wgpuInstanceRequestAdapter(instance, &aopts, acb);
    for (int i = 0; i < 1000 && !areq.done; ++i) {
        wgpuInstanceProcessEvents(instance);
    }
    if (!areq.adapter) {
        wgpuSurfaceRelease(surface);
        wgpuInstanceRelease(instance);
        SDL_Metal_DestroyView(view);
        return nullptr;
    }

    WGPULimits limits = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(areq.adapter, &limits);
    WGPUDeviceDescriptor dd = {};
    dd.requiredLimits = &limits;
    DeviceReq dreq;
    WGPURequestDeviceCallbackInfo dcb = {};
    dcb.mode = WGPUCallbackMode_AllowProcessEvents;
    dcb.callback = [](WGPURequestDeviceStatus, WGPUDevice dev, WGPUStringView,
                      void* u1, void*) {
        DeviceReq* r = static_cast<DeviceReq*>(u1);
        r->device = dev;
        r->done = true;
    };
    dcb.userdata1 = &dreq;
    wgpuAdapterRequestDevice(areq.adapter, &dd, dcb);
    for (int i = 0; i < 1000 && !dreq.done; ++i) {
        wgpuInstanceProcessEvents(instance);
    }
    if (!dreq.device) {
        wgpuAdapterRelease(areq.adapter);
        wgpuSurfaceRelease(surface);
        wgpuInstanceRelease(instance);
        SDL_Metal_DestroyView(view);
        return nullptr;
    }

    cfg.plat.instance = instance;
    cfg.plat.adapter = areq.adapter;
    cfg.plat.device = dreq.device;
    cfg.plat.queue = wgpuDeviceGetQueue(dreq.device);
    cfg.plat.surface = surface;
    cfg.plat.surface_format = WGPUTextureFormat_BGRA8Unorm;
    cfg.surfaceless = true;  // present-by-copy (Frames::Present), identical to web
    return view;  // shell handle -> DetachWindow destroys the SDL_MetalView
#endif
}

void DetachWindow(void* shell_handle) {
#ifdef __EMSCRIPTEN__
    (void)shell_handle;  // web owns nothing; the browser manages the canvas
#else
    if (shell_handle) {
        SDL_Metal_DestroyView(static_cast<SDL_MetalView>(shell_handle));
    }
#endif
}

}  // namespace cairns::shell

#endif  // CAIRNS_WEBGPU
