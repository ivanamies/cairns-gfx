// rhi/device.hpp
//
// Platform device lifetime — the leaf of the rhi DAG. Owns the instance /
// surface / physical+logical device / queues / command pool (Vulkan) or the
// MTL::Device + command queue (Metal). Backend handles are PUBLIC members
// (SwapChain-style, no pimpl); the GIANT-CAPS banner below is the access
// contract that replaces the old friend list.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL
#include "rhi/metal/device_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/device_plat.hpp"
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

private:
    bool inited_ = false;
};

}  // namespace cairns::rhi
