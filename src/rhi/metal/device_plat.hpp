// rhi/metal/device_plat.hpp

#pragma once

#include <Metal/Metal.hpp>

namespace cairns::rhi {

struct DevicePlat {
    MTL::Device* device_ = nullptr;
    MTL::CommandQueue* queue_ = nullptr;
};

}  // namespace cairns::rhi
