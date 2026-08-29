// tests/test_seams.hpp
//
// Thin wrappers over the Engine + headless API that the Tier G goldens use as
// integration seams. Keeping them here (rather than inlining each Engine call
// site into a test) (a) gives the tests one stable surface area to read
// against, and (b) lets us return false from unwired seams so the scenario
// SECTIONs `SKIP` instead of erroring out -- the golden ladder grows green-bar
// as Phase 3 lands more of these.

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

// C.18: parameterized advance + per-frame capture support. Tick `N`
// frames forward. Tests call this twice to land on frame 9 then frame
// 55 -- two refs per rung per platform.
bool AdvanceFrames(cairns::Engine& engine, uint32_t n);

// Compose a ladder scene from glb file names + instance count. Returns true
// iff the requested entities are live. When `glbs` is empty, configures the
// tiny_quad path (L1 triangle rung).
bool BuildScene(cairns::Engine& engine,
                      const std::vector<std::string>& glbs,
                      uint32_t instances, bool animated);

// Same shape but for scenarios: load a set of glbs and spawn one instance
// per name. Used by G2 hot reload + G3/G4 multi-instance.
bool SpawnGlbs(cairns::Engine& engine,
               const std::vector<std::string>& glbs, bool animated);

// Clear all spawned entities; the asset registry retains its slots but the
// scene goes empty. Used by G2 hot reload between checkpoint passes.
bool ClearSpawned(cairns::Engine& engine);

// G3 helper: open viewport 1 alongside viewport 0, place a glb there, opt-in
// to particle rendering only on that viewport.
bool OpenSecondViewport(cairns::Engine& engine, const char* glb,
                        float yaw_rad, bool with_particles);

// G4 helper: program the render graph for one color + resolved-depth +
// third-camera pass.
bool ConfigureNestedGraph(cairns::Engine& engine);

// G5 helper: spawn N inside the active frustum + M outside it. The cull
// counter scenario asserts draw_calls == in and culled == out.
bool SpawnInsideOutsideSplit(cairns::Engine& engine, const char* glb,
                             uint32_t inside, uint32_t outside);

struct FrameStats {
    uint32_t draw_calls = 0;
    uint64_t verts_processed = 0;
    uint32_t culled = 0;
    uint32_t submitted = 0;
};
// G5 readback. Pulls the BSF / extract counters from the last advanced frame.
bool LastFrameStats(cairns::Engine& engine, FrameStats& out);

// G1: deterministic particle SSBO readback. Until production particle init
// switches to cairns::ParticleRng, this returns false and the scenario SKIPs.
bool ReadParticleBuffer(cairns::Engine& engine, std::vector<uint8_t>& out);

// G6: imgui overlay toggle.
bool EnableImguiOverlay(cairns::Engine& engine, bool on);

// G6: pump fixed HUD numbers into the overlay so the screen is byte-stable.
bool InjectHudStats(cairns::Engine& engine, const cairns::HudStats& s);

// G1: enable / disable particle compute + draw at the engine level. Tests
// that need particles (G1, G3 right viewport) flip this true; everything
// else lives with the default-off Phase A.2 gate.
bool EnableParticles(cairns::Engine& engine, bool on);

// Ladder & scenario screen readback. Same surface as
// Engine::ReadFinalTargetRgba; mirrored here so tests don't include
// engine.hpp twice.
bool ReadFinalTargetRgba(cairns::Engine& engine, std::vector<uint8_t>& rgba,
                         uint32_t& w, uint32_t& h);

void DumpFinalTargetPng(cairns::Engine& engine, const std::string& name);

// L6/L7 skin-output buffer readback. Returns false until the stash@{0}
// ReadBackBuffer salvage is applied; that day the rungs' buffer SECTION
// turns from SKIP to a real cross-platform check.
bool ReadSkinOutputUsedBytes(cairns::Engine& engine, std::vector<uint8_t>& out);

// G4 resolved-depth readback. Same SKIP discipline.
bool ReadResolvedDepth(cairns::Engine& engine, std::vector<uint8_t>& out);

// Asset presence check used by every ladder/scenario test. Returns true iff
// each name resolves to a file the engine could load.
bool AssetsPresent(const std::vector<std::string>& glbs);

// MD5 digest of `bytes`, lowercase hex, no separators. Used by every golden
// + the bake script.
std::string Md5Hex(const std::vector<uint8_t>& bytes);

}  // namespace cairns::test_seams
