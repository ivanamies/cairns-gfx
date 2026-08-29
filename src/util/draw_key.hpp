#pragma once

#include <cstdint>

namespace cairns {

using DrawKey = uint64_t;

// https://realtimecollisiondetection.net/blog/?p=86
inline DrawKey BuildDrawKey(uint32_t material_id, uint32_t depth,
                            uint32_t translucency = 0, uint32_t viewport = 0,
                            uint32_t viewport_layer = 0, uint32_t fullscreen_layer = 0) {
    return (static_cast<uint64_t>(fullscreen_layer & 0x3u) << 62)
         | (static_cast<uint64_t>(viewport & 0x7u)         << 59)
         | (static_cast<uint64_t>(viewport_layer & 0x7u)   << 56)
         | (static_cast<uint64_t>(translucency & 0x3u)     << 54)
         | (static_cast<uint64_t>(depth & 0xFFFFFFu)       << 30)
         |  static_cast<uint64_t>(material_id & 0x3FFFFFFFu);
}

} // namespace cairns
