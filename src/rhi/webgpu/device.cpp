// rhi/webgpu/device.cpp -- WebGPU backend.
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/device.hpp"
#include "rhi/init_config.hpp"
#include "rhi/swap_chain.hpp"

#include <cstdio>

#include <webgpu/webgpu.h>

#include "rhi/webgpu/native_compat.hpp"  // DrainGpu (wgpuDevicePoll wrapper)

namespace cairns::rhi {

namespace {
struct AdapterReq { WGPUAdapter adapter = nullptr; bool done = false; };
struct DeviceReq { WGPUDevice device = nullptr; bool done = false; };
void OnAdapter(WGPURequestAdapterStatus, WGPUAdapter a, WGPUStringView, void* u1, void*) {
    auto* r = static_cast<AdapterReq*>(u1); r->adapter = a; r->done = true;
}
void OnDevice(WGPURequestDeviceStatus, WGPUDevice d, WGPUStringView, void* u1, void*) {
    auto* r = static_cast<DeviceReq*>(u1); r->device = d; r->done = true;
}
}  // namespace

Device::~Device() { Deinit(); }

bool Device::Init(const InitConfig& cfg) {
    // Browser: the async adapter/device request cannot be busy-waited (callbacks
    // only fire when control returns to the JS event loop), so the web entry
    // acquires them up front and injects them here. Adopt + skip the request.
    if (cfg.plat.device) {
        plat.instance = cfg.plat.instance;
        plat.adapter = cfg.plat.adapter;
        plat.device = cfg.plat.device;
        plat.queue = cfg.plat.queue ? cfg.plat.queue
                                    : wgpuDeviceGetQueue(cfg.plat.device);
        plat.surface = cfg.plat.surface;
        plat.owns_handles = false;
        WGPULimits limits = {};
        if (wgpuDeviceGetLimits(plat.device, &limits) == WGPUStatus_Success) {
            const uint64_t sb = limits.maxStorageBufferBindingSize;
            const uint64_t ub = limits.maxUniformBufferBindingSize;
            caps.max_storage_buffer_range =
                sb > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(sb);
            caps.max_uniform_buffer_range =
                ub > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(ub);
            caps.max_storage_buffers_per_stage =
                limits.maxStorageBuffersPerShaderStage;
        } else {
            caps.max_storage_buffer_range = 128u * 1024u * 1024u;
            caps.max_uniform_buffer_range = 64u * 1024u;
            caps.max_storage_buffers_per_stage = 8u;
        }
        caps.resident_budget_bytes = 8ull * 1024 * 1024 * 1024;
        inited_ = true;
        return true;
    }

    plat.instance = wgpuCreateInstance(nullptr);
    if (!plat.instance) { return false; }

    AdapterReq areq;
    WGPURequestAdapterCallbackInfo acb = {};
    acb.mode = WGPUCallbackMode_AllowProcessEvents;
    acb.callback = OnAdapter;
    acb.userdata1 = &areq;
    wgpuInstanceRequestAdapter(plat.instance, nullptr, acb);
    for (int i = 0; i < 1000 && !areq.done; ++i) { wgpuInstanceProcessEvents(plat.instance); }
    plat.adapter = areq.adapter;
    if (!plat.adapter) { return false; }

    // WebGPU defaults storage-buffer binding to 128 MB + 8 storage buffers/stage;
    // the skin pool needs a 256 MB binding and we standardize on a 10-SSBO floor.
    // Request the adapter's full limits at device creation (the boot guard refuses
    // the device if the adapter can't meet kMin* in device_caps.hpp).
    WGPULimits adapter_limits = WGPU_LIMITS_INIT;
    wgpuAdapterGetLimits(plat.adapter, &adapter_limits);
    WGPUDeviceDescriptor dev_desc = {};
    dev_desc.requiredLimits = &adapter_limits;

    DeviceReq dreq;
    WGPURequestDeviceCallbackInfo dcb = {};
    dcb.mode = WGPUCallbackMode_AllowProcessEvents;
    dcb.callback = OnDevice;
    dcb.userdata1 = &dreq;
    wgpuAdapterRequestDevice(plat.adapter, &dev_desc, dcb);
    for (int i = 0; i < 1000 && !dreq.done; ++i) { wgpuInstanceProcessEvents(plat.instance); }
    plat.device = dreq.device;
    if (!plat.device) { return false; }
    plat.queue = wgpuDeviceGetQueue(plat.device);

    // Real device limits (clamped to uint32 for DeviceCaps; the skin-pool fit
    // check reads max_storage_buffer_range).
    WGPULimits limits = {};
    if (wgpuDeviceGetLimits(plat.device, &limits) == WGPUStatus_Success) {
        const uint64_t sb = limits.maxStorageBufferBindingSize;
        const uint64_t ub = limits.maxUniformBufferBindingSize;
        caps.max_storage_buffer_range =
            sb > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(sb);
        caps.max_uniform_buffer_range =
            ub > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(ub);
        caps.max_storage_buffers_per_stage =
            limits.maxStorageBuffersPerShaderStage;
    } else {
        caps.max_storage_buffer_range = 128u * 1024u * 1024u;
        caps.max_uniform_buffer_range = 64u * 1024u;
        caps.max_storage_buffers_per_stage = 8u;
    }
    caps.resident_budget_bytes = 8ull * 1024 * 1024 * 1024;

    inited_ = true;
    return true;
}

void Device::Deinit() {
    if (plat.owns_handles) {
        if (plat.queue) { wgpuQueueRelease(plat.queue); }
        if (plat.device) { wgpuDeviceRelease(plat.device); }
        if (plat.adapter) { wgpuAdapterRelease(plat.adapter); }
        if (plat.instance) { wgpuInstanceRelease(plat.instance); }
    }
    plat = DevicePlat{};
    inited_ = false;
}

bool Device::InitSwapChain(SwapChain& sc, const InitConfig& cfg) {
    (void)sc; (void)cfg;  // headless: render offscreen, no swapchain
    return true;
}

void Device::WaitIdle() {
    if (plat.device) { webgpu::DrainGpu(plat.device); }
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
