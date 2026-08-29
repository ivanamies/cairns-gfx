#pragma once

#include "util/define.hpp"
#include "util/alloc_count.hpp"  // Per-phase allocation receipts.
#include "util/memory_budget.hpp"  // Single source of reservation sizes.
#include "engine/engine_config.hpp"  // EngineConfig (the shell includes it directly).
#include "engine/particle_system.hpp"
#include "engine/pick_selection.hpp"  // PickSelection + PickResult.
#include "engine/scene_manager.hpp"
#include "engine/anim_skin_system.hpp"
#include "engine/viewport_manager.hpp"  // ViewportManager + kNumViewports.
#include "engine/prefab_store.hpp"
#include "engine/present_targets.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>  // std::memcpy for SSBO-pack reinterprets.
#include <string_view>
#include <filesystem>
#include <optional>
#include <thread>
#include <chrono>
#include <fstream>
#include <numbers>
#include <variant>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <stb_image_write.h>

#include "render/worker_context.hpp"
#include "util/cpu_arena.hpp"
#include "util/cpu_pool.hpp"  // RangePool for skinning_.output_pool.
#include "util/chunk_allocator.hpp"  // The one owning CPU block.
#include "util/fnv1a.hpp"  // Per-frame determinism hash.
#include "util/device_caps.hpp"  // boot-invariant + HUD/skin fit predicates
#include "util/hud_stats.hpp"
#include "util/animation_runtime.hpp"  // SelectWalkingClip + sampler.
#include "render/render_proxy.hpp"  // SkinnedAttachment Hot/Cold.
#include "render/particle_emitter.hpp"  // ParticleRng (portable mt19937).

#include "gfx_api.hpp"
#include "rhi/init_config.hpp"
#include "rhi/swap_chain.hpp"
#include "util/misc.hpp"
#include "util/render_pass_globals.hpp"
#include "util/offset_allocator.hpp"
#include "util/gltf_loader.hpp"
#include "util/primitives.hpp"
#include "util/debug_asset.hpp"
#include "util/load_trace.hpp"  // LoadTrace / LoaderCounters PODs.
#include "util/draw.hpp"
#include "util/draw_key.hpp"
#include "util/material_gpu.hpp"
#include "util/worker_pool.hpp"
#include "util/scene_gpu.hpp"
#include "util/timer.hpp"
#include "util/frame_clock.hpp"
#include "util/log.hpp"
#include "util/print_allocator.hpp"
#include "util/signpost.hpp"
#include "scene/asset_registry.hpp"
#include "scene/components.hpp"
#include "scene/component_type.hpp"  // ComponentType for the generic component ops.
#include "scene/world.hpp"
#include "scene/viewport.hpp"
#include "scene/selection.hpp"
#include "render/frame_packet.hpp"
#include "render/render_extract.hpp"
#include "render/scene_draw_ranges.hpp"  // CarveSceneDrawRanges (spec-tested).
#include "render/render_graph.hpp"
#include "render/render_scene.hpp"
#include "render/render_thread.hpp"
#include "scene/transform_propagation.hpp"

#include <memory>
#include "rhi/rhi.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/command_recorder.hpp"
#include "util/task_guard.hpp"
#include "imgui.h"

#include "platform/platform.hpp"

namespace cairns {

// Forward decl: the JS-composed scene primitives (SpawnFitted etc.) call
// into the headless free-function layer, but engine_headless.hpp
// transitively includes engine.hpp -- this avoids the include cycle.
class Engine;
namespace headless {
uint32_t RuntimeLoadGlbPath(Engine* engine, const std::string& path);
}

inline static constexpr uint32_t kUboAlign = 32;
inline static constexpr uint32_t kMeshPosBindSlot = 0;

// TODO: these four DrawKey fields are mocked to 0; hook them into the RDG
// (render dependency graph) later.
inline static constexpr uint32_t kMockTranslucency = 0;
inline static constexpr uint32_t kMockViewport = 0;
inline static constexpr uint32_t kMockViewportLayer = 0;
inline static constexpr uint32_t kMockFullscreenLayer = 0;

class Engine {
public:

    using TexHandle = rhi::Handle<rhi::Texture>;
    using BufHandle = rhi::Handle<rhi::Buffer>;
    using DynBufId = uint32_t;
    using ShaderHandle = rhi::Handle<rhi::Shader>;
    // MatId is a generational Handle into prefab_store_.materials
    // (cairns::ResourceManager<Material>). Stale slots fail safe at
    // GetHot/GetCold instead of silently aliasing a recycled bind group.
    using MatId = cairns::Handle<cairns::Material>;
    using SamplerHandle = rhi::Handle<rhi::Sampler>;
    using BindGroupId = uint32_t;

    // kFramesInFlight has one home -- cairns::rhi::kFramesInFlight in
    // rhi/resource_manager.hpp. This re-export keeps Engine::kFramesInFlight
    // call sites compiling.
    static constexpr uint32_t kFramesInFlight = cairns::rhi::kFramesInFlight;

    // Caps for the GPU anim_eval kernel + its persistent buffers. KEEP IN
    // SYNC with assets/anim_eval.comp.glsl (records[1024], kMaxNodesPerScene,
    // kMaxJointsPerSkin) and assets/anim_eval.metal.
    static constexpr uint32_t kAnimActorsCap = 1024u;
    static constexpr uint32_t kAnimMaxNodes = 256u;
    static constexpr uint32_t kAnimMaxJoints = 256u;

    // Per-slot storage. drawList / drawListSorted / proxies /
    // draw_world_matrices live here so the game thread can fill slot S while
    // the render thread reads slot ~S. pending_globals etc. are staged by
    // Build and consumed by EncodeDraws.
    static constexpr int kNumViewportsPerSlot = 4;  // Matches kNumViewports.
    struct PerSlot {
        cairns::RenderProxyArrays proxies;
        // These per-frame spans ride the slot's BumpArena: the producer
        // counts first, then arena.AllocateArray, then fills by index.
        // Valid from arena.Reset() at slot Acquire through this slot's
        // frame completion; no span may outlive the next Acquire's Reset.
        std::span<cairns::Draw> drawList;
        std::span<std::pair<cairns::DrawKey, uint32_t>> drawListSorted;
        std::span<glm::mat4> draw_world_matrices;
        // Parallel to draw_world_matrices; baked from MeshProxy::entity_id
        // by BuildMeshOpaqueDraws so unlit.frag can write the per-fragment id.
        std::span<uint32_t> draw_entity_ids;
        // Parallel: each draw's material, so the UBO encode uploads ONE
        // MaterialGpu per referenced material (offset shared across its
        // draws) -- the Draw struct itself never carries it.
        std::span<cairns::Handle<cairns::Material>> draw_material_ids;
        // Shadow pass: same geometry as drawList but with .shader nulled +
        // material/shadow bind groups cleared, so the recorder binds
        // shadow_pso_ (depth-only) and reads only globals + drawtmp.
        std::span<cairns::Draw> shadowDrawList;
        uint32_t shadow_globals_offset = 0;  // globals w/ light VP in view_proj
        bool shadow_active = false;          // scene has a cast_shadows light
        glm::mat4 light_view_proj{1.0f};
        // Post-effect chain, extracted from viewport-0's bound scene (active
        // fallback, same resolution as the shadow light) and insertion-sorted
        // by order. RecordFrame appends fullscreen passes between outline and
        // swap for each entry.
        static constexpr int kMaxPostEffects = 8;
        std::array<cairns::PostEffect, kMaxPostEffects> post_effects{};
        uint32_t post_effect_count = 0;
        // Multi-scene fan-out: every distinct scene any viewport binds is
        // extracted into the single s.proxies union; this records each scene's
        // [mesh) range (at extract) and [draw) range (after the prefix sum) so
        // a viewport draws only its own scene's sorted sub-span. Fixed-cap, on
        // PerSlot -> zero heap, same lifetime as drawList.
        struct SceneDrawRange {
            cairns::SceneId scene{};
            uint32_t mesh_lo = 0;
            uint32_t mesh_hi = 0;
            uint32_t draw_lo = 0;
            uint32_t draw_hi = 0;
        };
        static constexpr int kMaxScenesPerSlot = 8;
        std::array<SceneDrawRange, kMaxScenesPerSlot> scene_ranges{};
        uint32_t scene_ranges_count = 0;
        std::array<int, kNumViewportsPerSlot> viewport_scene_idx{};
        // Per-viewport camera state. One RenderPassGlobals upload per
        // viewport at distinct globals_offset; RecordFrame issues one
        // forward pass per viewport with the matching offset.
        std::array<cairns::rhi::RenderPassGlobals, kNumViewportsPerSlot> pending_globals{};
        std::array<glm::mat4, kNumViewportsPerSlot> pending_view_matrix{};
        std::array<float, kNumViewportsPerSlot> pending_near_z{};
        std::array<float, kNumViewportsPerSlot> pending_far_z{};
        std::array<uint32_t, kNumViewportsPerSlot> globals_offset{};
        uint32_t dt_off = 0;
        cairns::FramePacket pkt{};
        rhi::FrameContext present_fc{};
        rhi::SwapResolveTarget present_target{};
        bool present_ready = false;
        // Per-slot CPU bump arena. SLOT IS THE LOCK -- render thread
        // sees this slot's arena exclusively during RecordFrame; main
        // thread resets at slot Acquire (already blocked on render
        // exclusivity). No mutex, no shared ptr.
        cairns::BumpArena arena{};  // Slab carved from cpu_block_ (kRegionFrame).
        // Per-slot mutex. std::lock_guard / std::unique_lock are the RAII
        // discipline; blocks the second acquirer instead of asserting.
        // SLOT IS THE LOCK -- the state machine already serialises slot
        // ownership; this mutex is the C++-idiomatic primitive that
        // catches misuse + composes with future workers that may want to
        // take_back / try_lock the slot.
        std::mutex slot_mutex;
    };

    Engine() {
        // slots_ is std::array (atomic<bool> not move-constructible). The
        // ctor for PerSlot fills pending_view_matrix / near_z / far_z.
    }
    
    bool initSwapChain(const rhi::InitConfig& cfg);
    
    // SDL fires SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED on the event thread.
    // We don't synchronously touch any GPU state here -- ApplyPendingResize
    // (top of draw()) drains the render thread first.
    bool requestResizeFrameBuffer(uint32_t width, uint32_t height) {
        if (width == 0 || height == 0) {
            return true;
        }
        present_.resize_pending_w = width;
        present_.resize_pending_h = height;
        present_.resize_pending = true;
        return true;
    }

    // The single synchronous resize entry. Record the target dims, then run
    // the same drain+WaitIdle+flush+realloc ApplyPendingResize does at the
    // top of draw(). Headless cairns.window.resize routes here so it shares
    // the windowed path's drain safety instead of destroying final_target
    // under a possibly-in-flight frame.
    bool ApplyResize(uint32_t width, uint32_t height) {
        if (width == 0 || height == 0) {
            return false;
        }
        requestResizeFrameBuffer(width, height);
        ApplyPendingResize();
        return true;
    }

    bool RequestViewportDump(const std::filesystem::path& path) {
        rhi_.frame_capture.SetDumpPath(path);
        return true;
    }

    // Spawn one hero entity in scene_mgr_.active from a pre-loaded prefab.
    // Returns the new entt entity id (0 on failure: bad scene_idx, no
    // active scene, empty prefab_store_.prefab_ids). Caller supplies the
    // full world transform; rendered next frame. SkinRef is opportunistically
    // attached via TryCreateSkinForScene ([SKIN-FAIL] logs the failure modes).
    uint32_t InstantiatePrefab(uint32_t scene_idx, const glm::mat4& world,
                        float time_phase) {
        return InstantiatePrefabImpl(scene_idx, world, time_phase,
                                      /*attach_skin=*/true);
    }

    // No-skin spawn variant: skips TryCreateSkinForScene so the entity
    // renders in its bind pose. Lets the static-spawn golden diverge
    // visibly from the animated one.
    uint32_t InstantiatePrefabNoSkin(uint32_t scene_idx,
                                     const glm::mat4& world) {
        return InstantiatePrefabImpl(scene_idx, world, /*time_phase=*/0.0f,
                                      /*attach_skin=*/false);
    }

    // Shared implementation. Public because both wrappers are inline.
    uint32_t InstantiatePrefabImpl(uint32_t scene_idx, const glm::mat4& world,
                                    float time_phase, bool attach_skin);

    // How many prefabs (GLBs) are loaded; clients call InstantiatePrefab
    // with scene_idx in [0, NumPrefabs()). Lets the NDJSON op validate args.
    uint32_t NumPrefabs() const {
        return static_cast<uint32_t>(prefab_store_.prefab_ids.size());
    }

    // Resolve the shared skin attrs buffer for a mesh from its batch_id.
    // Null on out-of-range or unset (= unskinned mesh).
    rhi::Handle<rhi::Buffer> ResolvedSharedSkin(const cairns::Mesh::Hot& mhot) const {
        if (mhot.batch_id >= prefab_store_.per_batch_shared_skin.size()) {
            return rhi::Handle<rhi::Buffer>::Null;
        }
        return prefab_store_.per_batch_shared_skin[mhot.batch_id];
    }

    // Validate a parsed Prefab against engine caps. Returns true iff no
    // errors. `report` accumulates issues across many calls (the caller
    // resets between batches). Static so engine_headless.cpp / the
    // validate op can call directly.
    static bool ValidatePrefab(const cairns::Prefab::Cold& cold,
                                cairns::ValidationReport& report,
                                uint32_t prefab_idx = UINT32_MAX);

    // Per-mesh weight-sum check. Called in LoadPrefabBatch before
    // CleanupTmps clears cpuSkinAttrs.
    bool ValidateMeshWeights(const cairns::Mesh::Cold& mc,
                              cairns::ValidationReport& report,
                              uint32_t prefab_idx = UINT32_MAX);

    // Append one batch of GLBs to the live Prefab / Mesh pools.
    // Returns {first_prefab_idx, count} = the span [first, first+count)
    // into prefab_store_.prefab_ids where this batch's prefabs landed. Bad parses are
    // skipped + logged (not fatal). The caller is responsible for:
    //   (a) calling Device::WaitIdle() before this if frames are in flight
    //   (b) re-calling uploadAnimTablesGpu() after this if the batch added
    //       skinned prefabs (the GPU anim table is a flat per-prefab array
    //       that must be rebuilt; cheap, scales with total prefabs)
    // Append-only contract: pre-existing Prefabs / Meshes / buffer handles
    // are NOT touched; only new pool slots are written.
    struct LoadPrefabBatchResult {
        uint32_t first_prefab_idx = 0;
        uint32_t count = 0;
    };
    LoadPrefabBatchResult LoadPrefabBatch(
            std::span<const std::filesystem::path> glbs);

    // Procedural primitive (triangle/pyramid/cylinder/ellipse/ellipsoid) built
    // on the CPU + pushed through the static-mesh path as a one-mesh, one-1x1-
    // color-material prefab -- a normal vbo mesh, not a special-case draw.
    LoadPrefabBatchResult LoadProceduralPrefab(cairns::PrimitiveKind kind,
                                               const glm::vec4& color);
    // Spawn one primitive fit to the active viewport (like SpawnFitted).
    bool SpawnPrimitive(cairns::PrimitiveKind kind, const glm::vec4& color);
    // Spawn one of each kind in a fitted grid (the "test all primitives" scene),
    // each with an optional per-entity TransformAnim (mix of motions).
    bool SpawnPrimitivesGrid(const std::vector<cairns::PrimitiveKind>& kinds,
                             const std::vector<glm::vec4>& colors,
                             const std::vector<cairns::TransformAnim>& anims);

    // Runtime entry point for cairns.prefab.loadBatch.
    //   (1) Device::WaitIdle so no in-flight frame reads pools being mutated
    //   (2) LoadPrefabBatch -- the actual parse + upload + Group A
    //   (3) re-upload anim tables (flat array; rebuilds with new prefabs)
    // Returns LoadPrefabBatchResult exactly like LoadPrefabBatch.
    LoadPrefabBatchResult RuntimeLoadBatch(
            std::span<const std::filesystem::path> glbs);

    // The LoadPrefabBatch manifest's contract: one invariant per manifest
    // line -- a manifest line without its matching assert here fails review.
    // Returns the number of violations and (if `out_msgs` non-null) appends
    // a description for each; 0 == contract held. Debug-only in spirit, but
    // exposed via cairns.debug.checkInvariants so the sequence harness can
    // assert it after every load/instantiate.
    uint32_t CheckPrefabStateInvariants(
            std::vector<std::string>* out_msgs = nullptr);

    // The LoadPrefabBatch manifest's helper bodies. Each is span-scoped
    // (or span-independent + idempotent) and runs once per batch. Every
    // helper here has a matching invariant in CheckPrefabStateInvariants;
    // adding a helper without its invariant fails review.

    // Stamp batch_id on every Mesh::Hot of the new prefabs and append
    // the batch's shared skin attrs buffer to prefab_store_.per_batch_shared_skin.
    // Also count the new meshes for the trace.
    void StampBatchSkinAndMeshIds(
            std::span<const cairns::PrefabId> new_span,
            rhi::Handle<rhi::Buffer> batch_shared_skin,
            uint32_t& batch_mesh_count_out);

    // Vulkan Group A descriptor set (positions slice + skin attrs slice)
    // per new skinned mesh. Metal binds buffers directly per batch in
    // DispatchSkinBatches; CreateSkinGroupA returns Null there.
    void BuildGroupABindGroups(
            std::span<const cairns::PrefabId> new_span,
            rhi::Handle<rhi::Buffer> batch_shared_skin);

    // Per-mesh weight-sum validation + Prefab::Cold::CleanupTmps +
    // per-new-mesh cpu temp clear. Combined because they share the
    // pre-cleanup-tmps window for skin attrs.
    void ValidateAndCleanupTmps(
            std::span<const cairns::PrefabId> new_span,
            uint32_t first_prefab_idx,
            cairns::ValidationReport& vreport);

    // Build Material::Hot::set2 for every live material that doesn't
    // already have one. Idempotent skip-if-built so running per-batch
    // doesn't rebuild prior batches' sets.
    void BuildMaterialSet2() {
        prefab_store_.materials.ForEachLive(
            [&](cairns::Material::Hot& hot,
                cairns::Material::Cold& cold) {
                if (!hot.set2.IsNull()) {
                    return;
                }
                // Untextured-material placeholder (texture-less prefab):
                // no texture to bind; leave set2 null (the recorder skips it).
                if (cold.color.IsNull()) {
                    return;
                }
                const rhi::TextureBinding tb{0, cold.color};
                const rhi::SamplerBinding sb{0, cold.sampler};
                rhi::BindGroupDesc bgd{};
                bgd.textures = std::span<const rhi::TextureBinding>(&tb, 1);
                bgd.samplers = std::span<const rhi::SamplerBinding>(&sb, 1);
                hot.set2 = rhi_.resources.CreateBindGroup(bgd);
            });
    }

    // Append the new prefabs' textureHandles to prefab_store_.resident_textures
    // so DrawMeshes' bindless sampler array can index them. APPEND-only;
    // existing entries' indices unchanged.
    void BuildResidentTextures(std::span<const cairns::PrefabId> new_span) {
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Cold* scold = prefab_store_.prefabs.GetCold(sid);
            if (!scold) {
                continue;
            }
            for (rhi::Handle<rhi::Texture> th : scold->textureHandles) {
                prefab_store_.resident_textures.push_back(th);
            }
        }
    }

    // Append the new batch's GLB paths to prefab_store_.glb_paths so the
    // [PICK] log can name a clicked hero by filename. Order matches the
    // prefab_store_.prefab_ids append order; failed parses don't append a
    // prefab_id, so the input `glbs` may be longer -- slice to
    // new_span.size().
    void AppendGlbPaths(std::span<const cairns::PrefabId> new_span,
                         std::span<const std::filesystem::path> glbs) {
        prefab_store_.glb_paths.reserve(prefab_store_.glb_paths.size() + new_span.size());
        for (size_t i = 0; i < new_span.size() && i < glbs.size(); ++i) {
            prefab_store_.glb_paths.push_back(glbs[i]);
        }
    }

    // Register each new prefab as an AssetId so InstantiatePrefab can
    // resolve prefab_store_.per_prefab_asset[prefab_idx]. A forgotten
    // append here fails InstantiatePrefab's bounds check silently
    // (returns entity 0).
    void StampPerPrefabAsset(std::span<const cairns::PrefabId> new_span);

    // Pick `count` GLB paths from the static kDebugGlbs window starting
    // at `cursor`. Returns the resolved paths.
    std::vector<std::filesystem::path> ResolveDebugGlbPaths(
            uint32_t cursor, uint32_t count);

    // Snapshot per-prefab extents for a span -- the FitGridToViewport
    // per_actor_extents input.
    std::vector<float> PrefabExtentSnapshot(uint32_t first_idx,
                                             uint32_t count) {
        std::vector<float> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            out.push_back(PrefabExtentMax(first_idx + i));
        }
        return out;
    }

    // Fit N actors into a grid that fills the active viewport.
    // `per_actor_extents[i]` is each actor's bind-pose max-axis extent
    // (PrefabExtentMax(...) of the actor's source prefab). Returns one
    // world matrix per actor: translation = grid cell at z=-4, scale =
    // uniform cell_size / extent so heterogeneous prefabs occupy the
    // same on-screen footprint. Pure function: deterministic for
    // identical inputs.
    std::vector<glm::mat4> FitGridToViewport(
            uint32_t n_total,
            std::span<const float> per_actor_extents);

    // APPEND-only acceptance test.
    //
    // Snapshot Mesh::Hot handles + batch_id for every live mesh in every
    // resident prefab. Returned as a flat vector of (prefab_idx, mesh_idx,
    // pos_idx, pos_gen, attr_idx, attr_gen, idx_idx, idx_gen, batch_id).
    // The caller passes this snapshot to AssertAppendOnly after a batch
    // load -- a mismatch on ANY prior entry proves the load mutated
    // existing buffers (= bug; the contract is APPENDS, never replaces).
    struct PrefabHandleSnapshot {
        uint32_t prefab_idx = 0;
        uint32_t mesh_idx = 0;
        uint32_t pos_idx = 0;     uint32_t pos_gen = 0;
        uint32_t attr_idx = 0;    uint32_t attr_gen = 0;
        uint32_t idx_idx = 0;     uint32_t idx_gen = 0;
        uint16_t batch_id = 0;
        uint32_t global_base_vertex = 0;
    };
    std::vector<PrefabHandleSnapshot> SnapshotPrefabHandles();

    // For each row in `prior`, look up the same (prefab_idx, mesh_idx) in
    // the current pool and compare every field. Returns the count of
    // mismatched rows; 0 == append-only contract held.
    uint32_t CountAppendOnlyMismatches(
            std::span<const PrefabHandleSnapshot> prior);

    // Load-instrument accessors.
    const cairns::LoadTrace& LastLoadTrace() const { return prefab_store_.last_load_trace; }
    const cairns::ValidationReport& LastValidationReport() const {
        return prefab_store_.last_validation_report;
    }
    // Editor-chrome lives per-viewport (Viewport::Cold::chrome_enabled).
    // These are thin API: read the active viewport, write all active
    // viewports (wire-compatible {on}).
    bool EditorChromeEnabled() {
        const cairns::Viewport::Cold* vc = viewport_mgr_.pool.GetCold(
            viewport_mgr_.ids[viewport_mgr_.active_index]);
        return vc ? vc->chrome_enabled : true;
    }
    void SetEditorChromeEnabled(bool on) {
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            if (cairns::Viewport::Cold* vc =
                    viewport_mgr_.pool.GetCold(viewport_mgr_.ids[v])) {
                vc->chrome_enabled = on;
            }
        }
    }

    // Snapshot/assert helpers exposed to the NDJSON debug ops.
    uint32_t DebugSnapshotPrefabHandles() {
        last_handle_snapshot_ = SnapshotPrefabHandles();
        return static_cast<uint32_t>(last_handle_snapshot_.size());
    }
    uint32_t DebugAssertAppendOnly() {
        return CountAppendOnlyMismatches(
            std::span<const PrefabHandleSnapshot>(
                last_handle_snapshot_.data(),
                last_handle_snapshot_.size()));
    }
    cairns::LoaderCounters Counters() {
        // Live-derive actors_live + textures_resident from the registry +
        // pools. Cached fields (prefabs/meshes/batches) are updated in
        // LoadPrefabBatch.
        cairns::LoaderCounters c = prefab_store_.loader_counters;
        if (cairns::Scene::Cold* wc = scene_mgr_.pool.GetCold(scene_mgr_.active)) {
            c.actors_live = static_cast<uint32_t>(
                wc->registry.storage<entt::entity>().size());
        }
        c.textures_resident =
            static_cast<uint32_t>(prefab_store_.resident_textures.size());
        return c;
    }

    // Bind-pose extent (max axis component of aabb_max - aabb_min)
    // of the scene's first skinned mesh -- the unit a caller normalizes
    // to when picking per-actor scale so heroes occupy a uniform cell
    // on screen. Returns 0 if scene_idx is out of range, no mesh has a
    // bind-pose AABB, or the AABB is degenerate.
    float PrefabExtentMax(uint32_t scene_idx);

    // Bind-pose AABB center of the prefab's first mesh, in mesh-local space.
    // Champions have their origin at the feet, so a fit that anchors the
    // origin pushes the body out the top of frame; subtract this to center.
    glm::vec3 PrefabAabbCenter(uint32_t scene_idx);

    // List every live entity in scene_mgr_.active's registry. The
    // values are entt::to_integral(entity), the same encoding InstantiatePrefab
    // returns. Caller pairs them with SetEntityTransform to drive a
    // no-flash relayout when the spawn count grows.
    std::vector<uint32_t> ListActiveSceneEntities() {
        std::vector<uint32_t> out;
        cairns::Scene::Cold* wc = scene_mgr_.pool.GetCold(scene_mgr_.active);
        if (!wc) {
            return out;
        }
        const auto& reg = wc->registry;
        out.reserve(reg.storage<entt::entity>()->size());
        // Transform is the authoritative spawn marker: WorldTransform is
        // created lazily by PropagateTransforms, so a just-spawned entity
        // has no WorldTransform until the next frame.
        for (const entt::entity e : reg.view<cairns::Transform>()) {
            out.push_back(static_cast<uint32_t>(entt::to_integral(e)));
        }
        return out;
    }

    // Overwrite an entity's WorldTransform. Used by the spawn-
    // relayout path so existing actors slide to new grid cells without
    // the visible empty-then-full flash a clear+respawn produces.
    // Returns false if entity isn't live in scene_mgr_.active's registry.
    bool SetEntityTransform(uint32_t entity_int, const glm::mat4& world);

    // Entity ops. scene_index: 0 primary / 1 secondary, -1 = active.
    // Explicit-scene-first (the N-scene compositor needs it); defaulting
    // to the active scene is only a convenience.
    cairns::Scene::Cold* EntitySceneCold(int scene_index) {
        const cairns::SceneId sid =
            (scene_index < 0) ? scene_mgr_.active
                              : SceneByIndex(static_cast<uint32_t>(scene_index));
        return scene_mgr_.pool.GetCold(sid);
    }
    void MarkSceneDirty(int scene_index) {
        const cairns::SceneId sid =
            (scene_index < 0) ? scene_mgr_.active
                              : SceneByIndex(static_cast<uint32_t>(scene_index));
        if (auto* wh = scene_mgr_.pool.GetHot(sid)) {
            wh->dirty = true;
        }
    }
    bool DestroyEntity(int scene_index, uint32_t entity_int) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!wc->registry.valid(e)) {
            return false;
        }
        wc->registry.destroy(e);
        // Scrub dangling selection/highlight targets so the outline pass never
        // reads a destroyed id (matched by type+id; cross-scene id collisions
        // are rare and a stale selection is worse than a rare over-scrub).
        auto scrub = [&](std::vector<cairns::SelectionTarget>& v, uint32_t& rev) {
            size_t w = 0;
            for (size_t i = 0; i < v.size(); ++i) {
                if (!(v[i].type == cairns::SelectionType::kEntity &&
                      v[i].id == entity_int)) {
                    v[w++] = v[i];
                }
            }
            if (w != v.size()) {
                v.resize(w);
                ++rev;
            }
        };
        scrub(picking_.selection, picking_.selection_rev);
        scrub(picking_.highlights, picking_.highlights_rev);
        MarkSceneDirty(scene_index);
        return true;
    }
    bool SetEntityTRS(int scene_index, uint32_t entity_int, const glm::vec3& t,
                      const glm::quat& r, const glm::vec3& s) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e)) {
            return false;
        }
        reg.emplace_or_replace<cairns::Transform>(e, cairns::Transform{t, r, s});
        if (!reg.all_of<cairns::DirtyTransform>(e)) {
            reg.emplace<cairns::DirtyTransform>(e);
        }
        MarkSceneDirty(scene_index);
        return true;
    }
    bool GetEntityTRS(int scene_index, uint32_t entity_int, glm::vec3& out_t,
                      glm::quat& out_r, glm::vec3& out_s) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e) || !reg.all_of<cairns::Transform>(e)) {
            return false;
        }
        const cairns::Transform& tr = reg.get<cairns::Transform>(e);
        out_t = tr.t;
        out_r = tr.r;
        out_s = tr.s;
        return true;
    }
    // parent_int ignored when clear=true (unparent). Cycle guard walks up from
    // parent; a parent chain that reaches the child is rejected.
    bool SetEntityParent(int scene_index, uint32_t entity_int,
                         uint32_t parent_int, bool clear) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e)) {
            return false;
        }
        if (clear) {
            reg.remove<cairns::Parent>(e);
        } else {
            const entt::entity p = static_cast<entt::entity>(parent_int);
            if (!reg.valid(p) || p == e) {
                return false;
            }
            entt::entity cur = p;
            for (int guard = 0; guard < 4096 && cur != entt::null; ++guard) {
                if (cur == e) {
                    return false;  // would create a cycle
                }
                auto* par = reg.try_get<cairns::Parent>(cur);
                cur = par ? par->value : entt::null;
            }
            reg.emplace_or_replace<cairns::Parent>(e, cairns::Parent{p});
        }
        if (!reg.all_of<cairns::DirtyTransform>(e)) {
            reg.emplace<cairns::DirtyTransform>(e);
        }
        MarkSceneDirty(scene_index);
        return true;
    }
    uint32_t FindEntityByName(int scene_index, const std::string& name) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return UINT32_MAX;
        }
        auto view = wc->registry.view<const cairns::Name>();
        for (const entt::entity e : view) {
            if (view.get<const cairns::Name>(e).value == name) {
                return static_cast<uint32_t>(entt::to_integral(e));
            }
        }
        return UINT32_MAX;
    }
    bool SetEntityName(int scene_index, uint32_t entity_int,
                       const std::string& name) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e)) {
            return false;
        }
        reg.emplace_or_replace<cairns::Name>(e, cairns::Name{name});
        return true;
    }

    // Generic component ops. Presence/removal switch on ComponentType
    // (entt needs the concrete type). Add/Get are per-type (props differ) --
    // the control-side table binds each to a typed method below.
    bool HasComponent(int scene_index, uint32_t entity_int,
                      cairns::ComponentType t) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!wc->registry.valid(e)) {
            return false;
        }
        auto& reg = wc->registry;
        switch (t) {
            case cairns::ComponentType::kName:
                return reg.all_of<cairns::Name>(e);
            case cairns::ComponentType::kCamera:
                return reg.all_of<cairns::CameraComponent>(e);
            case cairns::ComponentType::kParticleEmitter:
                return reg.all_of<cairns::ParticleEmitterComponent>(e);
            case cairns::ComponentType::kRenderable:
                return reg.all_of<cairns::Renderable>(e);
            case cairns::ComponentType::kTransform:
                return reg.all_of<cairns::Transform>(e);
            case cairns::ComponentType::kDirectionalLight:
                return reg.all_of<cairns::DirectionalLight>(e);
            case cairns::ComponentType::kPostEffect:
                return reg.all_of<cairns::PostEffect>(e);
            default:
                return false;
        }
    }
    bool RemoveComponent(int scene_index, uint32_t entity_int,
                         cairns::ComponentType t) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!wc->registry.valid(e)) {
            return false;
        }
        auto& reg = wc->registry;
        switch (t) {
            case cairns::ComponentType::kName:
                reg.remove<cairns::Name>(e);
                break;
            case cairns::ComponentType::kCamera:
                reg.remove<cairns::CameraComponent>(e);
                break;
            case cairns::ComponentType::kParticleEmitter:
                reg.remove<cairns::ParticleEmitterComponent>(e);
                break;
            case cairns::ComponentType::kRenderable:
                reg.remove<cairns::Renderable>(e);
                break;
            case cairns::ComponentType::kDirectionalLight:
                reg.remove<cairns::DirectionalLight>(e);
                break;
            case cairns::ComponentType::kPostEffect:
                reg.remove<cairns::PostEffect>(e);
                break;
            default:
                return false;  // Transform is not removable (draw needs it)
        }
        MarkSceneDirty(scene_index);
        return true;
    }
    bool GetEntityName(int scene_index, uint32_t entity_int, std::string& out) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e) || !reg.all_of<cairns::Name>(e)) {
            return false;
        }
        out = reg.get<cairns::Name>(e).value;
        return true;
    }
    bool SetEntityCamera(int scene_index, uint32_t entity_int, float fov_y_rad,
                         float near_z, float far_z, bool is_main) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e)) {
            return false;
        }
        reg.emplace_or_replace<cairns::CameraComponent>(
            e, cairns::CameraComponent{fov_y_rad, near_z, far_z, is_main});
        return true;
    }
    bool GetEntityCamera(int scene_index, uint32_t entity_int, float& fov_y_rad,
                         float& near_z, float& far_z, bool& is_main) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e) || !reg.all_of<cairns::CameraComponent>(e)) {
            return false;
        }
        const cairns::CameraComponent& c = reg.get<cairns::CameraComponent>(e);
        fov_y_rad = c.fov_y_rad;
        near_z = c.near_z;
        far_z = c.far_z;
        is_main = c.is_main;
        return true;
    }
    // Empty entity (no Transform/Renderable): carrier for scene-scoped
    // components -- lights, effect stacks. Returns the entt id, or
    // UINT32_MAX on a bad scene index.
    uint32_t CreateEmptyEntity(int scene_index, const char* name) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return UINT32_MAX;
        }
        const entt::entity e = wc->registry.create();
        if (name != nullptr && name[0] != '\0') {
            wc->registry.emplace<cairns::Name>(e, cairns::Name{name});
        }
        return static_cast<uint32_t>(e);
    }
    bool SetEntityDirectionalLight(int scene_index, uint32_t entity_int,
                                   const cairns::DirectionalLight& light) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e)) {
            return false;
        }
        reg.emplace_or_replace<cairns::DirectionalLight>(e, light);
        return true;
    }
    bool GetEntityDirectionalLight(int scene_index, uint32_t entity_int,
                                   cairns::DirectionalLight& out) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e) || !reg.all_of<cairns::DirectionalLight>(e)) {
            return false;
        }
        out = reg.get<cairns::DirectionalLight>(e);
        return true;
    }
    bool SetEntityPostEffect(int scene_index, uint32_t entity_int,
                             const cairns::PostEffect& fx) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e)) {
            return false;
        }
        reg.emplace_or_replace<cairns::PostEffect>(e, fx);
        return true;
    }
    bool GetEntityPostEffect(int scene_index, uint32_t entity_int,
                             cairns::PostEffect& out) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e) || !reg.all_of<cairns::PostEffect>(e)) {
            return false;
        }
        out = reg.get<cairns::PostEffect>(e);
        return true;
    }
    // Data mutation, not a mode: flips every live material's shader family
    // (test/effect convenience; per-material authoring comes with material
    // ops). Returns the count touched.
    uint32_t SetMaterialShaderAll(cairns::ShaderKey key) {
        uint32_t n = 0;
        prefab_store_.materials.ForEachLive(
            [&](cairns::Material::Hot&, cairns::Material::Cold& cold) {
                cold.shader_key = key;
                ++n;
            });
        return n;
    }
    bool AddParticleEmitter(int scene_index, uint32_t entity_int) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e)) {
            return false;
        }
        if (!reg.all_of<cairns::ParticleEmitterComponent>(e)) {
            reg.emplace<cairns::ParticleEmitterComponent>(e);
        }
        MarkSceneDirty(scene_index);
        return true;
    }
    bool SetEntityRenderable(int scene_index, uint32_t entity_int,
                             uint32_t layer_mask, uint32_t flags) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e)) {
            return false;
        }
        reg.emplace_or_replace<cairns::Renderable>(
            e, cairns::Renderable{layer_mask, flags});
        MarkSceneDirty(scene_index);
        return true;
    }
    bool GetEntityRenderable(int scene_index, uint32_t entity_int,
                             uint32_t& layer_mask, uint32_t& flags) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e) || !reg.all_of<cairns::Renderable>(e)) {
            return false;
        }
        const cairns::Renderable& r = reg.get<cairns::Renderable>(e);
        layer_mask = r.layer_mask;
        flags = r.flags;
        return true;
    }

    // Deterministic sim-clock snapshot for cairns.time.get. Fixed
    // timestep (Fiedler): time = sim_frame_ * kFixedDt. Read-only -- never
    // ticks the clock.
    void TimeNow(double& time, double& dt, uint64_t& frame) {
        frame = sim_frame_;
        dt = cairns::kFixedDt;
        time = static_cast<double>(sim_frame_) * cairns::kFixedDt;
    }

    // Two parallel out-vectors (headless zips them) since the
    // control-facing CameraEntry POD isn't visible from engine.hpp.
    void ListCameras(int scene_index, std::vector<uint32_t>& out_entities,
                     std::vector<uint8_t>& out_is_main) {
        cairns::Scene::Cold* wc = EntitySceneCold(scene_index);
        if (!wc) {
            return;
        }
        auto view = wc->registry.view<const cairns::CameraComponent>();
        for (const entt::entity e : view) {
            out_entities.push_back(static_cast<uint32_t>(entt::to_integral(e)));
            out_is_main.push_back(
                view.get<const cairns::CameraComponent>(e).is_main ? 1u : 0u);
        }
    }
    // Records the camera entity on the viewport (resolution into the view
    // matrix is a later wiring; the field is the stable seam). Returns false
    // for an out-of-range viewport.
    bool SetViewportCameraEntity(int viewport, uint32_t entity_int) {
        if (viewport < 0 || viewport >= cairns::kNumViewports) {
            return false;
        }
        cairns::Viewport::Cold* vc =
            viewport_mgr_.pool.GetCold(viewport_mgr_.ids[viewport]);
        if (!vc) {
            return false;
        }
        vc->camera_entity = static_cast<entt::entity>(entity_int);
        return true;
    }

    uint32_t ClearActiveScene() {
        cairns::Scene::Cold* wc = scene_mgr_.pool.GetCold(scene_mgr_.active);
        if (!wc) {
            return 0;
        }
        auto& reg = wc->registry;
        const uint32_t n =
            static_cast<uint32_t>(reg.storage<entt::entity>().size());
        reg.clear();
        if (auto* wh = scene_mgr_.pool.GetHot(scene_mgr_.active)) {
            wh->dirty = true;
        }
        return n;
    }

    // Hot-reload a Prefab in place behind its stable PrefabId.
    // Re-parses the GLB at |path| as a NEW append-load via the regular
    // RuntimeLoadBatch flow, then SWAPS the new pool slot's Hot/Cold
    // INTO the old slot at |idx|. The old PrefabId handle (index +
    // generation) is preserved -- entities holding AssetRef remain
    // valid and pick up the new mesh on the next frame. Previously-
    // resident GPU resources go through the per-resource retire-frame
    // deferred-deletion ring, freed kFIF frames later when no
    // in-flight submission still binds them.
    bool ReloadPrefab(uint32_t idx, const std::filesystem::path& path);

    // Convenience: look up |path| in prefab_store_.glb_paths and reload the matching
    // prefab in place. Matches by full path OR by basename so callers
    // can pass "aatrox.glb" instead of the resolved absolute path that
    // prefab_store_.glb_paths stores.
    bool ReloadPrefabByPath(const std::filesystem::path& path) {
        const std::filesystem::path lookup_name = path.filename();
        for (uint32_t i = 0; i < prefab_store_.glb_paths.size(); ++i) {
            if (prefab_store_.glb_paths[i] == path ||
                prefab_store_.glb_paths[i].filename() == lookup_name) {
                return ReloadPrefab(i, prefab_store_.glb_paths[i]);
            }
        }
        return false;
    }

    // Shader / pipeline hot-reload with KEEP-LAST-GOOD. Attempts to
    // build a new pipeline using the same desc the matching init
    // function uses; on success DeferFrees the old handle through
    // the deferred-deletion ring and swaps the engine member to the
    // new one. On failure (missing .spv / compile error / null
    // result) the existing pipeline is LEFT UNTOUCHED so render
    // continues with the previous PSO -- a broken shader must never
    // take rendering down.
    //
    // Supported logical names: "anim_eval", "skin", "particle"
    // (the three compute kernels). Graphics PSOs (forward_lit,
    // imgui, unlit, outline) are not reloadable here yet.
    bool ReloadPipelineByName(const std::string& name);

    // Evict: the manifest's 'remove' verb over the WHOLE batch span --
    // drops every currently resident prefab, defer-frees its GPU
    // resources (textures, samplers, meshes' position/index/attr buffers,
    // materials' bind groups), releases pool slots, and resets every
    // state array touched by the manifest (prefab_store_.per_prefab_asset,
    // prefab_store_.glb_paths, prefab_store_.resident_textures,
    // prefab_store_.per_batch_shared_skin) plus the anim cursors so the
    // next load triggers a clean full-rebuild. Returns the number of
    // prefabs dropped.
    //
    // Caller contract: clear any entities referencing these prefabs
    // first via cairns.scene.clear. UnloadAllPrefabs does NOT clear
    // entities itself; cross-frame in-flight proxies that captured the
    // entity's prefab handle pre-clear would race with the descriptor
    // updates here, hanging the render thread on the next frame.
    // ClearActiveScene first, THEN UnloadAllPrefabs.
    uint32_t UnloadAllPrefabs();

    // Fly-camera input surface for main.cpp. Both no-op under CAIRNS_CAM_POSE
    // so a byte-gate dump can't be perturbed by an event that snuck through.

    // move_input.x = right(+) / left(-), .y = up(+) / down(-),
    // .z = forward(+) / back(-). Caller multiplies by dt + speed.
    void ApplyFlyMovement(const glm::vec3& move_input);

    void ApplyMouseLook(float dyaw, float dpitch) {
        if (viewport_mgr_.cam_pose_override) {
            return;
        }
        cairns::FlyController& fc =
            viewport_mgr_.pool.GetCold(viewport_mgr_.active)->fly;
        fc.yaw += dyaw;
        // Clamp pitch just inside +/-pi/2 so forward never becomes degenerate.
        constexpr float kPitchLimit = 1.55334f;
        fc.pitch = std::clamp(fc.pitch + dpitch, -kPitchLimit, kPitchLimit);
    }

    bool CamPoseOverridden() const { return viewport_mgr_.cam_pose_override; }

    // HUD stat injection seam. When set, the imgui HUD draws from this value
    // instead of live Timer accumulators so the captured screen is
    // byte-stable. The imgui-stability golden feeds cairns::HudStats::Mock()
    // (16.6 ms / 60 fps / flat graph).
    void SetInjectedHudStats(const cairns::HudStats& s) {
        injected_hud_stats_ = s;
    }
    void ClearInjectedHudStats() { injected_hud_stats_.reset(); }
    const std::optional<cairns::HudStats>& InjectedHudStats() const {
        return injected_hud_stats_;
    }

    // Runtime particle gate. Default off (EngineConfig::particles_enabled =
    // false) so captured frames stay "pipeline + clear + meshes" unless a
    // scenario opts in. The emitter is a per-scene component
    // (ParticleEmitterComponent) on the active scene, not an engine flag:
    // presence gates the global sim + draw;
    // Viewport::Cold::particles_enabled still filters per-viewport draw.
    void EnableParticles(bool on) {
        cairns::Scene::Cold* sc = scene_mgr_.pool.GetCold(scene_mgr_.active);
        if (!sc) {
            return;
        }
        auto& reg = sc->registry;
        if (on) {
            if (reg.view<cairns::ParticleEmitterComponent>().empty()) {
                reg.emplace<cairns::ParticleEmitterComponent>(reg.create());
            }
        } else {
            const auto v = reg.view<cairns::ParticleEmitterComponent>();
            const std::vector<entt::entity> doomed(v.begin(), v.end());
            for (entt::entity e : doomed) {
                reg.destroy(e);
            }
        }
    }
    bool ParticlesEnabled() { return AnyBoundSceneHasEmitter(); }
    // Install/clear the canonical composition -- the active viewport's color
    // fullscreen + its resolved depth in a bottom-right PIP. Data the
    // composite pass iterates, not an engine mode flag.
    void SetNestedGraphMode(bool on) {
        if (on) {
            const uint32_t vi =
                static_cast<uint32_t>(viewport_mgr_.active_index);
            viewport_mgr_.composition[0] = cairns::CompositionView{
                glm::vec4(0.0f, 0.0f, 1.0f, 1.0f), vi,
                cairns::CompositionView::Source::kColor};
            viewport_mgr_.composition[1] = cairns::CompositionView{
                glm::vec4(0.7f, 0.7f, 0.3f, 0.3f), vi,
                cairns::CompositionView::Source::kResolvedDepth};
            viewport_mgr_.composition_count = 2;
        } else {
            viewport_mgr_.composition_count = 0;
        }
    }
    // Particle sim + draw gate -- any active viewport's bound scene
    // (or the active scene) carrying a ParticleEmitterComponent.
    bool AnyBoundSceneHasEmitter() {
        if (cairns::Scene::Cold* sc =
                scene_mgr_.pool.GetCold(scene_mgr_.active)) {
            if (!sc->registry.view<cairns::ParticleEmitterComponent>()
                     .empty()) {
                return true;
            }
        }
        for (int v = 0; v < viewport_mgr_.active_count; ++v) {
            cairns::Viewport::Hot* vh =
                viewport_mgr_.pool.GetHot(viewport_mgr_.ids[v]);
            if (!vh) {
                continue;
            }
            if (cairns::Scene::Cold* sc =
                    scene_mgr_.pool.GetCold(vh->scene)) {
                if (!sc->registry.view<cairns::ParticleEmitterComponent>()
                         .empty()) {
                    return true;
                }
            }
        }
        return false;
    }

    // Imgui panel hook: the app (sdl-min) hands a callback that draws extra
    // imgui windows (the scenario launcher) into the HUD frame. Raw fn ptr +
    // ctx so the engine gains no singleton + no per-app coupling; the app
    // does any command dispatch itself, outside the render frame.
    // Surfaceless web app opts into the imgui HUD + panel (native windowed
    // gets it free via !surfaceless; cairns_serve leaves it false).
    void SetImguiEnabled(bool on) { imgui_enabled_ = on; }
    // The perf HUD ("cairns" window) is app-shell chrome, gated
    // separately from the app panel so a clean capture (scenario picker only)
    // can suppress it. Default on -- windowed/serve keep the HUD.
    void SetHudVisible(bool on) { hud_visible_ = on; }
    bool HudVisible() const { return hud_visible_; }
    void SetImguiPanel(void (*fn)(void*), void* ctx) {
        imgui_panel_fn_ = fn;
        imgui_panel_ctx_ = ctx;
    }

    // Render imgui into golden captures (the imgui-stability golden flips
    // this on). SetInjectedHudStats must be called alongside so the rendered
    // HUD numbers are byte-stable (HudStats::Mock() = 60 fps flat).
    void SetImguiInGolden(bool on) { imgui_in_golden_ = on; }
    bool ImguiInGolden() const { return imgui_in_golden_; }

    // Per-frame SIM determinism digest (golden mode only; 0 otherwise).
    // Stable run-to-run across independent Engine instances (advances frame-
    // to-frame with the sim clock). test_state_hash asserts run-to-run
    // equality.
    uint64_t LastSimHash() const { return last_sim_hash_; }
    // Per-frame RENDER digest -- the bytes EncodeDraws feeds the GPU.
    // Stable run-to-run => GPU input deterministic; a flake past this point
    // is GPU-execution nondeterminism.
    uint64_t LastRenderHash() const { return last_render_hash_; }

    // Read the currently-bound particle SSBO bytes (the deterministic-
    // particle golden's cross-platform buffer check). Returns false when
    // no parity buffer is bound.
    bool ReadParticleBuffer(std::vector<uint8_t>& out) {
        if (particles_.ssbo[particles_.latest_parity_out].IsNull()) {
            return false;
        }
        return rhi_.resources.ReadBackBuffer(
            rhi_.alloc, particles_.ssbo[particles_.latest_parity_out],
            ParticleSystem::kParticleCount * static_cast<uint32_t>(sizeof(Particle)), out);
    }

    // Per-frame draw / cull / vert counters. Populated at the end of
    // BuildMeshOpaqueDraws each frame. `culled` is 0: no frustum cull stage
    // exists for static meshes yet (frustum.hpp is only consumed by the
    // spec test); once one lands, fill `culled` and reduce `draw_calls` /
    // `verts_processed` accordingly.
    struct FrameStats {
        uint32_t draw_calls = 0;
        uint64_t verts_processed = 0;
        uint32_t culled = 0;       // Always 0 -- see note above.
        uint32_t submitted = 0;
        bool cull_stage_implemented = false;  // false => cull golden SKIPs.
    };
    bool LastFrameStats(FrameStats& out) const {
        out = last_frame_stats_;
        return true;
    }

    // ---- General scene/viewport primitives composed from JS (cairns.dispatch).
    // These replace the bespoke C++ test seams; the SCENARIO-specific choreography
    // (which glbs, how many viewports) lives in JS, not here.
    cairns::SceneId SceneByIndex(uint32_t index) const {
        return index == 1 ? scene_mgr_.secondary : scene_mgr_.primary;
    }
    // Retarget where subsequent InstantiatePrefab* spawn (0 primary, 1 secondary).
    void UseScene(uint32_t index) { scene_mgr_.active = SceneByIndex(index); }
    bool SetViewportScene(int vp, uint32_t index) {
        if (vp < 0 || vp >= viewport_mgr_.active_count) {
            return false;
        }
        if (cairns::Viewport::Hot* vh = viewport_mgr_.pool.GetHot(viewport_mgr_.ids[vp])) {
            vh->scene = SceneByIndex(index);
            vh->camera_dirty = true;
            return true;
        }
        return false;
    }
    bool SetViewportParticles(int vp, bool on) {
        if (vp < 0 || vp >= viewport_mgr_.active_count) {
            return false;
        }
        if (cairns::Viewport::Cold* vc = viewport_mgr_.pool.GetCold(viewport_mgr_.ids[vp])) {
            vc->particles_enabled = on;
            return true;
        }
        return false;
    }
    bool SetViewportCamera(int vp, const glm::vec3& pos, float yaw,
                           float pitch) {
        if (vp < 0 || vp >= viewport_mgr_.active_count) {
            return false;
        }
        if (cairns::Viewport::Cold* vc = viewport_mgr_.pool.GetCold(viewport_mgr_.ids[vp])) {
            vc->fly.position = pos;
            vc->fly.yaw = yaw;
            vc->fly.pitch = pitch;
        }
        if (cairns::Viewport::Hot* vh = viewport_mgr_.pool.GetHot(viewport_mgr_.ids[vp])) {
            vh->camera_dirty = true;
        }
        return true;
    }
    // Load + normalize-to-frame + center `instances` actors cycling over `glbs`,
    // spawned into the active scene. The general fit-place primitive.
    bool SpawnFitted(const std::vector<std::string>& glbs, uint32_t instances,
                     bool animated);
    bool AdvanceFrames(uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            if (!RenderHeadlessFrame()) {
                return false;
            }
        }
        return true;
    }

    // Per-Engine synthetic scene id counter -- a member, not a process
    // global, so it is deterministic per instance for the run-to-run hash.
    uint64_t NextSceneId() { return scene_mgr_.next_id++; }

    // Runtime viewport management: handle-pilled pool + vpN wire-name layer.
    int ActiveViewportCount() const { return viewport_mgr_.active_count; }

    // Returns the engine-assigned monotonic name counter ("vp{N}" without
    // the prefix) or UINT32_MAX if at kNumViewports cap. Names are never
    // reused for the lifetime of the engine process. The RPC layer formats
    // the result as "vp{N}" on the wire.
    uint32_t OpenViewport();

    // Close the highest-index viewport. Returns false if at 1 (cannot drop
    // below 1). Existing RPC takes no argument; this targets the last
    // opened viewport for backward compat with the protocol.
    bool CloseViewport();

    // Int-indexed slot-position API for in-engine callers; the wire layer
    // uses SetViewportLayoutByName.
    bool SetViewportLayout(int viewport, glm::vec4 rect) {
        if (viewport < 0 || viewport >= viewport_mgr_.active_count) {
            return false;
        }
        viewport_mgr_.pool.GetHot(viewport_mgr_.ids[viewport])->layout_rect = rect;
        return true;
    }

    // Wire-name layer. Parse "vp{N}" -> counter -> binary search the
    // sorted name table -> ViewportId. Returns Null on miss.
    cairns::ViewportId ResolveViewportName(uint32_t counter) const;

    int FindViewportSlot(cairns::ViewportId id) const {
        for (int i = 0; i < viewport_mgr_.active_count; ++i) {
            if (viewport_mgr_.ids[i].index == id.index &&
                viewport_mgr_.ids[i].generation == id.generation) {
                return i;
            }
        }
        return -1;
    }

    bool SetViewportLayoutByName(uint32_t name_counter, glm::vec4 rect) {
        cairns::ViewportId id = ResolveViewportName(name_counter);
        if (id.IsNull()) {
            return false;
        }
        viewport_mgr_.pool.GetHot(id)->layout_rect = rect;
        return true;
    }

    void SetActiveViewportSlot(int idx) {
        if (idx < 0 || idx >= viewport_mgr_.active_count) {
            return;
        }
        viewport_mgr_.active_index = idx;
        viewport_mgr_.active = viewport_mgr_.ids[idx];
    }

    // ===== Selection / highlight / pick. Document-side state -- the
    // selection set names "what the user (human or VLM) cares about right
    // now"; the highlight set names "what should glow". RequestPick only
    // records the click coordinate; a later tick resolves it
    // (ResolvePickRaycast) and delivers the entity via ConsumePickResult.

    const std::vector<cairns::SelectionTarget>& Selection() const { return picking_.selection; }
    const std::vector<cairns::SelectionTarget>& Highlights() const { return picking_.highlights; }
    uint32_t SelectionRevision() const { return picking_.selection_rev; }

    void ClearSelection() {
        if (!picking_.selection.empty()) {
            picking_.selection.clear();
            ++picking_.selection_rev;
        }
    }
    void SetSelection(std::vector<cairns::SelectionTarget>&& targets) {
        picking_.selection = std::move(targets);
        ++picking_.selection_rev;
    }
    void AddSelection(const cairns::SelectionTarget& t) {
        for (const auto& s : picking_.selection) {
            if (s == t) {
                return;
            }
        }
        picking_.selection.push_back(t);
        ++picking_.selection_rev;
    }
    void RemoveSelection(const cairns::SelectionTarget& t) {
        for (size_t i = 0; i < picking_.selection.size(); ++i) {
            if (picking_.selection[i] == t) {
                picking_.selection.erase(picking_.selection.begin() + static_cast<long>(i));
                ++picking_.selection_rev;
                return;
            }
        }
    }

    void ClearHighlights() {
        if (!picking_.highlights.empty()) {
            picking_.highlights.clear();
            ++picking_.highlights_rev;
        }
    }
    void SetHighlights(std::vector<cairns::SelectionTarget>&& targets) {
        picking_.highlights = std::move(targets);
        ++picking_.highlights_rev;
    }

    // Window-pixel coords. Records the request only; a later tick resolves
    // and delivers the entity.
    void RequestPick(int viewport, uint32_t x, uint32_t y) {
        picking_.pending = true;
        picking_.viewport = viewport;
        picking_.x = x;
        picking_.y = y;
    }
    bool PickPending() const { return picking_.pending; }
    int PendingPickViewport() const { return picking_.viewport; }
    uint32_t PendingPickX() const { return picking_.x; }
    uint32_t PendingPickY() const { return picking_.y; }

    // CPU ray-cast pick: unproject the click to a world ray, intersect every
    // entity's world AABB (the mesh bind-pose AABB transformed by WorldTransform),
    // nearest hit wins. Returns entt id + 1 (0 = clicked empty space, matching
    // the id-buffer convention). SYNCHRONOUS + identical on metal/vulkan/webgpu
    // -- no GPU id-buffer readback, which the browser cannot do synchronously.
    // The GPU id buffer stays only for the outline edge-detect, which is
    // GPU-side and already symmetric.
    //
    // TODO(picking-accel): O(entities) linear scan + first-mesh bind AABB only.
    // Add a BVH/grid (sub-linear) and union all meshes / use the live animated
    // AABB before the 3300-GLB rung. See TODO.md #picking-accel.
    uint32_t ResolvePickRaycast(int vp, uint32_t px, uint32_t py,
                                const glm::mat4& inv_view_proj);

    // Pick/selection state lives in PickSelection (engine/pick_selection.hpp);
    // these accessors operate on picking_.
    bool PickResolved() const { return picking_.resolved; }
    PickResult ConsumePickResult() {
        picking_.resolved = false;
        return picking_.last_result;
    }

    // Click-to-focus: caller passes the window-x of the LMB click. Engine
    // picks the half of the swap target the click lands in. fly_/keyboard
    // input is then routed to that viewport on subsequent iterates.
    void SetActiveViewportFromClickX(float window_x) {
        const float half = static_cast<float>(FrameWidth()) /
                            static_cast<float>(std::max(1, viewport_mgr_.active_count));
        SetActiveViewportSlot((window_x < half) ? 0 : 1);
    }
    int ActiveViewport() const { return viewport_mgr_.active_index; }

    // Override the deterministic-particles seed (default kept at 42 to match
    // the existing CAIRNS_DUMP byte-gate). Must be called before
    // GreaterInit's initParticles for the change to take effect.
    void SetRandomSeed(uint32_t seed) { particles_.random_seed = seed; }
    uint32_t GetRandomSeed() const { return particles_.random_seed; }

    uint32_t GetFinalTargetWidth() const { return present_.final_target_w; }
    uint32_t GetFinalTargetHeight() const { return present_.final_target_h; }

    // Current logical frame dims. Windowed: tracks the swapchain.
    // Surfaceless: tracks present_.final_target. Single source of truth for
    // aspect / screen_params / ImGui DPI scaling.
    uint32_t FrameWidth() const {
        return present_.final_target.IsNull() ? present_.swapchain.Width() : present_.final_target_w;
    }
    uint32_t FrameHeight() const {
        return present_.final_target.IsNull() ? present_.swapchain.Height() : present_.final_target_h;
    }

    // (Re)allocate per-viewport persistent R32U id targets if dims drift.
    // Called at the top of RecordFrame so each viewport's id_target_ matches
    // the current vp_w/vp_h. Destroys old targets through the rhi destroy
    // queue so any in-flight frame using the prior dims is unaffected.
    void EnsureIdTargets(uint32_t w, uint32_t h);

    // (Re)build the highlights texture from picking_.highlights. Called by
    // RecordFrame each frame; if the rev hasn't changed, no-op. On change,
    // destroys the prior texture through the rhi destroy queue and creates
    // a fresh 65x1 R32U with the new pack. Empty highlight set still
    // produces a valid texture (count=0) so the outline frag's descriptor
    // binding is always satisfied -- the early-out in id_in_highlights
    // keeps it cheap.
    void EnsureHighlightsTex();

    // Reallocate present_.final_target at the new dimensions. Surfaceless mode only.
    bool ResizeFinalTarget(uint32_t w, uint32_t h);

    // Surfaceless (cairns_serve) one-frame render: drives the windowed draw()
    // path once. Swap pass writes into present_.final_target; the engine builds a
    // SwapResolveTarget with no drawable so Frames neither acquires a
    // drawable nor presents one. Synchronous: render thread (if used)
    // drained before return; the metal/vulkan Frames::End waitUntilCompleted's
    // the render-to-texture path. Returns false if not surfaceless.
    bool RenderHeadlessFrame() {
        if (present_.final_target.IsNull()) {
            return false;
        }
        return draw();
    }

    // Test seam: surfaceless byte readback of the offscreen target. The
    // golden ladder hashes this buffer and compares to a per-platform ref.
    // Mirrors DumpFinalTarget's pipeline but skips the PNG encode.
    bool ReadFinalTargetRgba(std::vector<uint8_t>& rgba, uint32_t& w,
                              uint32_t& h) {
        if (present_.final_target.IsNull()) {
            return false;
        }
        return rhi_.resources.ReadBackTextureRgba(present_.final_target, rgba, w, h);
    }

#if CAIRNS_WEBGPU
    // Browser present support: the offscreen present_.final_target's native texture
    // (WGPUTexture as void*). The web entry copies it into the canvas surface
    // each frame, reusing the whole surfaceless render path. Null if unset.
    void* FinalTargetNativeTexture() {
        if (present_.final_target.IsNull()) {
            return nullptr;
        }
        rhi::Texture::Cold* cold = rhi_.resources.textures.GetCold(present_.final_target);
        return cold ? cold->api_image : nullptr;
    }
#endif

    // Headless texture readback: blit present_.final_target -> Shared buffer ->
    // PNG. Mirrors the windowed dump in metal/frames.cpp::End() but reads
    // from the offscreen target instead of the swapchain drawable. Apple
    // origin is top-left so no Y-flip needed (matches the windowed dump's
    // contract). BGRA -> RGBA swizzle on the host side.
    bool DumpFinalTarget(const std::filesystem::path& path) {
        if (present_.final_target.IsNull()) {
            return false;
        }
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        if (!rhi_.resources.ReadBackTextureRgba(present_.final_target, rgba, w, h)) {
            return false;
        }
        return stbi_write_png(path.string().c_str(), static_cast<int>(w),
                              static_cast<int>(h), 4, rgba.data(),
                              static_cast<int>(w * 4)) != 0;
    }
    
    bool initCpuAllocators();

    // Acquire the initial viewport slot (vp0) and pre-fill the layout /
    // active selection. Called from GreaterInit before any cam_pose
    // override walks the viewport pool. Idempotent: bails if viewport 0
    // is already live.
    void InitInitialViewport();
    
    // Re-seat a default-constructed (malloc-fallback) block-backed vector
    // onto cpu_block_ + reserve. POCMA makes the empty move-assign adopt
    // the block allocator. Call once at init, before first use.
    template <typename Vec>
    void ReseatOnBlock(Vec& v, size_t cap) {
        v = Vec(typename Vec::allocator_type(cpu_block_));
        v.reserve(cap);
    }

    bool initResourceManagers();
    
    bool GreaterInit(const rhi::InitConfig& cfg, const EngineConfig& ecfg);

    bool BuildMeshOpaqueDraws(uint32_t slot);

    // Render-side bump of all per-frame UBOs. Order matters: globals first,
    // per-draw (material, draw_tmp) in stable_idx order, fixed_dt last.
    // Writes s.globals_offset, s.drawList[*].dynamic_buffer_offsets[0..1],
    // s.dt_off. Compute kernel sees pkt.fixed_dt (constant sim dt), not wall.
    void EncodeDraws(const FramePacket& pkt);

    // Drain in-flight work, settle GPU, invalidate any caches keyed on the
    // old swap dims, then resize per-viewport / final_target state. Called
    // at the top of draw(); a no-op when no SDL resize is pending and the
    // observed swap dims haven't drifted (Vk's WSI may auto-recreate the
    // swapchain on OUT_OF_DATE without ever calling this path).
    void ApplyPendingResize();

    bool draw();

    // Pick the per-frame swap target. SwapChain and Frames are
    // app-mode-agnostic -- the engine is the one place that knows which
    // texture the swap pass writes into this frame.
    rhi::SwapResolveTarget AcquireFrameSwapTarget() {
        if (present_.final_target.IsNull()) {
            return present_.swapchain.AcquireForFrame();
        }
        return rhi_.resources.MakeSurfacelessSwapResolveTarget(
            present_.final_target, present_.final_target_w, present_.final_target_h);
    }

    // Render-thread entry point; today called synchronously from draw().
    // Owns: rhi_.frames.Begin/End, the bump-ring EncodeDraws, the compute +
    // render-pass encode. Reads pkt + slots_[pkt.slot].
    void RecordFrame(FramePacket& pkt);

    bool initRenderPipeline();

    struct Particle {
        float position[2];
        float velocity[2];
        float color[4];
    };

    // Opportunistic SkinnedAttachment factory. Resolves the
    // scene, picks the walking clip (`SelectWalkingClip`), finds the first
    // skinned mesh (cpuSkinAttrs non-empty), allocates a skinning_.output_pool
    // slice sized to that mesh's vertex count, and returns the new SkinId.
    // Null on any miss (no skins, no skinned mesh, no clips, no pool
    // capacity left). The actor's per-frame palette uses the stored
    // clip_index + time_offset and writes deformed verts at slice.offset.
    cairns::SkinId TryCreateSkinForScene(cairns::PrefabId scene_id,
                                          float time_offset);

    // Per-frame skin pipeline (game-thread side). Walks the active scene
    // for SkinRef entities, samples each actor's clip into a per-slot
    // palette slab on the arena, buckets visible actors by mesh
    // (flat-array prefix-sum, no map per standing rule), emits SkinBatchGpu
    // rows + flat InstanceMeta + flat palettes, publishes spans on s.pkt.
    // No SkinRef in the registry => skin_batches empty; the static path is
    // bit-for-bit unchanged.
    void BuildSkinFrame(uint32_t slot);

    void initAnimEvalKernel();

    // (Re)create the skinning_.dyn_anim_eval DynamicBuffers set using the
    // current anim buffer handles. Idempotent and safe to call before
    // skinning_.eval_tables_uploaded flips true (early-returns when buffers
    // don't exist yet). vk needs this set; metal ignores dyn_set_0 in
    // DispatchAnimEval.
    bool recreateAnimDynBindings();

    // Recreate skinning_.dyn_skin_group_b after the first anim-table upload
    // so binding 1 (palettes) captures skinning_.palette_out_buf. At
    // GreaterInit (empty boot) eval_tables_uploaded is false, so binding 1
    // falls back to the kDynamic master ring and the skin compute would
    // read palettes from the wrong buffer. Mirrors GreaterInit's gb[] table.
    bool recreateSkinGroupB();

    // Flatten every loaded scene's animation tables into the GPU buffers the
    // anim_eval kernel reads. Pose evaluation + palette build run on the GPU -- one
    // workgroup per actor over global flattened tables -- so per-frame skinning
    // stays off the CPU and shared data is addressed by offset, not per-object
    // bindings.
    //
    // WebGPU guarantees only 8 storage buffers per stage (Chrome caps at 10, and
    // there is no portable tier above that), so the read-only tables are folded
    // into 3 buffers grouped by element stride. The data is already
    // offset-addressed (every SceneHeader.*_off), so packing is just sharing one
    // buffer per stride class; the offsets become element offsets into the
    // packed buffer. The 6 buffers:
    //
    //   1  ae_i32 (int)      -- skeleton wiring + clip timestamps. Packs: parent[]
    //        (each node's parent index, for the world-compose walk), topo[] (the
    //        topological node order that walk follows), joint_nodes[] (joint ->
    //        node map used to gather the palette), times[] (keyframe timestamps,
    //        stored as float bit-patterns and bitcast back to float on read).
    //   2  ae_vec4 (vec4)    -- every vec4-stride animation value. Packs:
    //        bind_pose[] (each node's rest transform as 3 vec4 = translation /
    //        rotation-quaternion / scale), values[] (keyframe values: xyz for
    //        translate+scale, xyzw quaternion for rotate), inverse_binds[] (each
    //        joint's inverse bind matrix as 4 vec4 columns).
    //   3  ae_word16 (uvec4) -- the two 16-byte clip descriptors, one uvec4 each.
    //        Packs: channels[] (which node + path a curve drives, plus its sampler
    //        index) and samplers[] (a curve's keyframe range + interpolation mode).
    //   4  scene_headers     -- per-scene index: node/joint/channel/sampler counts,
    //        the *_off element offsets into buffers 1-3 for this scene, the mesh
    //        node, and the clip duration. headers[actor.scene_idx] tells each
    //        workgroup where its scene's slice lives. Read-only.
    //   5  world_scratch     -- RW scratch: the composed world-space matrix per
    //        node, per actor (stage 3 writes it walking topo, stage 4 reads it).
    //   6  palette_out       -- the result: inv(mesh_world) * world[joint] *
    //        inverse_bind, one mat4 per joint per actor. The skin kernel consumes
    //        it. (binding 0 is the per-frame ActorRecord UBO, not packed here.)
    void uploadAnimTablesGpu();

    // Best-effort load of the skin compute kernel. A failed load (missing
    // skin.comp.spv / skin.metal) leaves skinning_.skin_kernel Null; the
    // dispatch checks IsNull() and degenerates to "no skinning this frame",
    // preserving the static path bit-for-bit.
    void initSkinKernel();

    bool initParticles();

    // Particle SSBOs split out of initParticles so the parity
    // DynamicBuffers can grab their handles before the kernel pipeline
    // is built (pipeline layout is sourced from parity[0]).
    bool initParticleSsbos();

    bool deinit();
    
private:
    uint32_t frame_ = 0;

    // The single owning CPU memory block. All persistent + per-frame CPU
    // state is carved from here (1 GB desktop / 256 MB mobile, fail-loud).
    // Declared BEFORE every block-backed member (pools, loose vectors, entt
    // registry, prefab_arena_) so it is destroyed LAST -- those containers'
    // ChunkStdAllocator dtors deallocate into it at ~Engine teardown, so the
    // block (and its mmap-backed chunks) must outlive them.
    cairns::ChunkAllocator cpu_block_;
    // String-interning arena: persistent block-backed slab for the prefab
    // load tables (node/clip names, Node::children, Skin/Clip inner arrays).
    // NameRef + ArenaSlice store byte offsets into this slab, so the pool
    // structs stay pointer-free POD (raw-span-hashable). Monotonic (no
    // per-asset free yet; reload churn bump-leaks -- bounded). Slab is
    // carved from cpu_block_, so it sits right after it (destroyed first).
    cairns::BumpArena prefab_arena_{};

    // Loaded-prefab store: prefab/material/mesh pools + parallel id/path/asset
    // vectors + resident-texture + per-batch-skin lists + loader instruments.
    // cpu_block_ + prefab_arena_ stay on Engine (declared earlier) so they
    // outlive the store's pools + interned slices.
    cairns::PrefabStore prefab_store_;
    // Material create-or-reuse table (hash -> handle, sorted, binary-searched;
    // no maps). Re-seated onto cpu_block_ in initResourceManagers.
    std::vector<cairns::MaterialDedupEntry,
                cairns::ChunkStdAllocator<cairns::MaterialDedupEntry>>
        material_dedup_;
    // Standalone effect textures (paper grain, noise, TAM chains): name ->
    // handle, linear scan (a handful of entries), engine-owned -- no prefab
    // lifetime. Loaded via util/texture_loader.hpp.
    struct EffectTexture {
        const char* name = nullptr;
        rhi::Handle<rhi::Texture> tex;
    };
    std::vector<EffectTexture, cairns::ChunkStdAllocator<EffectTexture>>
        effect_textures_;
    // APPEND-only debug snapshot stashed between two NDJSON op calls
    // (cairns.debug.snapshotPrefabHandles -> cairns.debug.assertAppendOnly).
    // Empty until first snapshot call.
    std::vector<PrefabHandleSnapshot> last_handle_snapshot_;
    std::vector<int32_t> root_nodes_stack_cache_;


    // Skinning + animation GPU state (pool, output RangePool, kernels, folded
    // anim-table SSBOs, delta-upload cursors, dyn descriptor sets) grouped in
    // AnimSkinSystem.
    cairns::AnimSkinSystem skinning_;

    // Per-slot frame buffers (drawList / drawListSorted / proxies /
    // draw_world_matrices / pending_globals / globals_offset / dt_off /
    // FramePacket). See PerSlot above.
    // std::array (not std::vector): PerSlot holds std::atomic<bool>, which
    // is not move-constructible, so vector::resize would fail to compile;
    // kFramesInFlight is compile-time anyway.
    std::array<PerSlot, kFramesInFlight> slots_{};

    // Multithreaded build_draws pool. Taskflow-backed persistent
    // worker pool behind a pimpl (lives in cairns_render_thread). Workers
    // are parked on a condition_variable so the per-frame fan-out cost is
    // wake/notify, not pthread_create. Sized once at GreaterInit from
    // std::thread::hardware_concurrency().
    std::unique_ptr<cairns::WorkerPool> build_pool_;

    // EnTT scene-layer path. scene_mgr_.pool pre-reserved at startup
    // (kMaxScenes Acquire+Release cycle) to keep Scene::Cold* pointer
    // stable across real Acquire later.
    static constexpr uint32_t kMaxScenes = 8;
    // Scene pool + asset registry + proxy arrays + the active/primary/secondary
    // scene-id trio grouped in SceneManager.
    cairns::SceneManager scene_mgr_;

    // Handle-pilled Viewport pool. viewport_mgr_.pool owns Hot+Cold;
    // viewport_mgr_.ids[0..viewport_mgr_.active_count) carry the slot ordering
    // (preserves the [0..N) layout/indexing semantics the rest of the
    // engine uses to address PerSlot::pending_globals[], id_target_[], etc.).
    //
    // viewport_mgr_.active is the ViewportId of the focused viewport;
    // viewport_mgr_.active_index caches its position in viewport_mgr_.ids so
    // the PerSlot per-viewport arrays can still be indexed by int. Both
    // fields are updated together via setActiveViewport().
    //
    // kNumViewports is the compile-time cap on simultaneous viewports;
    // viewport_mgr_.active_count (runtime) tells the engine how many slots
    // are LIVE. Default = 1 (full-frame viewport 0). Grow via
    // cairns.viewport.open / shrink via cairns.viewport.close. Layout rects
    // on Viewport::Hot::layout_rect (NDC 0..1 over the swap pane) describe
    // where each live viewport tiles.
    cairns::ViewportManager viewport_mgr_;

    // Per-slot CPU arena capacity. 16 MiB covers the per-frame
    // palette/InstanceMeta/SkinMeshBatch arrays the skin pipeline parks
    // there. The arena lives on PerSlot (the slot IS the lock);
    // initialized in initCpuAllocators, Reset()'d at slot Acquire (render
    // thread already drained).
    static constexpr size_t kArenaBytesPerSlot = 16u * 1024u * 1024u;
    // Sized to the measured 100-GLB footprint (~58 MB; mostly all-clip
    // sampler/channel slices, which are load-scratch -- persisting only walk
    // clips would shrink this a lot). Platform-independent: the same GLBs
    // need the same space, so NOT budget-scaled. Oversize (> chunk) => a
    // dedicated malloc, not carved from the 256 MB mobile chunk reservation.
    static constexpr size_t kPrefabArenaBytes = 96u * 1024u * 1024u;

    rhi::Rhi rhi_;
    // Per-frame render graph. Reused via Reset() across frames (vector storage
    // for passes/textures is preserved). Constructed lazily on first RecordFrame
    // because Resources& / Allocator& must already be initialized.
    std::unique_ptr<rhi::RenderGraph> graph_;

    // Swap / present / final-target / resize state grouped in PresentTargets;
    // declared after rhi_ so its swapchain tears down before the device.
    cairns::PresentTargets present_;
    // shaders
    ShaderHandle unlit_offscreen_ = ShaderHandle::Null;
    // Id-less variant; selected when no consumer wants the R32U id
    // attachment this frame.
    ShaderHandle unlit_offscreen_noid_ = ShaderHandle::Null;
    // Lit (half-lambert directional) variants; EncodeDraws stamps them
    // per-draw when the draw's material shader_key == kLit.
    ShaderHandle lit_offscreen_ = ShaderHandle::Null;
    ShaderHandle lit_offscreen_noid_ = ShaderHandle::Null;
    // Directional shadow map: persistent fixed-size depth target (no resize
    // coupling), rendered by the shadow pass when a light casts; lit draws
    // sample it via the slot-3 bind group (nearest sampler, manual PCF).
    rhi::Handle<rhi::Texture> shadow_target_;
    rhi::Handle<rhi::Sampler> shadow_sampler_;
    rhi::Handle<rhi::BindGroup> shadow_bind_group_;
    ShaderHandle shadow_pso_ = ShaderHandle::Null;
    // DynamicBuffers for unlit set 0 (pass globals UBO) + set 2 (per-draw
    // drawtmp UBO). Created post-Frames::Init with backing = kDynamic
    // master. RecordFrame stamps them on MeshDrawList +
    // Draw::dynamic_buffers; recorder reads the per-FIF set from Hot.
    rhi::Handle<rhi::DynamicBuffers> dyn_globals_;
    rhi::Handle<rhi::DynamicBuffers> dyn_drawtmp_;
    // Particle parity DynamicBuffers. Index 0 binds particles_.ssbo[0] ->
    // binding 1 and particles_.ssbo[1] -> binding 2 (step_src=0). Index 1
    // swaps them (step_src=1). Binding 0 (UBO_DYN dt) backed by kDynamic
    // master; per-dispatch dyn offset = current dt_off.
    rhi::Handle<rhi::DynamicBuffers> dyn_particle_parity_[2];
    ShaderHandle composite_pip_ = ShaderHandle::Null;
    ShaderHandle depthviz_ = ShaderHandle::Null;
    ShaderHandle outline_pip_ = ShaderHandle::Null;
    // Kuwahara post-effect triplet (tensor -> tfm -> filter). Any Null
    // disables the chain honestly (mirrors shadow_pso_), so a backend
    // missing the shaders renders unfiltered instead of crashing.
    ShaderHandle kuwahara_tensor_pip_ = ShaderHandle::Null;
    ShaderHandle kuwahara_tfm_pip_ = ShaderHandle::Null;
    ShaderHandle kuwahara_filter_pip_ = ShaderHandle::Null;
    // Post-effect params: per-FIF dyn-UBO set over the kDynamic master
    // (dyn_globals_ shape, 64B blocks); DrawFullscreenParams binds it.
    rhi::Handle<rhi::DynamicBuffers> dyn_postfx_;
    rhi::Handle<rhi::Sampler> composite_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // Nearest sampler for outline's id_off binding -- R32_UINT can't be
    // linearly filtered (VUID-vkCmdDraw-magFilter-04553). Outline's color
    // binding is also nearest because the fullscreen tri samples color_off
    // at native res (texel-aligned), so linear vs nearest is identical.
    rhi::Handle<rhi::Sampler> outline_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // imgui
    rhi::Handle<rhi::Shader> imgui_ = rhi::Handle<rhi::Shader>::Null;
    rhi::Handle<rhi::Texture> imgui_font_ = rhi::Handle<rhi::Texture>::Null;
    rhi::Handle<rhi::Sampler> imgui_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // Particle state grouped in ParticleSystem -- kernel/shaders/SSBOs +
    // the cross-thread parity handshake (mutex/cv/counters) travel as
    // one unit. Engine's init/draw systems operate on it.
    cairns::ParticleSystem particles_;
    std::unique_ptr<cairns::RenderThread> render_thread_;


    // Persistent per-viewport R32U id targets (not transient graph
    // textures) so an end-of-frame id readback (vkCmdCopyImageToBuffer /
    // metal blit on a 1x1 region) can read the last-rendered id without
    // racing the transient pool's reuse. Sized (vp_w, vp_h); reallocated
    // lazily by EnsureIdTargets when dims drift.
    std::array<rhi::Handle<rhi::Texture>, kNumViewports> id_target_{};
    uint32_t id_target_w_ = 0;
    uint32_t id_target_h_ = 0;

    // Selection/highlight/pick document state + the pick request/result
    // handshake, grouped in PickSelection. The per-viewport id_target
    // render targets above stay on Engine (GPU resources, kNumViewports-coupled).
    cairns::PickSelection picking_;
    static constexpr uint32_t kMaxHighlights = 64;


    // Fiedler fixed-timestep accumulator state. Game-thread only -- never
    // touched by the render thread. clock_ is WallClock in live mode,
    // FixedClock under CAIRNS_DUMP.
    std::unique_ptr<cairns::FrameClock> clock_;
    double accumulator_ = 0.0;
    uint64_t sim_frame_ = 0;
    uint32_t sim_steps_this_frame_ = 0;
    // Last per-frame SIM determinism digest (FixedClock + static scene
    // => byte-identical every frame and run-to-run). Read by test_state_hash.
    uint64_t last_sim_hash_ = 0;
    // Last per-frame RENDER digest (the GPU-input bytes EncodeDraws
    // stamps: globals UBOs + bump offsets + per-draw model/entity + dt offset).
    uint64_t last_render_hash_ = 0;
    bool golden_ = false;
    // Subset of golden_: only true when dump_path != "" (the CLI byte-gate
    // path). Tests use use_fixed_clock => golden_=true, dump_and_exit_=false.
    bool dump_and_exit_ = false;
    // Optional override for HUD numbers (HudStats::Mock used by the imgui
    // overlay golden so the captured screen is byte-stable). When present,
    // the HUD draw path reads from this instead of HudFromTimer. Set via
    // SetInjectedHudStats from the golden test harness.
    std::optional<cairns::HudStats> injected_hud_stats_;
    // App-provided imgui panel (scenario launcher). nullptr = none.
    void (*imgui_panel_fn_)(void*) = nullptr;
    void* imgui_panel_ctx_ = nullptr;
    // Surfaceless web app opts into imgui (HUD + panel); cairns_serve doesn't.
    bool imgui_enabled_ = false;
    bool hud_visible_ = true;
    // Stamped at the end of BuildMeshOpaqueDraws every frame.
    FrameStats last_frame_stats_{};
#if CAIRNS_ALLOC_TRACE
    // Prev snapshot for the [STEADY-60] alloc receipt. A member, NOT a
    // function-local static -- global mutable state is banned. Trace only.
    cairns::alloc_count::Snapshot alloc_steady_prev_{};
#endif
    // Opt-in: render imgui into the golden capture.
    bool imgui_in_golden_ = false;
    // Shell-lowered startup options (env-vars are read shell-side and
    // populated here). Engine never reads std::getenv.
    EngineConfig engine_cfg_;
    // cpu frame-time history (wall-clock between draw() calls) for the imgui graph
    static constexpr int kCpuMsHistory = 128;
    float cpu_ms_history_[kCpuMsHistory] = {};
    int cpu_ms_head_ = 0;
    float slot_ms_cache_[cairns::Timer::kMaxSlots] = {};
    float cpu_ms_last_ = 0.0f;
    uint64_t cpu_last_frame_ns_ = 0;
    // render pass
    static constexpr size_t sampleCount = 4;
};

} // namespace cairns

