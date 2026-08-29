// engine_headless.hpp
//
// Thin facade over Engine's headless render+dump methods for TUs that must
// not pull engine.hpp (which transitively defines NS/MTL/CA private-
// implementation symbols via gfx_api.hpp; including from more than one TU
// produces duplicate symbols at link). cairns_control's render_ops.cpp uses
// only this facade; the implementation lives in engine_headless.cpp inside
// cairns_core where engine.hpp is instantiated exactly once.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "scene/selection.hpp"
#include "util/load_trace.hpp"

namespace cairns {

class Engine;

namespace headless {

bool RenderFrame(Engine* engine);
bool DumpFinalTarget(Engine* engine, const std::filesystem::path& path);

// Note: SetRandomSeed must run BEFORE GreaterInit's initParticles for the
// new seed to take effect on particle init. Calling at runtime updates the
// field but leaves the in-flight particle state alone.
void SetRandomSeed(Engine* engine, uint32_t seed);

// Reallocate final_target_ at the given dims. Returns false if the engine
// is null, not surfaceless, or the allocation failed. Used by
// window.resize in headless mode.
bool ResizeFinalTarget(Engine* engine, uint32_t w, uint32_t h);

uint32_t GetFinalTargetWidth(Engine* engine);
uint32_t GetFinalTargetHeight(Engine* engine);

// Windowed-mode dump: queue a swapchain readback for the NEXT presented
// frame and write it to `path`. Lives on this facade because the live agent
// transport in cairns_app reuses the same registry shape as cairns_serve.
// Returns false if the engine is null (the request itself just queues the
// path; the actual readback happens in the next Frames::End).
bool RequestWindowDump(Engine* engine, const std::filesystem::path& path);

// P4 selection / highlight / pick. Document-side state on Engine; this facade
// just forwards through. revision counters tick on every mutation so the
// protocol's cairns.selection.changed event has something to compare against.
std::vector<SelectionTarget> GetSelection(Engine* engine);
std::vector<SelectionTarget> GetHighlights(Engine* engine);
uint32_t GetSelectionRevision(Engine* engine);
void SetSelection(Engine* engine, std::vector<SelectionTarget>&& targets);
void AddSelection(Engine* engine, const SelectionTarget& target);
void RemoveSelection(Engine* engine, const SelectionTarget& target);
void ClearSelection(Engine* engine);
void SetHighlights(Engine* engine, std::vector<SelectionTarget>&& targets);
void ClearHighlights(Engine* engine);
void RequestPick(Engine* engine, int viewport, uint32_t x, uint32_t y);

// #194 viewport lifetime + tile layout (NDC 0..1 over the swap pane).
// #220 Step 4: OpenViewport returns the engine-assigned monotonic name
// counter ("vp{N}" without prefix; cast to int so -1 signals capped).
// Names are never reused.
int OpenViewport(Engine* engine);
bool CloseViewport(Engine* engine);
// Legacy int-slot setLayout kept; new wire-name path is
// SetViewportLayoutByName.
bool SetViewportLayout(Engine* engine, int viewport,
                        float x, float y, float w, float h);
bool SetViewportLayoutByName(Engine* engine, uint32_t name_counter,
                              float x, float y, float w, float h);
int ActiveViewportCount(Engine* engine);

// #208: poll the most recent resolved pick. resolved=false until the
// engine has run a frame after RequestPick + completed the readback.
struct PickResultExport {
    bool resolved = false;
    int viewport = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    SelectionType type = SelectionType::kEntity;
    uint32_t id = 0;
    // Stub value (final_target_ BGRA at the click texel) until #206 lands
    // the dedicated R32U ID buffer; id decoder swaps with the buffer.
    uint32_t raw = 0;
};
PickResultExport ConsumePickResult(Engine* engine);

// #269: spawn one hero entity in active_scene_ from a pre-loaded scene
// at the given world-space position + uniform scale. Returns the new
// entt entity id (0 on failure). Frame-thread safe assuming the engine
// is between Begin()/End() — call from the control thread; the next
// frame picks up the new entity via the dirty-world rebuild path.
uint32_t InstantiatePrefab(Engine* engine, uint32_t scene_idx,
                    float x, float y, float z, float scale,
                    float time_phase);
uint32_t NumPrefabs(Engine* engine);
float PrefabExtentMax(Engine* engine, uint32_t scene_idx);
std::vector<uint32_t> ListActiveSceneEntities(Engine* engine);
bool SetEntityTransform(Engine* engine, uint32_t entity_int,
                         float x, float y, float z, float scale);
uint32_t ClearActiveScene(Engine* engine);

// #224 L3: the instrument.
LoadTrace LastLoadTrace(Engine* engine);
LoaderCounters Counters(Engine* engine);
// #224 L2: validation report from the last LoadPrefabBatch call.
ValidationReport LastValidationReport(Engine* engine);

// #224 L5: runtime batch load (drain + load + re-upload anim tables).
// Returns {first_prefab_idx, count}.
struct LoadBatchExport { uint32_t first_prefab_idx = 0; uint32_t count = 0; };
LoadBatchExport RuntimeLoadGlbs(Engine* engine,
                                  uint32_t cursor, uint32_t count);
// #224 L9: single-path load. The JS catalog loops + calls this once
// per file. Path is absolute OR a short name resolved via the engine's
// static resource lookup. Returns the new prefab_idx (or UINT32_MAX
// on failure -- the agent should check). drain + re-upload anim
// tables happen inside, same as the multi-path batch.
uint32_t RuntimeLoadGlbPath(Engine* engine, const std::string& path);

// #224 L5: instantiate `prefab_count` prefabs starting at `first_prefab_idx`
// into active scene + slide all existing actors to the new fitted grid.
// Returns the new entity ids.
std::vector<uint32_t> InstantiateGridFitted(Engine* engine,
                                              uint32_t first_prefab_idx,
                                              uint32_t prefab_count);

// #224 L8: editor-chrome (selection outline) toggle.
bool EditorChromeEnabled(Engine* engine);
void SetEditorChromeEnabled(Engine* engine, bool on);

// #224 L6: APPEND-only debug pair.
uint32_t DebugSnapshotPrefabHandles(Engine* engine);
uint32_t DebugAssertAppendOnly(Engine* engine);

// #224 L7: deterministic load-twice check. Runs RuntimeLoadGlbs twice
// with the same {cursor, count}, compares prefab/mesh counts and
// per-actor fitted transforms (modulo trace timing). Returns mismatch
// count; 0 == deterministic.
uint32_t DebugDeterminismCheck(Engine* engine,
                                 uint32_t cursor, uint32_t count);

}  // namespace headless
}  // namespace cairns
