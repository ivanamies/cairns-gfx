// tests/test_seams.hpp
//
// Thin wrappers over the Engine + headless API that the Tier G goldens use as
// integration seams. Keeping them here (rather than inlining each Engine call
// site into a test) (a) gives the tests one stable surface area to read
// against, and (b) lets us return false from unwired seams so the scenario
// SECTIONs `SKIP` instead of erroring out -- the golden ladder grows green-bar
// as seams get wired.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cairns {
class Engine;
struct HudStats;
}  // namespace cairns

namespace cairns::test_seams {

// Stable platform key for ref filenames. macos-metal / macos-vk / android-vk /
// ios-sim-metal / ios-metal. Same string the golden tests pass to
// LoadImageRef.
const char* PlatformKey();

// CLI shells (sdl-min / cairns_serve) call ImGui::CreateContext() before
// Engine::GreaterInit; Engine::GreaterInit's initImguiPipeline reads
// ImGui::GetIO() and aborts without a context. Tests calling GreaterInit
// directly (the ladder rungs) must call this first to install one. Idempotent.
void EnsureImguiContext();

// Init the engine with surfaceless rendering + a FixedClock (deterministic
// frame dt), WITHOUT the CLI dump+exit path. The golden test loop calls
// AdvanceToGoldenFrame after, then reads the final target.
bool BootHeadless(cairns::Engine& engine, uint32_t width, uint32_t height);

// Reach kGoldenDumpFrame (60) by ticking RenderHeadlessFrame in a loop.
// Returns true iff every tick returned true.
bool AdvanceToGoldenFrame(cairns::Engine& engine);

// Tick `N` frames forward. Golden subjects call this twice to land on
// frame 9 then frame 55 -- two refs per subject per platform.
bool AdvanceFrames(cairns::Engine& engine, uint32_t n);

struct FrameStats {
    uint32_t draw_calls = 0;
    uint64_t verts_processed = 0;
    uint32_t culled = 0;
    uint32_t submitted = 0;
};
// Frustum-cull golden readback. Pulls the BSF / extract counters from the
// last advanced frame.
bool LastFrameStats(cairns::Engine& engine, FrameStats& out);

// Deterministic particle SSBO readback for the particles state-hash golden.
// Returns false (the scenario SKIPs) on backends without buffer readback.
bool ReadParticleBuffer(cairns::Engine& engine, std::vector<uint8_t>& out);

// Ladder & scenario screen readback. Same surface as
// Engine::ReadFinalTargetRgba; mirrored here so tests don't include
// engine.hpp twice.
bool ReadFinalTargetRgba(cairns::Engine& engine, std::vector<uint8_t>& rgba,
                         uint32_t& w, uint32_t& h);

void DumpFinalTargetPng(cairns::Engine& engine, const std::string& name);

// Skin-output buffer readback for the append-only load acceptance pair.
// Returns false until Resources::ReadBackBuffer lands; then the buffer
// SECTION turns from SKIP to a real cross-platform check.
bool ReadSkinOutputUsedBytes(cairns::Engine& engine, std::vector<uint8_t>& out);

// Nested-graph resolved-depth readback. Same SKIP discipline.
bool ReadResolvedDepth(cairns::Engine& engine, std::vector<uint8_t>& out);

// Asset presence check used by every ladder/scenario test. Returns true iff
// each name resolves to a file the engine could load.
bool AssetsPresent(const std::vector<std::string>& glbs);

// MD5 digest of `bytes`, lowercase hex, no separators. Used by every golden
// + the bake script.
std::string Md5Hex(const std::vector<uint8_t>& bytes);

}  // namespace cairns::test_seams
