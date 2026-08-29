// tests/test_top_risks.cpp
//
// SPEC: cross-subsystem top-risk consolidation.
// TAGS: [spec][risks][regression]
//
// (1) render_graph_schedule.hpp::SchedulePasses is a sibling of
// RenderGraph::Bake -- a divergence is invisible to either's tests alone.
// We can spec the schedule function in isolation (it IS the sibling we
// already exercise); the divergence detection lives in the golden target.
//
// (2) kFramesInFlight consistency: spec target asserts the constant; the
// golden target asserts both backends agree.
//
// (3) EntityRef anti-singleton: grep CI check (not a runtime test).
//
// (4) Asset-gated SKIPs hiding test gaps: REQUIRED.txt fail-on-missing.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "render/render_graph_schedule.hpp"
#include "rhi/resource_manager.hpp"
#include "util/device_caps.hpp"

using namespace cairns;

SCENARIO("kFramesInFlight is 2 (spec mirror)",
         "[spec][risks][regression]") {
    REQUIRE(cairns::rhi::kFramesInFlight == 2u);
}

SCENARIO("SchedulePasses behaves deterministically on the canonical "
         "single-resource chain",
         "[spec][risks][schedule][regression]") {
    // Schedule must match Bake. Spec the deterministic
    // output for a known 3-pass chain; cairns_golden_tests is responsible
    // for verifying RenderGraph::Bake produces the same order.
    std::vector<SchedPass> passes;
    passes.push_back({{}, {1}});
    passes.push_back({{1}, {2}});
    passes.push_back({{2}, {3}});
    const auto r = SchedulePasses(passes, /*output=*/3);
    REQUIRE_FALSE(r.cycle);
    REQUIRE(r.order.size() == 3u);
    REQUIRE(r.order[0] == 0u);
    REQUIRE(r.order[1] == 1u);
    REQUIRE(r.order[2] == 2u);
}

SCENARIO("the regression-class arithmetic invariants are all live",
         "[spec][risks][regression]") {
    // Numeric invariants that don't belong to any single subsystem,
    // kept in one grep-able, review-able place.
    REQUIRE(kSkinVertexStride == 16u);
    REQUIRE(cairns::rhi::kFramesInFlight == 2u);

    // S22 garble boundary at 256 MB pool / 256 MB device range.
    DeviceCaps adreno{};
    adreno.max_storage_buffer_range = 256u * 1024u * 1024u;
    REQUIRE(SkinPoolFitsDevice(256u * 1024u * 1024u, adreno));
    REQUIRE_FALSE(SkinPoolFitsDevice(257u * 1024u * 1024u, adreno));
}
