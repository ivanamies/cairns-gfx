#pragma once

#include "util/draw.hpp"

#include <cstdint>

namespace cairns {

using DrawKey = uint32_t;

// the way I build draw keys is wrong.
// this is how to do it right: https://realtimecollisiondetection.net/blog/?p=86
DrawKey BuildDrawKey(const Draw& draw) {
    return draw.bind_groups[cairns::kMaterialBindSlot - 1];
}

} // namespace cairns
