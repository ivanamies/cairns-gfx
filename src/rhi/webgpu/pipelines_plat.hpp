// rhi/webgpu/pipelines_plat.hpp
#pragma once

#include <webgpu/webgpu.h>

namespace cairns::rhi {

struct PipelinesPlat {
    WGPUDevice device_ = nullptr;  // mirrored from Device
};

}  // namespace cairns::rhi
