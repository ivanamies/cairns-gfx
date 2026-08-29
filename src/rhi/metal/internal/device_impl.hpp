// rhi/metal/internal/device_impl.hpp
//
// Internal: defines Device::Impl for the Metal backend. Shared between
// metal/device.cpp (creates/destroys) and metal/resource_manager.cpp (mirrors
// the handle values into its own Impl during InitDevice). Not a public header.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <Metal/Metal.hpp>

#include "rhi/device.hpp"

namespace cairns::rhi {

struct Device::Impl {
    MTL::Device* device = nullptr;
    MTL::CommandQueue* queue = nullptr;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
