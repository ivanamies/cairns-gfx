// rhi/metal/internal/resources_impl.hpp
//
// Internal: Resources::Impl for Metal. Shared between metal/resources.cpp and
// metal/resource_manager.cpp (transitional). Not a public header.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <cstdint>

#include <Metal/Metal.hpp>

#include "rhi/resources.hpp"

namespace cairns::rhi {

struct Resources::Impl {
    MTL::Device* device = nullptr;  // mirrored from Device
    Allocator* alloc = nullptr;     // borrowed
    uint32_t frame_index = 1;       // drives deferred-free + bump retire
};

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
