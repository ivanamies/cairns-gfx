// tests/test_rhi_interfaces.cpp
//
// SPEC: src/rhi/resource_manager.hpp -- cross-backend descriptors +
//        kFramesInFlight invariant (G.3 of the test tech tree).
// TAGS: [spec][rhi]
//
// The audit named "kFramesInFlight hardcoded to 3 instead of 2" as a real
// risk: arithmetic test passes with wrong FIF input, OOMs on-device. This
// spec pins kFramesInFlight == 2 and the BufferUsage bit positions so a
// drift on either side gets caught at the spec level.

#include <catch2/catch_test_macros.hpp>

#include "rhi/resource_manager.hpp"

using namespace cairns::rhi;

SCENARIO("kFramesInFlight is exactly 2",
         "[spec][rhi][fif][regression]") {
    // Phase G.9 #84: pin to 2 at the spec level. The audit named the risk
    // of a back-slide to 3 (the value that crossed the S22 lmkd budget).
    REQUIRE(kFramesInFlight == 2u);
}

SCENARIO("BufferUsage flag bit positions are pinned",
         "[spec][rhi][layout][regression]") {
    REQUIRE(kUsageNone == 0u);
    REQUIRE(kUsageVertex == (1u << 0));
    REQUIRE(kUsageIndex == (1u << 1));
    REQUIRE(kUsageUniform == (1u << 2));
    REQUIRE(kUsageStorage == (1u << 3));
    REQUIRE(kUsageIndirect == (1u << 4));
    REQUIRE(kUsageTransferSrc == (1u << 5));
    REQUIRE(kUsageTransferDst == (1u << 6));
}

SCENARIO("BufferUsage flags compose by OR",
         "[spec][rhi][layout]") {
    const BufferUsage combo = kUsageStorage | kUsageVertex;
    REQUIRE((combo & kUsageStorage) != 0u);
    REQUIRE((combo & kUsageVertex) != 0u);
    REQUIRE((combo & kUsageIndex) == 0u);
}

SCENARIO("BufferDesc defaults", "[spec][rhi][descriptors]") {
    BufferDesc bd{};
    // The default usage and memory have to remain stable; ResourceManager
    // tests construct BufferDescs by mutating fields.
    REQUIRE(bd.usage == kUsageUniform);
    REQUIRE(bd.memory == Memory::kDefault);
}

SCENARIO("TextureDesc defaults", "[spec][rhi][descriptors]") {
    TextureDesc td{};
    REQUIRE(td.memory == Memory::kDefault);
}
