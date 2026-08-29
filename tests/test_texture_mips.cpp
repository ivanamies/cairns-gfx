// tests/test_texture_mips.cpp
//
// SPEC: cairns::FullMipLevels / BuildMipChainRgba8 (src/util/texture_mips.hpp)
// TAGS: [spec][texture][mips]
// The blob layout (levels concatenated, level 0 first) is the multi-level
// initial_data contract shared by all three backends.

#include <catch2/catch_test_macros.hpp>

#include "util/texture_mips.hpp"

using cairns::BuildMipChainRgba8;
using cairns::FullMipLevels;

SCENARIO("full mip level counts", "[spec][texture][mips]") {
    REQUIRE(FullMipLevels(1, 1) == 1u);
    REQUIRE(FullMipLevels(2, 2) == 2u);
    REQUIRE(FullMipLevels(256, 256) == 9u);
    REQUIRE(FullMipLevels(512, 256) == 10u);  // non-square: max dim rules
}

SCENARIO("mip blob is levels concatenated, level 0 first",
         "[spec][texture][mips]") {
    // 4x4 solid mid-gray: every level must stay exactly mid-gray (the box
    // filter of a constant is that constant).
    std::vector<uint8_t> base(4 * 4 * 4, 128);
    const std::vector<uint8_t> blob = BuildMipChainRgba8(base.data(), 4, 4);
    // 4x4 + 2x2 + 1x1 texels, 4 bytes each.
    REQUIRE(blob.size() == (16u + 4u + 1u) * 4u);
    for (uint8_t b : blob) {
        REQUIRE(b == 128);
    }
}

SCENARIO("the box filter rounds to nearest, deterministically",
         "[spec][texture][mips]") {
    // 2x2 -> 1x1: (0 + 1 + 2 + 3 + 2) >> 2 == 2 per channel.
    std::vector<uint8_t> base = {
        0,   0,   0,   0,    //
        1,   1,   1,   1,    //
        2,   2,   2,   2,    //
        3,   3,   3,   3,    //
    };
    const std::vector<uint8_t> blob = BuildMipChainRgba8(base.data(), 2, 2);
    REQUIRE(blob.size() == (4u + 1u) * 4u);
    for (uint32_t ch = 0; ch < 4; ++ch) {
        REQUIRE(blob[16 + ch] == 2);
    }
}

SCENARIO("non-square chains clamp the short axis at 1",
         "[spec][texture][mips]") {
    // 4x1: levels are 4x1, 2x1, 1x1.
    std::vector<uint8_t> base(4 * 1 * 4, 200);
    const std::vector<uint8_t> blob = BuildMipChainRgba8(base.data(), 4, 1);
    REQUIRE(blob.size() == (4u + 2u + 1u) * 4u);
    for (uint8_t b : blob) {
        REQUIRE(b == 200);
    }
}
