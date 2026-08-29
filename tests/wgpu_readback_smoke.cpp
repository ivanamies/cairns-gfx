// wgpu-native headless readback smoke (#W3 de-risk). device -> texture -> render
// pass clear -> copyTextureToBuffer (256-aligned) -> map -> verify the pixel.
// Standalone; proves the headless golden-readback mechanic before the backend.
#include <cstdint>
#include <cstdio>

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

namespace {
WGPUAdapter g_adapter = nullptr;
WGPUDevice g_device = nullptr;
bool g_adapter_done = false;
bool g_device_done = false;
bool g_map_done = false;

void OnAdapter(WGPURequestAdapterStatus, WGPUAdapter a, WGPUStringView, void*,
               void*) {
  g_adapter = a;
  g_adapter_done = true;
}
void OnDevice(WGPURequestDeviceStatus, WGPUDevice d, WGPUStringView, void*,
              void*) {
  g_device = d;
  g_device_done = true;
}
void OnMap(WGPUMapAsyncStatus, WGPUStringView, void*, void*) {
  g_map_done = true;
}
}  // namespace

int main() {
  WGPUInstance instance = wgpuCreateInstance(nullptr);
  if (!instance) {
    std::printf("[readback] FAIL no instance\n");
    return 1;
  }

  WGPURequestAdapterCallbackInfo acb = {};
  acb.mode = WGPUCallbackMode_AllowProcessEvents;
  acb.callback = OnAdapter;
  wgpuInstanceRequestAdapter(instance, nullptr, acb);
  for (int i = 0; i < 1000 && !g_adapter_done; ++i) {
    wgpuInstanceProcessEvents(instance);
  }
  if (!g_adapter) {
    std::printf("[readback] FAIL no adapter\n");
    return 2;
  }

  WGPURequestDeviceCallbackInfo dcb = {};
  dcb.mode = WGPUCallbackMode_AllowProcessEvents;
  dcb.callback = OnDevice;
  wgpuAdapterRequestDevice(g_adapter, nullptr, dcb);
  for (int i = 0; i < 1000 && !g_device_done; ++i) {
    wgpuInstanceProcessEvents(instance);
  }
  if (!g_device) {
    std::printf("[readback] FAIL no device\n");
    return 3;
  }

  WGPUQueue queue = wgpuDeviceGetQueue(g_device);

  const uint32_t kW = 64;
  const uint32_t kH = 64;
  WGPUTextureDescriptor td = {};
  td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  td.dimension = WGPUTextureDimension_2D;
  td.size = {kW, kH, 1};
  td.format = WGPUTextureFormat_RGBA8Unorm;
  td.mipLevelCount = 1;
  td.sampleCount = 1;
  WGPUTexture tex = wgpuDeviceCreateTexture(g_device, &td);
  WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);

  const uint32_t kBytesPerRow = 256;  // kW*4 = 256, already 256-aligned
  WGPUBufferDescriptor bd = {};
  bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
  bd.size = static_cast<uint64_t>(kBytesPerRow) * kH;
  WGPUBuffer buf = wgpuDeviceCreateBuffer(g_device, &bd);

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_device, nullptr);
  WGPURenderPassColorAttachment ca = {};
  ca.view = view;
  ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  ca.loadOp = WGPULoadOp_Clear;
  ca.storeOp = WGPUStoreOp_Store;
  ca.clearValue = {0.25, 0.5, 0.75, 1.0};
  WGPURenderPassDescriptor rp = {};
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &ca;
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderEnd(pass);

  WGPUTexelCopyTextureInfo src = {};
  src.texture = tex;
  src.mipLevel = 0;
  src.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo dst = {};
  dst.buffer = buf;
  dst.layout.offset = 0;
  dst.layout.bytesPerRow = kBytesPerRow;
  dst.layout.rowsPerImage = kH;
  WGPUExtent3D ext = {kW, kH, 1};
  wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ext);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(queue, 1, &cmd);

  WGPUBufferMapCallbackInfo mcb = {};
  mcb.mode = WGPUCallbackMode_AllowProcessEvents;
  mcb.callback = OnMap;
  wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mcb);
  for (int i = 0; i < 1000 && !g_map_done; ++i) {
    wgpuDevicePoll(g_device, /*wait=*/true, nullptr);
  }
  if (!g_map_done) {
    std::printf("[readback] FAIL map timeout\n");
    return 4;
  }

  const uint8_t* data = static_cast<const uint8_t*>(
      wgpuBufferGetConstMappedRange(buf, 0, bd.size));
  if (!data) {
    std::printf("[readback] FAIL no mapped range\n");
    return 5;
  }
  std::printf("[readback] pixel0 = %u %u %u %u (expect ~64 128 191 255)\n",
              data[0], data[1], data[2], data[3]);
  const bool ok = data[0] >= 60 && data[0] <= 68 && data[1] >= 123 &&
                  data[1] <= 132 && data[2] >= 186 && data[2] <= 195 &&
                  data[3] == 255;
  wgpuBufferUnmap(buf);
  std::printf("[readback] %s\n", ok ? "OK" : "MISMATCH");
  return ok ? 0 : 6;
}
