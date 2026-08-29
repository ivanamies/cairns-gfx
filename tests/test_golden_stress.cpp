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
#include "golden_js.hpp"

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

// Compose the spawnFitted dispatch with the 100 distinct glb names as JS.
std::string SpawnHundredJs(const std::vector<std::string>& glbs) {
    std::string js = "cairns.dispatch(\"cairns.scene.spawnFitted\", { glbs: [";
    for (const std::string& g : glbs) {
        js += "\"";
        js += g;
        js += "\", ";
    }
    js += "], instances: ";
    js += std::to_string(glbs.size());
    js += ", animated: true });";
    return js;
}

}  // namespace

SCENARIO("stress: 100 distinct animated champions (JS-driven)",
         "[stress][golden]") {
    const std::vector<std::string> glbs = HundredChampions();
    cairns::golden::RunJsSubject("hundred_champ_anim", 512, 512, glbs,
                                 SpawnHundredJs(glbs));
}
