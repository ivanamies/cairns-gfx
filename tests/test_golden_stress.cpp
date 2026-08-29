// tests/test_golden_stress.cpp
//
// TIER G STRESS -- scale workloads, kept OUT of [scenarios] so the fast
// correctness pass stays fast. Same per-platform golden capture as the
// scenarios, just heavier scenes.
// TAGS: [stress][golden]

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "util/debug_asset.hpp"  // kDebugGlbs -- 100 distinct champion names
#include "golden_subject.hpp"

namespace {

// The 100 distinct champion glbs from the engine's debug manifest
// (kDebugGlbs[3..103)). One instance per glb -> 100 DISTINCT actors.
std::vector<std::string> HundredChampions() {
    std::vector<std::string> out;
    out.reserve(cairns::kDebugGlbsToParse);
    for (uint32_t i = cairns::kDebugGlbsToParseStart;
         i < cairns::kDebugGlbsToParseStart + cairns::kDebugGlbsToParse; ++i) {
        out.emplace_back(cairns::kDebugGlbs[i]);
    }
    return out;
}

}  // namespace

SCENARIO("stress: 100 distinct animated champions", "[stress][golden]") {
    cairns::golden::RunSubject("hundred_champ_anim", HundredChampions(),
                               cairns::kDebugGlbsToParse, /*animated=*/true,
                               {0, 0, 0, 0, 0});
}
