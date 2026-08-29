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
#include <string>
#include <vector>

#include "scene/component_type.hpp"  // #229 C4.2 ComponentType
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
// #229 C4.2 entity ops. scene_index -1 = active. TRS as plain float arrays so
// the control layer (json / -fno-exceptions) needs no glm.
bool DestroyEntity(Engine* engine, int scene_index, uint32_t entity);
bool SetEntityTRS(Engine* engine, int scene_index, uint32_t entity,
                  const float t3[3], const float r4[4], const float s3[3]);
bool GetEntityTRS(Engine* engine, int scene_index, uint32_t entity,
                  float out_t3[3], float out_r4[4], float out_s3[3]);
bool SetEntityParent(Engine* engine, int scene_index, uint32_t entity,
                     uint32_t parent, bool clear);
uint32_t FindEntityByName(Engine* engine, int scene_index,
                          const std::string& name);
bool SetEntityName(Engine* engine, int scene_index, uint32_t entity,
                   const std::string& name);
// #229 C4.2 generic component ops. type = ComponentType; add/get are typed
// (props differ per component), has/remove switch on the type id.
bool HasComponent(Engine* engine, int scene_index, uint32_t entity,
                  ComponentType type);
bool RemoveComponent(Engine* engine, int scene_index, uint32_t entity,
                     ComponentType type);
bool GetEntityName(Engine* engine, int scene_index, uint32_t entity,
                   std::string& out);
bool SetEntityCamera(Engine* engine, int scene_index, uint32_t entity,
                     float fov_y_rad, float near_z, float far_z, bool is_main);
bool GetEntityCamera(Engine* engine, int scene_index, uint32_t entity,
                     float& fov_y_rad, float& near_z, float& far_z,
                     bool& is_main);
bool AddParticleEmitter(Engine* engine, int scene_index, uint32_t entity);
bool SetEntityRenderable(Engine* engine, int scene_index, uint32_t entity,
                         uint32_t layer_mask, uint32_t flags);
bool GetEntityRenderable(Engine* engine, int scene_index, uint32_t entity,
                         uint32_t& layer_mask, uint32_t& flags);
uint32_t ClearActiveScene(Engine* engine);
// #229 M0b: per-Engine synthetic scene id (was the g_scene_counter global).
uint64_t NextSceneId(Engine* engine);

// #228 F2: drop every resident prefab + DeferFree their GPU resources
// through the F1 ring. Calls ClearActiveScene internally so the post-
// state is "engine empty, ready for fresh loads." Returns the number
// of prefabs that were dropped (0 if already empty).
uint32_t UnloadAllPrefabs(Engine* engine);

// #228 R1: hot-reload the resident prefab matching |path|. Re-parses
// the GLB and swaps the pool slot's contents behind the same PrefabId;
// entities holding AssetRef remain valid and render the new mesh next
// frame. Returns true on success, false if no resident prefab matches
// the path or the parse failed (existing prefab is left untouched).
bool ReloadPrefabByPath(Engine* engine, const std::string& path);

// #228 R2: hot-reload a pipeline by logical name (anim_eval / skin /
// particle). KEEP-LAST-GOOD: on compile/link failure the existing
// pipeline stays bound -- rendering never goes black from a broken
// shader. Returns true on swap, false on failure or unknown name.
bool ReloadPipelineByName(Engine* engine, const std::string& name);

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

// #195 JS-driven golden primitives. The SCENARIO choreography lives in JS
// (cairns.dispatch); these are the general engine ops it composes.
bool SpawnFitted(Engine* engine, const std::vector<std::string>& glbs,
                 uint32_t instances, bool animated);
void UseScene(Engine* engine, uint32_t index);
bool SetViewportScene(Engine* engine, int viewport, uint32_t scene_index);
bool SetViewportParticles(Engine* engine, int viewport, bool on);
bool SetViewportCamera(Engine* engine, int viewport, float x, float y, float z,
                       float yaw, float pitch);
bool AdvanceFrames(Engine* engine, uint32_t n);
void EnableParticles(Engine* engine, bool on);
void SetImguiInGolden(Engine* engine, bool on);
void SetInjectedHud(Engine* engine, float cpu_ms, float fps);
void SetTinyTriangle(Engine* engine, bool on);
void SetNestedGraphMode(Engine* engine, bool on);

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

// #228 H2: invariant check across the LoadPrefabBatch manifest contract.
// Returns the number of violations; on non-zero, the engine is in an
// inconsistent state (one of the manifest lines forgot a fixup or a
// new state bucket was added without its matching invariant).
struct InvariantsExport {
    uint32_t violations = 0;
    std::vector<std::string> messages;
};
InvariantsExport DebugCheckInvariants(Engine* engine);

// #224 L7: deterministic load-twice check. Runs RuntimeLoadGlbs twice
// with the same {cursor, count}, compares prefab/mesh counts and
// per-actor fitted transforms (modulo trace timing). Returns mismatch
// count; 0 == deterministic.
uint32_t DebugDeterminismCheck(Engine* engine,
                                 uint32_t cursor, uint32_t count);

}  // namespace headless
}  // namespace cairns
