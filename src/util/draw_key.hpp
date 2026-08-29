#pragma once

#include "util/draw.hpp"
#include "rhi/resource_impl.hpp"

#include <cstdint>

namespace cairns {

using DrawKey = uint32_t;

// the way I build draw keys is wrong.
// this is how to do it right: https://realtimecollisiondetection.net/blog/?p=86
DrawKey BuildDrawKey(const Draw& draw) {
    const rhi::Handle<rhi::BindGroup> material = draw.bind_groups[cairns::kMaterialBindSlot];
    return material.get_id();
}

} // namespace cairns
