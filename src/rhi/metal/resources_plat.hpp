// rhi/metal/resources_plat.hpp

#pragma once

#include <cstdint>

namespace MTL {
class Device;
class CommandQueue;
}

namespace cairns::rhi {

struct ResourcesPlat {
    MTL::Device* device_ = nullptr;             // mirrored from Device
    MTL::CommandQueue* queue_ = nullptr;        // mirrored from Device
    uint32_t frame_index_ = 1;                  // drives deferred-free + bump retire
};

}  // namespace cairns::rhi
