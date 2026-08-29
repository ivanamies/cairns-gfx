#pragma once

#include <cstdint>
#include <limits>

namespace cairns {

struct Size {
    Size(uint32_t width, uint32_t height) : width(width), height(height) { }

    uint32_t width;
    uint32_t height;

};

static const Size kInvalidSize(std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max());

} // namespace cairns
