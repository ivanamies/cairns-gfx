// tests/test_state_hash.cpp
//
// #229 P7 -- determinism tests for the per-frame SIM state hash
// (Engine::LastSimHash: FNV-1a over the block-backed frame arena [0,Used) plus
// the sim drivers). The digest advances every frame BY DESIGN (sim_frame_ /
// accumulator_ / render_angle_deg_ are folded in), so the invariant is
// RUN-TO-RUN reproducibility of the whole sequence, not frame-to-frame
// equality. Asserts:
//   (1) two independent Engine instances in ONE process produce byte-identical
//       hash SEQUENCES (also the JS-pristine / no-surviving-process-global
//       check from P0c -- a leaked static would diverge the second engine),
//   (2) the same holds with a real GLB scene loaded through the block-backed
//       Mesh/Prefab pools (P2/P3),
//   (3) perturbing the scene changes the sequence (discrimination -- the hash
//       is a real projection of state, not a constant).
// TAGS: [golden][statehash]

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "engine.hpp"
#include "engine_headless.hpp"
#include "test_seams.hpp"
#include "golden_js.hpp"

namespace seam = cairns::test_seams;

namespace {

// Boot a headless 512x512 fixed-clock engine. use_fixed_clock => golden_=true,
// so the SIM determinism hash is stamped every frame.
void BootGolden(cairns::Engine& e) {
    cairns::rhi::InitConfig icfg{};
    icfg.surfaceless = true;
    icfg.width = 512;
    icfg.height = 512;
    cairns::EngineConfig ecfg{};
    ecfg.use_fixed_clock = true;
    REQUIRE(e.GreaterInit(icfg, ecfg));
}

// Advance exactly one frame and return the SIM digest stamped during it.
uint64_t StepHash(cairns::Engine& e) {
    cairns::golden::DriveJs(
        e,
        R"JS(cairns.dispatch("cairns.render.advanceFrames", { count: 1 });)JS");
    return e.LastSimHash();
}

std::vector<uint64_t> Sequence(cairns::Engine& e, int frames) {
    std::vector<uint64_t> seq;
    seq.reserve(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        seq.push_back(StepHash(e));
    }
    return seq;
}

constexpr int kFrames = 32;

}  // namespace

// (1) Reproducibility, no assets required: two engines, same (empty) workload.
SCENARIO("sim hash is reproducible run-to-run (two engines, one process)",
         "[golden][statehash]") {
    seam::EnsureImguiContext();

    cairns::Engine a;
    BootGolden(a);
    const std::vector<uint64_t> seq_a = Sequence(a, kFrames);

    cairns::Engine b;
    BootGolden(b);
    const std::vector<uint64_t> seq_b = Sequence(b, kFrames);

    REQUIRE(seq_a.size() == static_cast<size_t>(kFrames));
    REQUIRE(seq_a == seq_b);
    // Non-trivial: the digest is real (nonzero) and tracks the sim clock
    // (advances across frames) -- proves it isn't a constant the equality
    // above would pass vacuously.
    REQUIRE(seq_a.front() != 0);
    REQUIRE(seq_a.front() != seq_a.back());
}

// (2) Reproducibility with a real GLB scene loaded through the block-backed
// Mesh/Prefab pools (P2/P3). The strongest run-to-run check: the draw list,
// world matrices, and pool contents all flow through cpu_block_.
SCENARIO("sim hash is reproducible with a loaded scene (two engines)",
         "[golden][statehash]") {
    if (!seam::AssetsPresent({"aatrox.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();

    const char* kScene = R"JS(
        const p = cairns.dispatch("cairns.prefab.load",
                                  { path: "aatrox.glb" }).result.prefab;
        for (let i = 0; i < 3; ++i) {
            cairns.dispatch("cairns.scene.instantiate",
                { prefab: p, x: i * 1.5 - 1.5, y: 0, z: -4, scale: 1 });
        }
    )JS";

    cairns::Engine a;
    BootGolden(a);
    cairns::golden::DriveJs(a, kScene);
    const std::vector<uint64_t> seq_a = Sequence(a, kFrames);

    cairns::Engine b;
    BootGolden(b);
    cairns::golden::DriveJs(b, kScene);
    const std::vector<uint64_t> seq_b = Sequence(b, kFrames);

    REQUIRE(seq_a == seq_b);
    REQUIRE(seq_a.front() != 0);
}

// (3) Discrimination: same prefab, different instantiate X => different draw
// world matrices => the hash sequence MUST differ. Guards against a hash that
// silently ignores the state it's meant to cover.
SCENARIO("sim hash discriminates a scene perturbation",
         "[golden][statehash]") {
    if (!seam::AssetsPresent({"aatrox.glb"})) {
        SKIP("assets absent");
    }
    seam::EnsureImguiContext();

    const char* kSceneA = R"JS(
        const p = cairns.dispatch("cairns.prefab.load",
                                  { path: "aatrox.glb" }).result.prefab;
        cairns.dispatch("cairns.scene.instantiate",
            { prefab: p, x: 0, y: 0, z: -4, scale: 1 });
    )JS";
    const char* kSceneB = R"JS(
        const p = cairns.dispatch("cairns.prefab.load",
                                  { path: "aatrox.glb" }).result.prefab;
        cairns.dispatch("cairns.scene.instantiate",
            { prefab: p, x: 1.5, y: 0, z: -4, scale: 1 });
    )JS";

    cairns::Engine a;
    BootGolden(a);
    cairns::golden::DriveJs(a, kSceneA);
    const std::vector<uint64_t> seq_a = Sequence(a, 16);

    cairns::Engine b;
    BootGolden(b);
    cairns::golden::DriveJs(b, kSceneB);
    const std::vector<uint64_t> seq_b = Sequence(b, 16);

    REQUIRE(seq_a != seq_b);
}
