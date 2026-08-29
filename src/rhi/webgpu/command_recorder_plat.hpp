// rhi/webgpu/command_recorder_plat.hpp
#pragma once

#include <vector>

#include <webgpu/webgpu.h>

namespace cairns::rhi {

struct CommandRecorderPlat {
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUCommandEncoder cmd_ = nullptr;
    WGPURenderPassEncoder enc_ = nullptr;
    WGPUComputePassEncoder comp_ = nullptr;
    // Per-draw bind groups (DrawFullscreen). The command buffer references them
    // until submit, so Frames::EndSubmit releases them after the device poll.
    std::vector<WGPUBindGroup> transient_bind_groups_;
};

}  // namespace cairns::rhi
