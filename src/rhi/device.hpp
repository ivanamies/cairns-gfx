// rhi/device.hpp
//
// Platform device lifetime — the leaf of the rhi DAG. Owns the instance /
// surface / physical+logical device / queues / command pool (Vulkan) or the
// MTL::Device + command queue (Metal). Backend handles are PUBLIC members
// (SwapChain-style, no pimpl); the GIANT-CAPS banner below is the access
// contract that replaces the old friend list.

#pragma once

#include "util/define.hpp"
#include "util/device_caps.hpp"

#if CAIRNS_METAL
#include "rhi/metal/device_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/device_plat.hpp"
#elif CAIRNS_WEBGPU
#include "rhi/webgpu/device_plat.hpp"
#endif

namespace cairns::rhi {

struct InitConfig;
struct SwapChain;

class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // CALLER: ENGINE.
    [[nodiscard]] bool Init(const InitConfig& cfg);
    // CALLER: ENGINE.
    void Deinit();
    // CALLER: ENGINE.
    [[nodiscard]] bool InitSwapChain(SwapChain& sc, const InitConfig& cfg);

    // Block until the GPU has drained all in-flight work. Metal no-op
    // (drawable resize is synchronous); vk calls vkDeviceWaitIdle. Used by
    // the engine's window-resize path.
    void WaitIdle();

    // Platform handles live in plat; the subsystems mirror these into their
    // own plat during their Init().
    DevicePlat plat;

    // Filled by Init() right after the backing device is up. Pure POD;
    // engine consumes via cairns::SkinPoolFitsDevice / FitsResidentBudget.
    // The 2026-06-17 S22 garble (Adreno 730 maxStorageBufferRange = 256 MB)
    // is caught at boot by reading max_storage_buffer_range from this.
    cairns::DeviceCaps caps{};

private:
    bool inited_ = false;
};

}  // namespace cairns::rhi
