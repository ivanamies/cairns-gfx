// rhi/metal/internal/bindless_impl.hpp
//
// Internal: Bindless::Impl for Metal (argument-buffer encoder). Shared between
// metal/bindless.cpp and metal/resource_manager.cpp. Not a public header.

#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <cstdint>

#include <Metal/Metal.hpp>

#include "rhi/bindless.hpp"

namespace cairns::rhi {

struct Bindless::Impl {
    MTL::Device* device = nullptr;  // mirrored from Device
    Resources* res = nullptr;       // borrowed

    MTL::ArgumentEncoder* bindless_encoder = nullptr;
    uint32_t bindless_tex_base = 0;
    uint32_t bindless_attr_base = 0;
    uint32_t bindless_samp_base = 0;
    uint32_t bindless_num_tex = 0;
    uint32_t bindless_num_attr = 0;
    uint32_t bindless_num_samp = 0;
};

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
