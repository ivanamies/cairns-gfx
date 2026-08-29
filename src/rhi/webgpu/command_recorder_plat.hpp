// rhi/webgpu/command_recorder_plat.hpp
#pragma once

#include <webgpu/webgpu.h>

namespace cairns::rhi {

struct CommandRecorderPlat {
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUCommandEncoder cmd_ = nullptr;
    WGPURenderPassEncoder enc_ = nullptr;
    WGPUComputePassEncoder comp_ = nullptr;
};

}  // namespace cairns::rhi
