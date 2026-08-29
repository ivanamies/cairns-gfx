// tests/test_rhi_backends.cpp
//
// SPEC: backend boot-cap fill contract + offset×16 binding.
// TAGS: [spec][rhi][backend]
//
// These are spec-tier even though they're about backend behavior: the
// arithmetic each backend must perform on its DeviceCaps + the byte-offset
// shape the engine binds at can be specced as pure-CPU functions.
// The "Init must populate caps" contract test for live backends moves to
// cairns_golden_tests (where a real Device exists); here we pin the math
// the backends share.

#include <catch2/catch_test_macros.hpp>

#include "util/device_caps.hpp"

using namespace cairns;

SCENARIO("offset*16 byte-offset arithmetic is bit-stable",
         "[spec][rhi][backend][regression]") {
    // Every backend's BindStorageBuffer for the skin pool
    // must bind at slice.offset * kSkinVertexStride. This pins the
    // arithmetic side; cairns_golden_tests runs the contract against the
    // live Device + bake.
    REQUIRE(kSkinVertexStride == 16u);
    REQUIRE(SkinSliceByteOffset(0u) == 0u);
    REQUIRE(SkinSliceByteOffset(1u) == 16u);
    REQUIRE(SkinSliceByteOffset(2u) == 32u);
    REQUIRE(SkinSliceByteOffset(1024u * 1024u) == 16u * 1024u * 1024u);
}

SCENARIO("DeviceCaps fields default to zero",
         "[spec][rhi][backend][regression]") {
    // Boot-cap fill contract. Each backend's Device::Init
    // is required to populate max_storage_buffer_range and
    // resident_budget_bytes. Default-zero is the "uninitialized" state
    // the engine's boot invariant catches.
    DeviceCaps caps{};
    REQUIRE(caps.max_storage_buffer_range == 0u);
    REQUIRE(caps.max_uniform_buffer_range == 0u);
    REQUIRE(caps.resident_budget_bytes == 0u);
}

SCENARIO("boundary <= check on SkinPoolFitsDevice",
         "[spec][rhi][backend][regression]") {
    // The relation is <=, not <. A pool sized exactly at the device's
    // reported range fits.
    DeviceCaps caps{};
    caps.max_storage_buffer_range = 256u * 1024u * 1024u;
    REQUIRE(SkinPoolFitsDevice(256u * 1024u * 1024u, caps));
    REQUIRE_FALSE(SkinPoolFitsDevice(256u * 1024u * 1024u + 1u, caps));
}
