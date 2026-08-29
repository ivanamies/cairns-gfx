// src/web/web_main.cpp -- Emscripten/Chrome entry for the WebGPU backend.
//
// The browser is async-only: the adapter/device requests cannot be busy-waited
// (their callbacks fire only when control returns to the JS event loop). So we
// acquire instance -> surface -> adapter -> device through a callback chain,
// then inject them into the engine (surfaceless render) and drive one frame per
// requestAnimationFrame. Each frame the offscreen final_target_ is copied into
// the canvas surface + presented -- reusing the entire native headless render
// path (no windowed swapchain pipeline, no MSAA/depth-on-swap divergence).
//
// Control: window.cairns.dispatch(op, args) -> cairns_dispatch -> the same
// CommandRegistry the headless NDJSON host uses. No SDL anywhere.
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include <emscripten.h>
#include <emscripten/html5.h>
#include <webgpu/webgpu.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>

#include "imgui.h"

#include "control/boot_run.hpp"
#include "control/command_registry.hpp"
#include "control/handlers/lifecycle_ops.hpp"
#include "control/handlers/perf_ops.hpp"
#include "control/handlers/render_ops.hpp"
#include "control/handlers/scene_ops.hpp"
#include "control/handlers/script_ops.hpp"
#include "control/handlers/selection_ops.hpp"
#include "engine.hpp"
#include "rhi/init_config.hpp"
#include "shell/env_config.hpp"
#include "util/json.hpp"

namespace {

struct WebApp {
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUSurface surface = nullptr;
    WGPUTextureFormat format = WGPUTextureFormat_BGRA8Unorm;
    uint32_t width = 1280;
    uint32_t height = 720;
    cairns::Engine* engine = nullptr;
    bool quit = false;
    bool ready = false;
};

// Heap-owned (callbacks outlive main); reached only via main-loop arg + the
// dispatch bridge. Not a global state container -- the registry it talks to is
// the existing CommandRegistry singleton.
WebApp* g_web = nullptr;

WGPUStringView Sv(const char* s) { return WGPUStringView{s, WGPU_STRLEN}; }

void Frame(void* arg) {
    WebApp* app = static_cast<WebApp*>(arg);
    if (!app->ready || !app->engine) { return; }
    app->engine->RenderHeadlessFrame();  // renders into the offscreen final_target_

    void* ftex = app->engine->FinalTargetNativeTexture();
    if (!ftex || !app->surface) { return; }
    WGPUSurfaceTexture st = {};
    wgpuSurfaceGetCurrentTexture(app->surface, &st);
    if (!st.texture) { return; }
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(app->device, nullptr);
    WGPUTexelCopyTextureInfo src = {};
    src.texture = static_cast<WGPUTexture>(ftex);
    src.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyTextureInfo dst = {};
    dst.texture = st.texture;
    dst.aspect = WGPUTextureAspect_All;
    WGPUExtent3D ext = {app->width, app->height, 1};
    wgpuCommandEncoderCopyTextureToTexture(enc, &src, &dst, &ext);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(app->queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    // No wgpuSurfacePresent in the browser: emdawnwebgpu auto-presents the
    // surface's current texture when this rAF callback returns.
}

void StartEngine(WebApp* app) {
    WGPUSurfaceConfiguration sc = {};
    sc.device = app->device;
    sc.format = app->format;
    sc.usage = static_cast<WGPUTextureUsage>(WGPUTextureUsage_RenderAttachment |
                                             WGPUTextureUsage_CopyDst);
    sc.width = app->width;
    sc.height = app->height;
    sc.alphaMode = WGPUCompositeAlphaMode_Auto;
    sc.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(app->surface, &sc);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    app->engine = new cairns::Engine;
    cairns::rhi::InitConfig cfg{};
    cfg.surfaceless = true;  // render offscreen; Frame() copies it to the canvas
    cfg.width = app->width;
    cfg.height = app->height;
    cfg.plat.instance = app->instance;
    cfg.plat.adapter = app->adapter;
    cfg.plat.device = app->device;
    cfg.plat.queue = app->queue;
    cfg.plat.surface = app->surface;
    cfg.plat.surface_format = app->format;
    const cairns::EngineConfig ecfg = cairns::shell::LoadEngineConfigFromEnv();
    if (!app->engine->GreaterInit(cfg, ecfg)) {
        std::fprintf(stderr, "[web] GreaterInit failed\n");
        return;
    }

    auto& reg = cairns::control::CommandRegistry::Instance();
    cairns::control::RegisterLifecycleOps(reg, app->quit);
    cairns::control::RegisterRenderOps(reg, *app->engine);
    cairns::control::RegisterSceneOps(reg, *app->engine);
    cairns::control::RegisterPerfOps(reg, *app->engine);
    cairns::control::RegisterSelectionOps(reg, *app->engine);
    cairns::control::RegisterScriptOps(reg);
    // No RunBootScript: run.js is the native 500-actor perf workload (loadBatch
    // 100 + instantiateGrid x5). The browser boots empty; scenarios spawn on a
    // button click via window.cairns.dispatch.

    app->ready = true;
    std::fprintf(stderr, "[web] engine ready (%ux%u)\n", app->width, app->height);
    emscripten_set_main_loop_arg(Frame, app, 0, /*simulate_infinite_loop=*/false);
}

void OnDevice(WGPURequestDeviceStatus, WGPUDevice dev, WGPUStringView,
              void* u1, void*) {
    WebApp* app = static_cast<WebApp*>(u1);
    if (!dev) { std::fprintf(stderr, "[web] device request failed\n"); return; }
    app->device = dev;
    app->queue = wgpuDeviceGetQueue(dev);
    StartEngine(app);
}

void OnAdapter(WGPURequestAdapterStatus, WGPUAdapter ad, WGPUStringView,
               void* u1, void*) {
    WebApp* app = static_cast<WebApp*>(u1);
    if (!ad) { std::fprintf(stderr, "[web] adapter request failed\n"); return; }
    app->adapter = ad;
    WGPULimits limits = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(ad, &limits);
    WGPUDeviceDescriptor dd = {};
    dd.requiredLimits = &limits;
    WGPURequestDeviceCallbackInfo cb = {};
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = OnDevice;
    cb.userdata1 = app;
    wgpuAdapterRequestDevice(ad, &dd, cb);
}

}  // namespace

extern "C" {

// window.cairns.dispatch bridge. Returns a malloc'd JSON string; JS reads it via
// UTF8ToString then frees it with cairns_free.
EMSCRIPTEN_KEEPALIVE
char* cairns_dispatch(const char* op, const char* args_json) {
    cairns::json req;
    req["op"] = op ? op : "";
    if (args_json && args_json[0]) {
        req["args"] = cairns::json::parse(args_json, nullptr, false);
    }
    cairns::json resp =
        cairns::control::CommandRegistry::Instance().Dispatch(req);
    const std::string s = resp.dump();
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

EMSCRIPTEN_KEEPALIVE
void cairns_free(char* p) { std::free(p); }

}  // extern "C"

int main() {
    g_web = new WebApp;
    g_web->instance = wgpuCreateInstance(nullptr);
    if (!g_web->instance) {
        std::fprintf(stderr, "[web] no WebGPU instance (browser too old?)\n");
        return 1;
    }
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas_src = {};
    canvas_src.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas_src.selector = Sv("#canvas");
    WGPUSurfaceDescriptor surf_desc = {};
    surf_desc.nextInChain = &canvas_src.chain;
    g_web->surface = wgpuInstanceCreateSurface(g_web->instance, &surf_desc);

    WGPURequestAdapterCallbackInfo acb = {};
    acb.mode = WGPUCallbackMode_AllowSpontaneous;
    acb.callback = OnAdapter;
    acb.userdata1 = g_web;
    wgpuInstanceRequestAdapter(g_web->instance, nullptr, acb);
    return 0;  // main returns; the callback chain + rAF loop drive the rest
}

#endif  // CAIRNS_WEBGPU
