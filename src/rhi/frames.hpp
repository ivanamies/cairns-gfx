// rhi/frames.hpp
//
// Fork C frame lifecycle: owns per-frame sync + command buffers + the per-frame
// descriptor sets (Vulkan) / render-pass + MSAA/depth targets (Metal) + the
// one-shot swapchain dump. Begin() returns a FrameContext whose CommandRecorder
// records into the frame; End() submits + presents. Depends on Device + Resources.

#pragma once

#include "util/define.hpp"

#include <cstdint>
#include <filesystem>

#include "rhi/command_recorder.hpp"  // FrameContext

namespace cairns::rhi {

class Device;
class Resources;
struct SwapChain;

class Frames {
public:
    Frames() = default;
    ~Frames();
    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    [[nodiscard]] bool Init(Device& device, Resources& resources);
    void Deinit();

    // Metal: create MSAA/depth render targets + render-pass descriptor (called
    // after scene textures load). Vulkan: no-op (targets created in SwapChain).
    [[nodiscard]] bool InitTargets(SwapChain& sc);

    FrameContext Begin(SwapChain& sc);
    void End(FrameContext& fc);

    // Request a one-shot swapchain dump on the next End(); cleared after writing.
    void SetDumpPath(const std::filesystem::path& path);

private:
    friend class ResourceManager;  // Vulkan pipeline layout reads the set layouts

    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace cairns::rhi
