// rhi/device.hpp
//
// Platform device lifetime — the leaf of the rhi DAG. Owns the instance /
// surface / physical+logical device / queues / command pool (Vulkan) or the
// MTL::Device + command queue (Metal). No public platform-handle accessors:
// cooperating rhi classes reach the handles via friendship + the per-backend
// internal Impl header (rhi/{metal,vulkan}/internal/device_impl.hpp).

#pragma once

#include "util/define.hpp"

struct SDL_Window;

namespace cairns::rhi {

class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] bool Init(SDL_Window* window);
    void Deinit();

private:
    friend class ResourceManager;
    friend class Allocator;
    friend class Resources;
    friend class Bindless;
    friend struct SwapChain;
    friend class Frames;
    friend class CommandRecorder;

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace cairns::rhi
