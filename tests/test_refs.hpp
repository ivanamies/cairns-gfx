// tests/test_refs.hpp
//
// Reference-image / reference-buffer loader + auto-baker for Tier G.
//
//   Image refs are PER-PLATFORM:  tests/refs/{name}.{platform}.imghash
//     (rasterization legitimately differs across backends -- macOS Metal vs
//     MoltenVK on macOS vs Adreno vk vs iOS Metal each get their own).
//   Buffer/counter refs are CROSS-PLATFORM: tests/refs/{name}.skinhash
//     (skinning output is meant to be deterministic across all four; if it
//     diverges that is the bug the divergence test catches).
//
// `observed` is passed to LoadImageRef/LoadSkinRef so that the FIRST run on
// a platform with no ref file BAKES the observed hash to disk and returns
// it. Subsequent runs read the file. Set `CAIRNS_GFX_BAKE_REFS=1` in the
// environment to force re-baking on every run (used by maintainers when
// intentionally regenerating refs after a rendering change).

#pragma once

#include <string>

namespace cairns::test_refs {

// Returns the reference hash for {name, platform}. When no ref file exists,
// `observed` is written to disk and returned (auto-bake). Returns empty
// string if writing failed (cannot bake -- the test SKIPs).
std::string LoadImageRef(const std::string& name, const std::string& platform,
                         const std::string& observed);

// Cross-platform shared ref (no platform suffix). Same auto-bake contract.
std::string LoadSkinRef(const std::string& name, const std::string& observed);

}  // namespace cairns::test_refs
