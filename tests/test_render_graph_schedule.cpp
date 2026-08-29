// tests/test_render_graph_schedule.cpp
//
// SPEC: cairns::SchedulePasses (src/render/render_graph_schedule.hpp)
// TAGS: [spec][render_graph][schedule]   implementation-order: render graph (6)
// BUILDING BLOCK OF: Tier G scenarios 3 & 4 (side-by-side; nested depth+camera).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "render/render_graph_schedule.hpp"

using cairns::SchedPass;
using cairns::SchedulePasses;

namespace {
long PosOf(const std::vector<uint32_t>& order, uint32_t pass) {
    auto it = std::find(order.begin(), order.end(), pass);
    return it == order.end() ? -1 : (it - order.begin());
}
}  // namespace

SCENARIO("writers precede readers", "[spec][schedule]") {
    // 0 writes depth(1); 1 reads depth, writes resolve(2); 2 reads resolve,
    // writes output(3). Output = 3.
    const std::vector<SchedPass> passes = {
        {{}, {1}}, {{1}, {2}}, {{2}, {3}},
    };
    const auto r = SchedulePasses(passes, 3);
    REQUIRE_FALSE(r.cycle);
    REQUIRE(r.order.size() == 3);
    REQUIRE(PosOf(r.order, 0) < PosOf(r.order, 1));
    REQUIRE(PosOf(r.order, 1) < PosOf(r.order, 2));
}

SCENARIO("a pass that does not reach the output is pruned",
         "[spec][schedule][regression]") {
    const std::vector<SchedPass> passes = {
        {{}, {1}},      // 0 -> output chain
        {{1}, {3}},     // 1 writes output(3)
        {{}, {9}},      // 2 writes a resource nobody reads => dead
    };
    const auto r = SchedulePasses(passes, 3);
    REQUIRE(PosOf(r.order, 2) == -1);
    REQUIRE(r.order.size() == 2);
}

SCENARIO("the nested depth + third-camera passes both survive (scenario 4)",
         "[spec][schedule]") {
    // 0: main color of 100 actors -> color(1)
    // 1: depth prepass -> depth(2)
    // 2: third camera -> color3(3)
    // 3: composite reads color(1), depth(2), color3(3) -> output(4)
    const std::vector<SchedPass> passes = {
        {{}, {1}}, {{}, {2}}, {{}, {3}}, {{1, 2, 3}, {4}},
    };
    const auto r = SchedulePasses(passes, 4);
    REQUIRE(r.order.size() == 4);
    REQUIRE(PosOf(r.order, 3) == 3);
    REQUIRE(PosOf(r.order, 0) < PosOf(r.order, 3));
    REQUIRE(PosOf(r.order, 1) < PosOf(r.order, 3));
    REQUIRE(PosOf(r.order, 2) < PosOf(r.order, 3));
}

SCENARIO("an imported-then-read buffer keeps its writer (engine prune note)",
         "[spec][schedule]") {
    const std::vector<SchedPass> passes = {
        {{}, {7}},        // 0: particle_sim compute
        {{7}, {8}},       // 1: particle draw -> output
    };
    const auto r = SchedulePasses(passes, 8);
    REQUIRE(r.order.size() == 2);
    REQUIRE(PosOf(r.order, 0) < PosOf(r.order, 1));
}

SCENARIO("ties break by submission order, deterministically",
         "[spec][schedule]") {
    const std::vector<SchedPass> passes = {
        {{}, {1}}, {{}, {2}}, {{1, 2}, {3}},
    };
    const auto r = SchedulePasses(passes, 3);
    REQUIRE(PosOf(r.order, 0) < PosOf(r.order, 1));
}

SCENARIO("a read/write cycle is reported", "[spec][schedule]") {
    const std::vector<SchedPass> passes = {
        {{20}, {10}}, {{10}, {20}},
    };
    const auto r = SchedulePasses(passes, 10);
    REQUIRE(r.cycle);
}

// ---- Granite invalidate/flush model (rhi/barrier.hpp AccessResource) -------
// The per-resource barrier step render_graph::Execute drives; specs drive the
// pure function over a PipelineEvent directly. These pin the CURRENT model:
// RAW + WAW + layout transitions. (WAR is unmodeled today -- its spec lands
// with the fake-flush port.)

#include "rhi/barrier.hpp"

using cairns::rhi::AccessResource;
using cairns::rhi::BarrierEmit;
using cairns::rhi::BarrierLayout;
using cairns::rhi::PipelineEvent;

SCENARIO("first use transitions from undefined", "[spec][graph_barrier]") {
    PipelineEvent pe{};
    BarrierEmit e{};
    const bool need = AccessResource(pe, cairns::rhi::kAccessShaderRead,
                                     cairns::rhi::kPipeFragment,
                                     BarrierLayout::kShaderRead,
                                     /*is_write=*/false, &e);
    REQUIRE(need);
    REQUIRE(e.old_layout == BarrierLayout::kUndefined);
    REQUIRE(e.new_layout == BarrierLayout::kShaderRead);
    REQUIRE(e.src_access == cairns::rhi::kAccessNone);
    // No known producer: the conservative all-commands fallback.
    REQUIRE(e.src_stage == cairns::rhi::kPipeAllCommands);
}

SCENARIO("RAW: a read after a write waits on the pending flush",
         "[spec][graph_barrier]") {
    PipelineEvent pe{};
    BarrierEmit e{};
    AccessResource(pe, cairns::rhi::kAccessColorWrite,
                   cairns::rhi::kPipeColorOutput,
                   BarrierLayout::kColorAttachment, /*is_write=*/true, &e);
    const bool need = AccessResource(pe, cairns::rhi::kAccessShaderRead,
                                     cairns::rhi::kPipeFragment,
                                     BarrierLayout::kShaderRead,
                                     /*is_write=*/false, &e);
    REQUIRE(need);
    REQUIRE(e.src_access == cairns::rhi::kAccessColorWrite);
    REQUIRE(e.src_stage == cairns::rhi::kPipeColorOutput);
    REQUIRE(e.dst_access == cairns::rhi::kAccessShaderRead);
    // The read consumed the flush.
    REQUIRE(pe.to_flush_access == cairns::rhi::kAccessNone);
}

SCENARIO("WAW: back-to-back writes emit even with no layout change",
         "[spec][graph_barrier]") {
    PipelineEvent pe{};
    BarrierEmit e{};
    AccessResource(pe, cairns::rhi::kAccessColorWrite,
                   cairns::rhi::kPipeColorOutput,
                   BarrierLayout::kColorAttachment, /*is_write=*/true, &e);
    const bool need = AccessResource(pe, cairns::rhi::kAccessColorWrite,
                                     cairns::rhi::kPipeColorOutput,
                                     BarrierLayout::kColorAttachment,
                                     /*is_write=*/true, &e);
    REQUIRE(need);
    REQUIRE(e.src_access == cairns::rhi::kAccessColorWrite);
    REQUIRE(e.old_layout == BarrierLayout::kColorAttachment);
    REQUIRE(pe.to_flush_access == cairns::rhi::kAccessColorWrite);
}

SCENARIO("a second same-layout read after the flush is consumed is free",
         "[spec][graph_barrier]") {
    PipelineEvent pe{};
    BarrierEmit e{};
    AccessResource(pe, cairns::rhi::kAccessColorWrite,
                   cairns::rhi::kPipeColorOutput,
                   BarrierLayout::kColorAttachment, /*is_write=*/true, &e);
    AccessResource(pe, cairns::rhi::kAccessShaderRead,
                   cairns::rhi::kPipeFragment, BarrierLayout::kShaderRead,
                   /*is_write=*/false, &e);
    const bool need = AccessResource(pe, cairns::rhi::kAccessShaderRead,
                                     cairns::rhi::kPipeFragment,
                                     BarrierLayout::kShaderRead,
                                     /*is_write=*/false, &e);
    REQUIRE_FALSE(need);
}

SCENARIO("WAR: a same-layout write after reads is execution-only",
         "[spec][graph_barrier]") {
    // Storage-image shape: write kGeneral, read kGeneral, write kGeneral.
    PipelineEvent pe{};
    BarrierEmit e{};
    AccessResource(pe, cairns::rhi::kAccessShaderWrite,
                   cairns::rhi::kPipeCompute, BarrierLayout::kGeneral,
                   /*is_write=*/true, &e);
    AccessResource(pe, cairns::rhi::kAccessShaderRead,
                   cairns::rhi::kPipeFragment, BarrierLayout::kGeneral,
                   /*is_write=*/false, &e);
    const bool need = AccessResource(pe, cairns::rhi::kAccessShaderWrite,
                                     cairns::rhi::kPipeCompute,
                                     BarrierLayout::kGeneral,
                                     /*is_write=*/true, &e);
    REQUIRE(need);
    // Readers flushed nothing: execution dependency only.
    REQUIRE(e.src_access == cairns::rhi::kAccessNone);
    REQUIRE((e.src_stage & cairns::rhi::kPipeFragment) != 0);
}

SCENARIO("WAR: readers accumulate and the write resets them",
         "[spec][graph_barrier]") {
    PipelineEvent pe{};
    BarrierEmit e{};
    AccessResource(pe, cairns::rhi::kAccessShaderWrite,
                   cairns::rhi::kPipeCompute, BarrierLayout::kGeneral,
                   /*is_write=*/true, &e);
    AccessResource(pe, cairns::rhi::kAccessShaderRead,
                   cairns::rhi::kPipeFragment, BarrierLayout::kGeneral,
                   /*is_write=*/false, &e);
    AccessResource(pe, cairns::rhi::kAccessShaderRead,
                   cairns::rhi::kPipeVertex, BarrierLayout::kGeneral,
                   /*is_write=*/false, &e);
    AccessResource(pe, cairns::rhi::kAccessShaderWrite,
                   cairns::rhi::kPipeCompute, BarrierLayout::kGeneral,
                   /*is_write=*/true, &e);
    REQUIRE((e.src_stage & cairns::rhi::kPipeFragment) != 0);
    REQUIRE((e.src_stage & cairns::rhi::kPipeVertex) != 0);
    // The write replaced the accumulation with its own stage.
    REQUIRE(pe.src_stages == cairns::rhi::kPipeCompute);
}

SCENARIO("buffers barrier on flush and WAR but never on layout",
         "[spec][graph_barrier]") {
    // The buffer arm pins layout to kUndefined: first use emits nothing,
    // RAW emits the flush, a later write emits the reader-only barrier.
    PipelineEvent pe{};
    BarrierEmit e{};
    const bool first = AccessResource(pe, cairns::rhi::kAccessShaderRead,
                                      cairns::rhi::kPipeCompute,
                                      BarrierLayout::kUndefined,
                                      /*is_write=*/false, &e);
    REQUIRE_FALSE(first);
    AccessResource(pe, cairns::rhi::kAccessShaderWrite,
                   cairns::rhi::kPipeCompute, BarrierLayout::kUndefined,
                   /*is_write=*/true, &e);
    const bool raw = AccessResource(pe, cairns::rhi::kAccessShaderRead,
                                    cairns::rhi::kPipeVertex,
                                    BarrierLayout::kUndefined,
                                    /*is_write=*/false, &e);
    REQUIRE(raw);
    REQUIRE(e.src_access == cairns::rhi::kAccessShaderWrite);
    REQUIRE(e.dst_stage == cairns::rhi::kPipeVertex);
    const bool war = AccessResource(pe, cairns::rhi::kAccessShaderWrite,
                                    cairns::rhi::kPipeCompute,
                                    BarrierLayout::kUndefined,
                                    /*is_write=*/true, &e);
    REQUIRE(war);
    REQUIRE(e.src_access == cairns::rhi::kAccessNone);
    REQUIRE((e.src_stage & cairns::rhi::kPipeVertex) != 0);
}

SCENARIO("the anim->skin->forward chain emits exactly its three edges",
         "[spec][graph_barrier]") {
    // palette: anim writes, skin reads (RAW). pool: skin writes, forward
    // vertex-reads (RAW). Next frame, anim's write waits skin's read (WAR).
    PipelineEvent palette{};
    PipelineEvent pool{};
    BarrierEmit e{};
    AccessResource(palette, cairns::rhi::kAccessShaderWrite,
                   cairns::rhi::kPipeCompute, BarrierLayout::kUndefined,
                   true, &e);
    REQUIRE(AccessResource(palette, cairns::rhi::kAccessShaderRead,
                           cairns::rhi::kPipeCompute,
                           BarrierLayout::kUndefined, false, &e));
    REQUIRE(e.src_access == cairns::rhi::kAccessShaderWrite);
    AccessResource(pool, cairns::rhi::kAccessShaderWrite,
                   cairns::rhi::kPipeCompute, BarrierLayout::kUndefined,
                   true, &e);
    REQUIRE(AccessResource(pool, cairns::rhi::kAccessShaderRead,
                           cairns::rhi::kPipeVertex | cairns::rhi::kPipeFragment,
                           BarrierLayout::kUndefined, false, &e));
    // Frame N+1: anim writes palette again -- waits frame N's skin read.
    REQUIRE(AccessResource(palette, cairns::rhi::kAccessShaderWrite,
                           cairns::rhi::kPipeCompute,
                           BarrierLayout::kUndefined, true, &e));
    REQUIRE(e.src_access == cairns::rhi::kAccessNone);
    REQUIRE((e.src_stage & cairns::rhi::kPipeCompute) != 0);
}

SCENARIO("alias_transfer: the reusing lifetime discards, never preserves",
         "[spec][graph_barrier]") {
    // Lifetime A writes + is read; the slot is then reused by lifetime B.
    // B's first write must transition from UNDEFINED (discard) while still
    // waiting on A's accesses (execution deps chain across the alias).
    PipelineEvent pe{};
    BarrierEmit e{};
    AccessResource(pe, cairns::rhi::kAccessColorWrite,
                   cairns::rhi::kPipeColorOutput,
                   BarrierLayout::kColorAttachment, /*is_write=*/true, &e);
    AccessResource(pe, cairns::rhi::kAccessShaderRead,
                   cairns::rhi::kPipeFragment, BarrierLayout::kShaderRead,
                   /*is_write=*/false, &e);
    cairns::rhi::AliasReset(pe);
    const bool need = AccessResource(pe, cairns::rhi::kAccessColorWrite,
                                     cairns::rhi::kPipeColorOutput,
                                     BarrierLayout::kColorAttachment,
                                     /*is_write=*/true, &e);
    REQUIRE(need);
    REQUIRE(e.old_layout == BarrierLayout::kUndefined);
    REQUIRE(e.new_layout == BarrierLayout::kColorAttachment);
    // A's reader/writer stages survive the reset.
    REQUIRE((e.src_stage & cairns::rhi::kPipeFragment) != 0);
    REQUIRE((e.src_stage & cairns::rhi::kPipeColorOutput) != 0);
}
