// src/util/texture_mips.hpp -- pure CPU mip-chain math (no SDL/rhi deps, so
// the Tier-S specs drive it directly).
//  - The blob layout (all levels concatenated, level 0 first) is exactly the
//    multi-level initial_data the WebGPU backend consumes; metal/vk share the
//    same size-walk.
//  - Integer box filter (a+b+c+d+2)>>2: bit-exact across platforms.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace cairns {

inline uint32_t FullMipLevels(uint32_t w, uint32_t h) {
    uint32_t levels = 1;
    while (w > 1 || h > 1) {
        w = w > 1 ? w >> 1 : 1u;
        h = h > 1 ? h >> 1 : 1u;
        ++levels;
    }
    return levels;
}

// All levels concatenated (level 0 first). Integer box filter
// (a+b+c+d+2)>>2 per channel: bit-exact across platforms, so refs that
// sample these mips never diverge by filtering.
inline std::vector<uint8_t> BuildMipChainRgba8(const uint8_t* base,
                                               uint32_t width,
                                               uint32_t height) {
    std::vector<uint8_t> blob;
    const uint32_t levels = FullMipLevels(width, height);
    size_t total = 0;
    {
        uint32_t w = width;
        uint32_t h = height;
        for (uint32_t l = 0; l < levels; ++l) {
            total += static_cast<size_t>(w) * h * 4;
            w = w > 1 ? w >> 1 : 1u;
            h = h > 1 ? h >> 1 : 1u;
        }
    }
    blob.resize(total);
    std::memcpy(blob.data(), base, static_cast<size_t>(width) * height * 4);
    size_t prev_off = 0;
    uint32_t pw = width;
    uint32_t ph = height;
    size_t off = static_cast<size_t>(width) * height * 4;
    for (uint32_t l = 1; l < levels; ++l) {
        const uint32_t lw = pw > 1 ? pw >> 1 : 1u;
        const uint32_t lh = ph > 1 ? ph >> 1 : 1u;
        const uint8_t* src = blob.data() + prev_off;
        uint8_t* dst = blob.data() + off;
        for (uint32_t y = 0; y < lh; ++y) {
            // Clamp the second tap on odd/1-wide parents.
            const uint32_t y0 = y * 2;
            const uint32_t y1 = (y0 + 1 < ph) ? y0 + 1 : y0;
            for (uint32_t x = 0; x < lw; ++x) {
                const uint32_t x0 = x * 2;
                const uint32_t x1 = (x0 + 1 < pw) ? x0 + 1 : x0;
                for (uint32_t ch = 0; ch < 4; ++ch) {
                    const uint32_t a = src[(y0 * pw + x0) * 4 + ch];
                    const uint32_t b = src[(y0 * pw + x1) * 4 + ch];
                    const uint32_t cc = src[(y1 * pw + x0) * 4 + ch];
                    const uint32_t dd = src[(y1 * pw + x1) * 4 + ch];
                    dst[(y * lw + x) * 4 + ch] =
                        static_cast<uint8_t>((a + b + cc + dd + 2u) >> 2u);
                }
            }
        }
        prev_off = off;
        off += static_cast<size_t>(lw) * lh * 4;
        pw = lw;
        ph = lh;
    }
    return blob;
}

}  // namespace cairns
