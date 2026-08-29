// wgpu-native link smoke (#W0). Proves the vendored libwgpu_native.a + webgpu.h +
// macOS frameworks link and a GPU adapter is reachable. Not part of the engine.
#include <cstdio>

#include <webgpu/webgpu.h>

namespace {
WGPUAdapter g_adapter = nullptr;
bool g_done = false;

void OnAdapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
               WGPUStringView message, void*, void*) {
  if (status == WGPURequestAdapterStatus_Success) {
    g_adapter = adapter;
  } else {
    std::printf("[smoke] adapter request failed: %.*s\n",
                static_cast<int>(message.length),
                message.data ? message.data : "");
  }
  g_done = true;
}
}  // namespace

int main() {
  WGPUInstance instance = wgpuCreateInstance(nullptr);
  if (!instance) {
    std::printf("[smoke] FAIL: no instance\n");
    return 1;
  }

  WGPURequestAdapterCallbackInfo cb = {};
  cb.mode = WGPUCallbackMode_AllowProcessEvents;
  cb.callback = OnAdapter;
  wgpuInstanceRequestAdapter(instance, nullptr, cb);
  for (int i = 0; i < 1000 && !g_done; ++i) {
    wgpuInstanceProcessEvents(instance);
  }
  if (!g_adapter) {
    std::printf("[smoke] FAIL: no adapter\n");
    return 2;
  }

  WGPUAdapterInfo info = {};
  if (wgpuAdapterGetInfo(g_adapter, &info) == WGPUStatus_Success) {
    std::printf("[smoke] adapter: %.*s | %.*s (backend=%d)\n",
                static_cast<int>(info.device.length),
                info.device.data ? info.device.data : "",
                static_cast<int>(info.description.length),
                info.description.data ? info.description.data : "",
                static_cast<int>(info.backendType));
    wgpuAdapterInfoFreeMembers(info);
  }
  wgpuAdapterRelease(g_adapter);
  wgpuInstanceRelease(instance);
  std::printf("[smoke] OK\n");
  return 0;
}
