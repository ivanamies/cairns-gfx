#pragma once

#include "util/define.hpp"
#include "util/alloc_count.hpp"  // #229 per-phase allocation receipts
#include "util/memory_budget.hpp"  // #229 single source of reservation sizes
#include "engine/engine_config.hpp"  // EngineConfig (split out; shell includes it directly)
#include "engine/particle_system.hpp"  // ParticleSystem state (C2 S3)
#include "engine/pick_selection.hpp"  // PickSelection + PickResult (C2 S6)
#include "engine/scene_manager.hpp"  // SceneManager state (C2 S4)
#include "engine/anim_skin_system.hpp"  // AnimSkinSystem state (C2 S2)
#include "engine/viewport_manager.hpp"  // ViewportManager + kNumViewports (C2 S5)
#include "engine/prefab_store.hpp"  // PrefabStore state (C2 S1)
#include "engine/present_targets.hpp"  // PresentTargets state (C2 S7)

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>  // #231 std::memcpy for SSBO-pack reinterprets
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
#include "util/cpu_pool.hpp"  // #221 Phase 3: RangePool for skinning_.output_pool.
#include "util/chunk_allocator.hpp"  // #229 M0b: the one owning CPU block.
#include "util/fnv1a.hpp"  // #229 M0b: per-frame determinism hash.
#include "util/device_caps.hpp"  // boot-invariant + HUD/skin fit predicates
#include "util/hud_stats.hpp"
#include "util/animation_runtime.hpp"  // #221 Phase 9: SelectWalkingClip + sampler.
#include "render/render_proxy.hpp"  // #221 Phase 3: SkinnedAttachment Hot/Cold.
#include "render/particle_emitter.hpp"  // A.1: ParticleRng (portable mt19937)

#include "gfx_api.hpp"
#include "rhi/init_config.hpp"
#include "rhi/swap_chain.hpp"
#include "util/misc.hpp"
#include "util/render_pass_globals.hpp"
#include "util/offset_allocator.hpp"
#include "util/gltf_loader.hpp"
#include "util/debug_asset.hpp"
#include "util/load_trace.hpp"  // #224 L3: LoadTrace / LoaderCounters PODs.
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
#include "scene/world.hpp"
#include "scene/viewport.hpp"
#include "scene/selection.hpp"
#include "render/frame_packet.hpp"
#include "render/render_extract.hpp"
#include "render/scene_draw_ranges.hpp"  // #195 CarveSceneDrawRanges (spec-tested)
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
    // #220 Step 1: MatId is now a generational Handle into Engine::prefab_store_.materials
    // (cairns::ResourceManager<Material>). Stale slots fail safe at
    // GetHot/GetCold instead of silently aliasing a recycled bind group.
    using MatId = cairns::Handle<cairns::Material>;
    using SamplerHandle = rhi::Handle<rhi::Sampler>;
    using BindGroupId = uint32_t;

    // #222 Phase T.1: kFramesInFlight has one home -- cairns::rhi::kFramesInFlight
    // in rhi/resource_manager.hpp. This re-export keeps existing
    // Engine::kFramesInFlight call sites compiling.
    static constexpr uint32_t kFramesInFlight = cairns::rhi::kFramesInFlight;

    // #222 Phase 0.2: caps for the GPU anim_eval kernel + its persistent
    // buffers. KEEP IN SYNC with assets/anim_eval.comp.glsl (records[1024],
    // kMaxNodesPerScene, kMaxJointsPerSkin) and assets/anim_eval.metal.
    static constexpr uint32_t kAnimActorsCap = 1024u;
    static constexpr uint32_t kAnimMaxNodes = 256u;
    static constexpr uint32_t kAnimMaxJoints = 256u;

    // Per-slot storage. drawList / drawListSorted / proxies / resident_textures
    // / draw_world_matrices live here so the game thread can fill slot S while
    // the render thread reads slot ~S. Capacity grows on demand; .clear()/
    // .resize() preserve buffers across frame reuse. pending_globals etc. are
    // staged by Build and consumed by EncodeDraws.
    static constexpr int kNumViewportsPerSlot = 4;  // #194 matches kNumViewports
    struct PerSlot {
        cairns::RenderProxyArrays proxies;
        // #219 Chunk A: these five per-frame arrays now ride the slot's
        // BumpArena. Producer (BuildMeshOpaqueDraws / RecordFrame's resident
        // texture gather) counts first, then arena.AllocateArray, then fills
        // by index. Lifetime: valid from arena.Reset() at slot Acquire
        // through render thread's Submit + completion of this slot's frame.
        // Next Acquire on the same slot resets and reuses the bytes.
        std::span<cairns::Draw> drawList;
        std::span<std::pair<cairns::DrawKey, uint32_t>> drawListSorted;
        // #222 Phase H.6: resident_textures lives on Engine, not PerSlot.
        std::span<glm::mat4> draw_world_matrices;
        // #207 parallel to draw_world_matrices; baked from MeshProxy::entity_id
        // by BuildMeshOpaqueDraws so unlit.frag can write the per-fragment id.
        std::span<uint32_t> draw_entity_ids;
        // #195 multi-scene fan-out: every distinct scene any viewport binds is
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
        // #210 per-slot CPU bump arena. SLOT IS THE LOCK -- render thread
        // sees this slot's arena exclusively during RecordFrame; main
        // thread resets at slot Acquire (already blocked on render
        // exclusivity). No mutex, no shared ptr.
        cairns::BumpArena arena{};  // #229 M0b: slab from cpu_block_ (kRegionFrame).
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

    bool RequestViewportDump(const std::filesystem::path& path) {
        rhi_.frame_capture.SetDumpPath(path);
        return true;
    }

    // #269: spawn one hero entity in scene_mgr_.active from a pre-loaded
    // scene. Returns the new entt entity id (0 on failure: bad
    // scene_idx, no active scene, prefab_store_.prefab_ids not populated, etc.).
    // Caller supplies the full world transform; rendered immediately
    // next frame. SkinRef is opportunistically attached via
    // TryCreateSkinForScene (so [SKIN-FAIL] logs cover the failure modes).
    uint32_t InstantiatePrefab(uint32_t scene_idx, const glm::mat4& world,
                        float time_phase) {
        return InstantiatePrefabImpl(scene_idx, world, time_phase,
                                      /*attach_skin=*/true);
    }

    // A.11: no-skin spawn variant. Skips TryCreateSkinForScene so the
    // entity renders in its bind pose without animation. Used by L5
    // "static LoL" rung so it diverges visibly from L6 "anim LoL".
    uint32_t InstantiatePrefabNoSkin(uint32_t scene_idx,
                                     const glm::mat4& world) {
        return InstantiatePrefabImpl(scene_idx, world, /*time_phase=*/0.0f,
                                      /*attach_skin=*/false);
    }

    // Shared implementation. Public because both wrappers are inline.
    uint32_t InstantiatePrefabImpl(uint32_t scene_idx, const glm::mat4& world,
                                    float time_phase, bool attach_skin);

    // #269: how many scenes (GLBs) loaded; clients call InstantiatePrefab with
    // scene_idx in [0, NumPrefabs()). Lets the NDJSON op validate args.
    uint32_t NumPrefabs() const {
        return static_cast<uint32_t>(prefab_store_.prefab_ids.size());
    }

    // #224 L1: resolve the shared skin attrs buffer for a mesh from its
    // batch_id. Null on out-of-range or unset (= unskinned mesh).
    rhi::Handle<rhi::Buffer> ResolvedSharedSkin(const cairns::Mesh::Hot& mhot) const {
        if (mhot.batch_id >= prefab_store_.per_batch_shared_skin.size()) {
            return rhi::Handle<rhi::Buffer>::Null;
        }
        return prefab_store_.per_batch_shared_skin[mhot.batch_id];
    }

    // #224 L2: validate a parsed Prefab against engine caps. Returns
    // true iff no errors. `report` accumulates issues across many calls
    // (the caller resets between batches). Header-only; static so
    // engine_headless.cpp / the validate op can call directly.
    static bool ValidatePrefab(const cairns::Prefab::Cold& cold,
                                cairns::ValidationReport& report,
                                uint32_t prefab_idx = UINT32_MAX) {
        const uint8_t pre_errors = report.issue_count;
        const uint32_t node_count =
            static_cast<uint32_t>(cold.nodes.size());
        if (node_count > kAnimMaxNodes) {
            report.Add(cairns::ValidationSeverity::kError,
                        "node_count > kAnimMaxNodes (256)", prefab_idx);
        }
        uint32_t max_joints = 0;
        for (const cairns::Skin& s : cold.skins) {
            const uint32_t jc = static_cast<uint32_t>(s.jointNodes.size());
            if (jc > max_joints) {
                max_joints = jc;
            }
        }
        if (max_joints > kAnimMaxJoints) {
            report.Add(cairns::ValidationSeverity::kError,
                        "max_joints > kAnimMaxJoints (256)", prefab_idx);
        }
        // Per-mesh weight-sum check requires Mesh::Cold (pre-CleanupTmps)
        // -- runs in LoadPrefabBatch via ValidateMeshWeights below since
        // mesh data is owned by the engine pool, not by Prefab::Cold.
        return report.issue_count == pre_errors;
    }

    // #224 L2: per-mesh weight-sum check. Called in LoadPrefabBatch
    // before CleanupTmps clears cpuSkinAttrs.
    bool ValidateMeshWeights(const cairns::Mesh::Cold& mc,
                              cairns::ValidationReport& report,
                              uint32_t prefab_idx = UINT32_MAX) {
        const uint8_t pre_errors = report.issue_count;
        uint32_t bad_vertices = 0;
        for (const cairns::SkinVertex& sv : mc.cpuSkinAttrs) {
            const float sum = sv.weights.x + sv.weights.y +
                              sv.weights.z + sv.weights.w;
            if (sum < 0.999f || sum > 1.001f) {
                ++bad_vertices;
            }
        }
        if (bad_vertices > 0) {
            // One warning per mesh (not per vertex) to bound issues[].
            report.Add(cairns::ValidationSeverity::kWarning,
                        "weight_sum != 1 on at least one skinned vertex",
                        prefab_idx);
        }
        return report.issue_count == pre_errors;
    }

    // #224 L1: append one batch of GLBs to the live Prefab / Mesh pools.
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
            std::span<const std::filesystem::path> glbs) {
        // Phase D: bracket every load with a printf + an Instruments
        // signpost so the time profiler distinguishes load work from
        // steady-state frames.
        const char* first_path =
            glbs.empty() ? "<empty>" : glbs.front().filename().c_str();
        CAIRNS_PRINT_ERR("[LOAD] begin batch n=%zu first=%s\n",
                          glbs.size(), first_path);
        CAIRNS_SIGNPOST_INTERVAL_SCOPED("load_prefab_batch", first_path);
#if CAIRNS_ALLOC_TRACE
        const cairns::alloc_count::Snapshot alloc_load_begin =
            cairns::alloc_count::Now();
#endif

        LoadPrefabBatchResult r{};
        r.first_prefab_idx = static_cast<uint32_t>(prefab_store_.prefab_ids.size());

        // #224 L3: per-stage timing. steady_clock so the trace numbers
        // are wall-clock; the byte-gate doesn't reference them.
        using Clock = std::chrono::steady_clock;
        const auto t_total = Clock::now();
        cairns::LoadTrace trace{};

        // ── parse + validate + prepare resources per glb (no GPU upload yet) ──
        const auto t_parse = Clock::now();
        cairns::ValidationReport vreport{};
        for (const std::filesystem::path& p : glbs) {
            cairns::PrefabId sid = prefab_store_.prefabs.Acquire();
            cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(sid);
            cairns::Prefab::Cold* scold = prefab_store_.prefabs.GetCold(sid);
            if (!shot || !scold ||
                !cairns::LoadPrefabFromGltf(p, *shot, *scold, prefab_store_.meshes, cpu_block_,
                                            prefab_arena_)) {
                CAIRNS_PRINT_ERR("[LoadPrefabBatch] parse failed: %s\n",
                                  p.string().c_str());
                prefab_store_.prefabs.Release(sid);
                continue;
            }
            // #224 L2: validate against engine caps before upload.
            const uint32_t prefab_idx_for_log =
                static_cast<uint32_t>(prefab_store_.prefab_ids.size());
            if (!ValidatePrefab(*scold, vreport, prefab_idx_for_log)) {
                CAIRNS_PRINT_ERR(
                    "[LoadPrefabBatch] validation failed for %s -- "
                    "skipping prefab.\n", p.string().c_str());
                prefab_store_.prefabs.Release(sid);
                continue;
            }
            cairns::PreparePrefabResources(*shot, *scold, rhi_.resources,
                                            rhi_.alloc, prefab_store_.materials);
            prefab_store_.prefab_ids.push_back(sid);
            ++r.count;
        }
        trace.Add("parse_gltf",
                   std::chrono::duration<double, std::milli>(
                       Clock::now() - t_parse).count(),
                   0, r.count);
        if (r.count == 0) {
            trace.total_ms = std::chrono::duration<double, std::milli>(
                Clock::now() - t_total).count();
            prefab_store_.last_load_trace = trace;
            return r;
        }

        // ── upload the new batch's prefabs to NEW kDefault buffers ──
        const auto t_upload = Clock::now();
        rhi::Handle<rhi::Buffer> batch_shared_skin =
            rhi::Handle<rhi::Buffer>::Null;
        std::span<const cairns::PrefabId> new_span(
            prefab_store_.prefab_ids.data() + r.first_prefab_idx, r.count);
        if (!cairns::rhi::LoadPrefabsGpu(new_span, prefab_store_.prefabs, prefab_store_.meshes,
                                          rhi_.resources, rhi_.alloc,
                                          &batch_shared_skin)) {
            CAIRNS_PRINT_ERR(
                "[LoadPrefabBatch] LoadPrefabsGpu failed for %u prefabs\n",
                r.count);
            trace.total_ms = std::chrono::duration<double, std::milli>(
                Clock::now() - t_total).count();
            prefab_store_.last_load_trace = trace;
            return r;
        }
        trace.Add("upload_kdefault",
                   std::chrono::duration<double, std::milli>(
                       Clock::now() - t_upload).count(),
                   0, r.count);

        // ══════════════════════════════════════════════════════════════
        // #228 H0: THE MANIFEST. The runtime/post-upload state-agreement
        // transformation, written as an explicit ordered list of named
        // one-liners. Adding engine state that depends on prefabs =
        // add a line here AND its matching invariant in
        // CheckPrefabStateInvariants (H2). There is no other site.
        // A forgotten member is a visible hole in this list, not a
        // silent fallback discovered overnight.
        // ══════════════════════════════════════════════════════════════
        uint32_t batch_mesh_count = 0;
        StampBatchSkinAndMeshIds(new_span, batch_shared_skin,
                                  batch_mesh_count);

        const auto t_group_a = std::chrono::steady_clock::now();
        BuildGroupABindGroups(new_span, batch_shared_skin);
        trace.Add("skin_group_a",
                   std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t_group_a).count(),
                   0, r.count);

        const auto t_cleanup = std::chrono::steady_clock::now();
        ValidateAndCleanupTmps(new_span, r.first_prefab_idx, vreport);
        trace.Add("cleanup_tmps",
                   std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t_cleanup).count());

        BuildMaterialSet2();           // span-independent (idempotent)
        BuildResidentTextures(new_span);
        StampPerPrefabAsset(new_span);
        AppendGlbPaths(new_span, glbs);  // [PICK] log
        // AppendAnimTables(new_span)  -- H4 (Aaltonen delta, replaces
        //                                  RuntimeLoadBatch's full
        //                                  uploadAnimTablesGpu re-call)

        // ── finalize trace + bump counters ──
        trace.total_ms = std::chrono::duration<double, std::milli>(
            Clock::now() - t_total).count();
        trace.prefabs_added = r.count;
        trace.meshes_added = batch_mesh_count;
        prefab_store_.last_load_trace = trace;
        prefab_store_.last_validation_report = vreport;
        ++prefab_store_.loader_counters.batches_loaded;
        prefab_store_.loader_counters.prefabs_resident += r.count;
        prefab_store_.loader_counters.meshes_resident += batch_mesh_count;
        prefab_store_.loader_counters.last_batch_ms = trace.total_ms;
        if (trace.total_ms > prefab_store_.loader_counters.peak_batch_ms) {
            prefab_store_.loader_counters.peak_batch_ms = trace.total_ms;
        }
        // Phase D close: end-of-batch marker. Pair with [LOAD] begin
        // so log scanning can compute per-batch wall time without
        // hunting for the LoadTrace summary.
        CAIRNS_PRINT_ERR("[LOAD] end batch ms=%.3f count=%u\n",
                          trace.total_ms, r.count);
        // #229: arena/block high-water -- visible in logcat so the S22's 256 MB
        // budget headroom is observable. prefab_arena (names/children/skin/clip
        // slices) and the cpu_block_ in-class total (pools + prefab tables +
        // entt; mesh cpu* are malloc, not here).
        CAIRNS_PRINT_ERR("[PREFAB-ARENA] used=%zu KiB / %zu KiB cap\n",
                          prefab_arena_.Used() / 1024,
                          prefab_arena_.Capacity() / 1024);
        CAIRNS_PRINT_ERR("[CPU-BLOCK] in_use=%llu KiB / %llu KiB budget\n",
                          (unsigned long long)(cpu_block_.BytesInUse() / 1024),
                          (unsigned long long)(
                              cairns::MemoryBudget::Default().cpu_persistent_bytes
                              / 1024));
#if CAIRNS_ALLOC_TRACE
        cairns::alloc_count::PrintDelta("[LOAD]", alloc_load_begin);
#endif
        return r;
    }

    // #224 L5: runtime entry point for cairns.prefab.loadBatch.
    //   (1) Device::WaitIdle so no in-flight frame reads pools being mutated
    //   (2) LoadPrefabBatch -- the actual parse + upload + Group A
    //   (3) re-upload anim tables (flat array; rebuilds with new prefabs)
    // Returns LoadPrefabBatchResult exactly like LoadPrefabBatch.
    LoadPrefabBatchResult RuntimeLoadBatch(
            std::span<const std::filesystem::path> glbs) {
        rhi_.device.WaitIdle();
        LoadPrefabBatchResult r = LoadPrefabBatch(glbs);
        if (r.count > 0) {
            // anim tables are a flat per-prefab GPU array; re-flatten +
            // re-upload picks up the new prefabs. Cost scales with total
            // prefabs (not batch); cheap relative to parse.
            uploadAnimTablesGpu();
            // Backfill any SkinId records that pre-dated the new headers.
            skinning_.skins.ForEachLive(
                [&](cairns::SkinnedAttachment::Hot& h,
                    cairns::SkinnedAttachment::Cold& c) {
                    if (cairns::Prefab::Hot* sht = prefab_store_.prefabs.GetHot(c.scene)) {
                        h.gpu_prefab_header_idx = sht->gpu_prefab_header_idx;
                    }
                });
        }
        return r;
    }

    // ══════════════════════════════════════════════════════════════════
    // #228 H2: the manifest's CONTRACT. One invariant per manifest line.
    // POLICY: a manifest line without its matching assert here fails
    // review. The two lists have the same length by construction.
    //
    // Returns the number of violations and (if `out_msgs` non-null)
    // appends a description for each violation. 0 == contract held.
    //
    // Debug-only in spirit (release builds don't pay for the O(n)
    // walks), but exposed via cairns.debug.checkInvariants so the
    // sequence harness can assert it after every load/instantiate.
    // ══════════════════════════════════════════════════════════════════
    uint32_t CheckPrefabStateInvariants(
            std::vector<std::string>* out_msgs = nullptr);

    // ══════════════════════════════════════════════════════════════════
    // #228 H0: the manifest's helper bodies. Each is span-scoped (or
    // span-independent + idempotent for AcquireSceneCells-style ones)
    // and does what an old GreaterInit post-load block did once over
    // all prefabs -- now invoked per batch.
    //
    // RULE OF THE MANIFEST: every helper here has a matching invariant
    // in CheckPrefabStateInvariants (H2). Adding a helper without its
    // invariant fails review.
    // ══════════════════════════════════════════════════════════════════

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
    // (initRenderPipeline also calls this once at boot; with L9's empty
    // boot it iterates zero materials and was stranded -- L10b moved
    // the loop here.)
    void BuildMaterialSet2() {
        prefab_store_.materials.ForEachLive(
            [&](cairns::Material::Hot& hot,
                cairns::Material::Cold& cold) {
                if (!hot.set2.IsNull()) {
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

    // Append the new prefabs' textureHandles to prefab_store_.resident_textures so
    // DrawMeshes' bindless sampler array can index them. APPEND-only;
    // existing entries' indices unchanged (L6 contract).
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

    // Append the new batch's GLB paths to prefab_store_.glb_paths so the [PICK] log
    // can name a clicked hero by filename. (Mole #6: this used to run
    // only in GreaterInit; runtime-loaded heroes logged "" until now.)
    // The order matches the prefab_store_.prefab_ids append order (new_span loops
    // forward); only paths that successfully Acquired a prefab slot
    // appear -- the input `glbs` may be longer if parses failed, so we
    // slice to new_span.size().
    void AppendGlbPaths(std::span<const cairns::PrefabId> new_span,
                         std::span<const std::filesystem::path> glbs) {
        // Skipped/failed parses don't append a prefab_id, so the input
        // glbs and the resulting new_span can disagree in length. Walk
        // glbs in order, appending only the ones we know succeeded by
        // iterating new_span in lockstep.
        prefab_store_.glb_paths.reserve(prefab_store_.glb_paths.size() + new_span.size());
        for (size_t i = 0; i < new_span.size() && i < glbs.size(); ++i) {
            prefab_store_.glb_paths.push_back(glbs[i]);
        }
    }

    // Register each new prefab as an AssetId so InstantiatePrefab can
    // resolve prefab_store_.per_prefab_asset[prefab_idx] without a bounds-check fail.
    // The InstantiatePrefab bounds check at engine.hpp:223 indexes this
    // array; a forgotten append here is the L10 silent failure that
    // returned UINT32_MAX / entity:0.
    void StampPerPrefabAsset(std::span<const cairns::PrefabId> new_span);

    // #224 L5: convenience -- pick `count` GLB paths from the static
    // kDebugGlbs window starting at `cursor`. Returns the resolved paths.
    std::vector<std::filesystem::path> ResolveDebugGlbPaths(
            uint32_t cursor, uint32_t count) {
        std::vector<std::filesystem::path> out;
        out.reserve(count);
        const uint32_t start =
            cairns::kDebugGlbsToParseStart + cursor;
        const uint32_t end_excl = std::min<uint32_t>(
            start + count,
            cairns::kDebugGlbsToParseStart + cairns::kDebugGlbsToParse);
        for (uint32_t i = start; i < end_excl; ++i) {
            std::filesystem::path p;
            if (cairns::GetStaticResourceFilepath(cairns::kDebugGlbs[i], p)) {
                out.push_back(p);
            }
        }
        return out;
    }

    // #224 L5: snapshot per-prefab extents for a span -- the
    // FitGridToViewport per_actor_extents input.
    std::vector<float> PrefabExtentSnapshot(uint32_t first_idx,
                                             uint32_t count) {
        std::vector<float> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            out.push_back(PrefabExtentMax(first_idx + i));
        }
        return out;
    }

    // #224 L4: fit N actors into a grid that fills the active viewport.
    // `per_actor_extents[i]` is each actor's bind-pose max-axis extent
    // (PrefabExtentMax(...) of the actor's source prefab). Returns one
    // world matrix per actor: translation = grid cell at z=-4, scale =
    // uniform cell_size / extent so heterogeneous prefabs occupy the
    // same on-screen footprint. Pure function: deterministic for
    // identical inputs.
    std::vector<glm::mat4> FitGridToViewport(
            uint32_t n_total,
            std::span<const float> per_actor_extents);

    // #224 L6: APPEND-only acceptance test.
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
    std::vector<PrefabHandleSnapshot> SnapshotPrefabHandles() {
        std::vector<PrefabHandleSnapshot> out;
        for (uint32_t pi = 0;
             pi < static_cast<uint32_t>(prefab_store_.prefab_ids.size()); ++pi) {
            cairns::Prefab::Hot* shot = prefab_store_.prefabs.GetHot(prefab_store_.prefab_ids[pi]);
            if (!shot) {
                continue;
            }
            for (uint32_t mi = 0;
                 mi < static_cast<uint32_t>(shot->meshes.size()); ++mi) {
                cairns::Mesh::Hot* mhot = prefab_store_.meshes.GetHot(shot->meshes[mi]);
                if (!mhot) {
                    continue;
                }
                PrefabHandleSnapshot s{};
                s.prefab_idx = pi;
                s.mesh_idx = mi;
                s.pos_idx  = mhot->posHandle.index;
                s.pos_gen  = mhot->posHandle.generation;
                s.attr_idx = mhot->attrHandle.index;
                s.attr_gen = mhot->attrHandle.generation;
                s.idx_idx  = mhot->indexHandle.index;
                s.idx_gen  = mhot->indexHandle.generation;
                s.batch_id = mhot->batch_id;
                s.global_base_vertex = mhot->global_base_vertex;
                out.push_back(s);
            }
        }
        return out;
    }

    // For each row in `prior`, look up the same (prefab_idx, mesh_idx) in
    // the current pool and compare every field. Returns the count of
    // mismatched rows; 0 == append-only contract held.
    uint32_t CountAppendOnlyMismatches(
            std::span<const PrefabHandleSnapshot> prior);

    // #224 L3: instrument accessors.
    const cairns::LoadTrace& LastLoadTrace() const { return prefab_store_.last_load_trace; }
    const cairns::ValidationReport& LastValidationReport() const {
        return prefab_store_.last_validation_report;
    }
    // #224 L8: editor-chrome toggle for cairns.editor.chrome op.
    bool EditorChromeEnabled() const { return editor_chrome_enabled_; }
    void SetEditorChromeEnabled(bool on) { editor_chrome_enabled_ = on; }

    // #224 L6: snapshot/assert helpers exposed to the NDJSON debug ops.
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

    // #269: bind-pose extent (max axis component of aabb_max - aabb_min)
    // of the scene's first skinned mesh -- the unit a caller normalizes
    // to when picking per-actor scale so heroes occupy a uniform cell
    // on screen. Returns 0 if scene_idx is out of range, no mesh has a
    // bind-pose AABB, or the AABB is degenerate.
    float PrefabExtentMax(uint32_t scene_idx);

    // Bind-pose AABB center of the prefab's first mesh, in mesh-local space.
    // Champions have their origin at the feet, so a fit that anchors the
    // origin pushes the body out the top of frame; subtract this to center.
    glm::vec3 PrefabAabbCenter(uint32_t scene_idx);

    // #269: list every live entity in scene_mgr_.active's registry. The
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
        for (const entt::entity e : reg.view<cairns::WorldTransform>()) {
            out.push_back(static_cast<uint32_t>(entt::to_integral(e)));
        }
        return out;
    }

    // #269: overwrite an entity's WorldTransform. Used by the spawn-
    // relayout path so existing actors slide to new grid cells without
    // the visible empty-then-full flash a clear+respawn produces.
    // Returns false if entity isn't live in scene_mgr_.active's registry.
    bool SetEntityTransform(uint32_t entity_int, const glm::mat4& world);

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

    // #228 R1: hot-reload a Prefab in place behind its stable PrefabId.
    // Re-parses the GLB at |path| as a NEW append-load via the regular
    // RuntimeLoadBatch flow, then SWAPS the new pool slot's Hot/Cold
    // INTO the old slot at |idx|. The old PrefabId handle (index +
    // generation) is preserved -- entities holding AssetRef remain
    // valid and pick up the new mesh on the next frame. The previously-
    // resident GPU resources go through the F1 (v2) per-resource
    // retire-frame ring, so they're freed kFIF frames later (when no
    // in-flight submission still binds them).
    //
    // F1 (v2)'s retire_frame stamping is what makes this safe -- the
    // F1 (v1) per-slot bucket would have batched these frees into the
    // wrong slot and the next-frame drain would tear down resources
    // still referenced by frames in flight, hanging the next render.
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

    // #228 R2: shader / pipeline hot-reload with KEEP-LAST-GOOD.
    // Attempts to build a new pipeline using the same desc the
    // matching init function uses; on success DeferFrees the old
    // handle through the F1 ring and swaps the engine member to the
    // new one. On failure (missing .spv / compile error / null
    // result) the existing pipeline is LEFT UNTOUCHED so render
    // continues with the previous PSO -- the hard non-negotiable
    // from the design: a broken shader must never take rendering
    // down.
    //
    // Supported logical names: "anim_eval", "skin", "particle"
    // (the three compute kernels). Graphics PSOs (forward_lit,
    // imgui, unlit, outline) reload via initRenderPipeline's
    // sub-call, deferred to R2.1.
    bool ReloadPipelineByName(const std::string& name);

    // #228 F2: Evict. Manifest 'remove' verb over the WHOLE batch span --
    // drops every currently resident prefab, defer-frees its GPU
    // resources (textures, samplers, meshes' position/index/attr buffers,
    // materials' bind groups), releases pool slots, and resets every
    // state array touched by the manifest (prefab_store_.per_prefab_asset, prefab_store_.glb_paths,
    // prefab_store_.resident_textures, prefab_store_.per_batch_shared_skin) plus the anim cursors
    // so the next load triggers a clean full-rebuild. Returns the
    // number of prefabs dropped.
    //
    // **Caller contract**: clear any entities referencing these prefabs
    // first via cairns.scene.clear. UnloadAllPrefabs does NOT clear
    // entities itself; cross-frame in-flight proxies that captured the
    // entity's prefab handle pre-clear would race with the descriptor
    // updates here, hanging the render thread on the next frame.
    // ClearActiveScene first, THEN UnloadAllPrefabs.
    //
    // Selective UnloadPrefabBatch(span<index>) is the follow-up; this
    // wholesale Unload covers the remixer "wipe + repopulate" beat and
    // proves F1's DeferFree path under bulk eviction.
    uint32_t UnloadAllPrefabs();

    // P1 input surface for main.cpp. Both no-op under CAIRNS_CAM_POSE so a
    // byte-gate dump can't be perturbed by an event that snuck through.

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

    // HUD stat injection seam (Tier G scenario 6: imgui stability). When set,
    // the imgui HUD draws from this value instead of live Timer accumulators
    // so the captured screen is byte-stable. The golden test feeds
    // cairns::HudStats::Mock() (16.6 ms / 60 fps / flat graph).
    void SetInjectedHudStats(const cairns::HudStats& s) {
        injected_hud_stats_ = s;
    }
    void ClearInjectedHudStats() { injected_hud_stats_.reset(); }
    const std::optional<cairns::HudStats>& InjectedHudStats() const {
        return injected_hud_stats_;
    }

    // A.2: runtime particle gate. Default off (EngineConfig::particles_enabled
    // = false). G1 particles scenario + G3 right-viewport flip on; everything
    // else stays off so the captured frames are "pipeline + clear + meshes",
    // no std::rand-shaped contamination.
    void EnableParticles(bool on) { particles_.enabled = on; }
    // A.3 single red NDC triangle (no scene): pipeline + clear + one draw.
    void SetTinyTriangle(bool on) { tiny_quad_test_ = on; }
    // G4: compose color + resolved-depth + extra-camera passes (the viewports
    // supplying the extra cameras are opened/aimed by the caller in JS).
    void SetNestedGraphMode(bool on) { nested_graph_mode_ = on; }
    bool ParticlesEnabled() const { return particles_.enabled; }

    // #229 imgui panel hook: the app (sdl-min) hands a callback that draws extra
    // imgui windows (the scenario launcher) into the HUD frame. Raw fn ptr + ctx
    // so the engine gains no singleton + no per-app coupling; the app does any
    // command dispatch itself, outside the render frame.
    // Surfaceless web app opts into the imgui HUD + panel (native windowed gets
    // it free via !surfaceless; cairns_serve leaves it false).
    void SetImguiEnabled(bool on) { imgui_enabled_ = on; }
    void SetImguiPanel(void (*fn)(void*), void* ctx) {
        imgui_panel_fn_ = fn;
        imgui_panel_ctx_ = ctx;
    }

    // A.9: drop the implicit "no imgui in golden" gate. G6 imgui stability
    // scenario flips this on so the HUD/overlay renders into the golden
    // capture. Implicit constraint: SetInjectedHudStats should be called
    // alongside this so the rendered HUD numbers are byte-stable
    // (HudStats::Mock() = 60 fps flat).
    void SetImguiInGolden(bool on) { imgui_in_golden_ = on; }
    bool ImguiInGolden() const { return imgui_in_golden_; }

    // #229 P7: per-frame SIM determinism digest (golden mode only; 0 otherwise).
    // Stable run-to-run across independent Engine instances (advances frame-to-
    // frame with the sim clock). test_state_hash asserts run-to-run equality.
    uint64_t LastSimHash() const { return last_sim_hash_; }
    // #229 P7: per-frame RENDER digest -- the bytes EncodeDraws feeds the GPU.
    // Stable run-to-run => GPU input deterministic; a flake past this point is
    // GPU-execution nondeterminism.
    uint64_t LastRenderHash() const { return last_render_hash_; }

    // A.12: read the currently-bound particle ssbo bytes for the G1
    // cross-platform buffer SECTION. Gated on A.10 (Resources::ReadBackBuffer)
    // being implemented -- today returns false so G1 buffer SECTION SKIPs
    // honestly. When ReadBackBuffer lands, this resolves
    // particles_.ssbo[particles_.latest_parity_out] and copies its bytes into `out`.
    bool ReadParticleBuffer(std::vector<uint8_t>& out) {
        if (particles_.ssbo[particles_.latest_parity_out].IsNull()) {
            return false;
        }
        return rhi_.resources.ReadBackBuffer(
            rhi_.alloc, particles_.ssbo[particles_.latest_parity_out],
            ParticleSystem::kParticleCount * static_cast<uint32_t>(sizeof(Particle)), out);
    }

    // A.5: open viewport 1, place its camera, spawn one glb into the active
    // scene, set its per-viewport particle gate. The G3 scenario drives this:
    // vp0 stays at default (left half, particles off), vp1 lands at right
    // half (particles on per the `particles_on` arg). Layout: equal halves
    // (OpenViewport itself does the uniform re-tile). Returns true iff a
    // viewport + a prefab + an entity all came up.
    // A.6: program viewports 1 + 2 around the existing scene to test
    // multi-pass render-graph behavior under the same backend submission.
    // Viewport 0 stays at default; viewports 1 + 2 open at yaws +60° / -60°
    // off heading. Particle gates default off (G4 isn't a particle test).
    // The "resolved-depth" readback is wired to ReadResolvedDepth in
    // test_seams; that path stays SKIP until the ReadBackBuffer salvage
    // (A.10) lands. Returns true iff both extra viewports came up.
    // A.7: per-frame draw / cull / vert counters. Populated at the end of
    // BuildMeshOpaqueDraws each frame. `culled` is 0 today -- the engine
    // doesn't yet run a frustum cull stage (frustum.hpp is currently only
    // consumed by the spec test). Adding the cull stage is in the Phase G
    // backlog; once it lands, fill `culled` and reduce `draw_calls` /
    // `verts_processed` accordingly.
    struct FrameStats {
        uint32_t draw_calls = 0;
        uint64_t verts_processed = 0;
        uint32_t culled = 0;       // currently always 0 -- see note above
        uint32_t submitted = 0;
        bool cull_stage_implemented = false;  // false => G5 SKIPs honestly
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
    // spawned into the active scene. The general fit-place primitive (#224).
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

    // #229 M0b: per-Engine synthetic scene id (was the g_scene_counter process
    // global -- no statics; deterministic per instance for the run-to-run hash).
    uint64_t NextSceneId() { return scene_mgr_.next_id++; }

    // #194 runtime viewport management. #220 Step 4: handle-pilled +
    // vpN wire-name layer.
    int ActiveViewportCount() const { return viewport_mgr_.active_count; }

    // Returns the engine-assigned monotonic name counter ("vp{N}" without
    // the prefix) or UINT32_MAX if at kNumViewports cap. Names are never
    // reused for the lifetime of the engine process (locked sub-decision
    // in plan #220 Step 4). The RPC layer formats the result as "vp{N}"
    // on the wire.
    uint32_t OpenViewport();

    // Close the highest-index viewport. Returns false if at 1 (cannot drop
    // below 1). Existing RPC takes no argument; this targets the last
    // opened viewport for backward compat with the protocol.
    bool CloseViewport();

    // Old int-indexed setLayout kept as a slot-position API for in-engine
    // callers; the wire layer uses SetViewportLayoutByName.
    bool SetViewportLayout(int viewport, glm::vec4 rect) {
        if (viewport < 0 || viewport >= viewport_mgr_.active_count) {
            return false;
        }
        viewport_mgr_.pool.GetHot(viewport_mgr_.ids[viewport])->layout_rect = rect;
        return true;
    }

    // #220 Step 4 wire-name layer. Parse "vp{N}" -> counter -> binary
    // search the sorted name table -> ViewportId. Returns Null on miss.
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

    // ===== P4 selection / highlight / pick. Document-side state -- the
    // selection set names "what the user (human or VLM) cares about right
    // now"; the highlight set names "what should glow". Both live on Engine
    // today; multi-scene will move them onto World once #195 lands. The GPU
    // ID buffer + outline shader + the actual pick readback are a separate
    // follow-up (#206..#208); RequestPick just records the click coordinate
    // so the handler can resolve it once the GPU side is wired. Full
    // screen->world ray picking is NOT a follow-up -- it was deliberately
    // replaced by the ID-buffer + readback path: O(1) per click, exact (no
    // geometry-vs-ray accuracy gap), cheap GPU-side, no scene-graph traversal.

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

    // Window-pixel coords. Engine doesn't resolve the pick yet -- the GPU
    // ID buffer + readback land in a follow-up; this just records the
    // request so a future RecordFrame can copy the texel out and a future
    // tick can deliver the resolved entity.
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
    // nearest hit wins. Returns entt id + 1 (0 = clicked empty space, matching the
    // old id-buffer convention). SYNCHRONOUS + identical on metal/vulkan/webgpu --
    // no GPU id-buffer readback (the browser cannot read back synchronously, which
    // is the asymmetry this replaces). The GPU id buffer stays only for the outline
    // edge-detect, which is GPU-side and already symmetric.
    //
    // TODO(picking-accel): O(entities) linear scan + first-mesh bind AABB only.
    // Add a BVH/grid (sub-linear) and union all meshes / use the live animated
    // AABB before the 3300-GLB rung. See TODO.md #picking-accel.
    uint32_t ResolvePickRaycast(int vp, uint32_t px, uint32_t py,
                                const glm::mat4& inv_view_proj);

    // PickResult + the pick/selection state now live in PickSelection
    // (engine/pick_selection.hpp); these accessors operate on picking_.
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

    // Current logical frame dims. Windowed: tracks the swapchain. Surfaceless:
    // tracks present_.final_target. Single source of truth for aspect / screen_params /
    // ImGui DPI scaling -- all of which used to read present_.swapchain directly and
    // were wrong by construction in surfaceless mode.
    uint32_t FrameWidth() const {
        return present_.final_target.IsNull() ? present_.swapchain.Width() : present_.final_target_w;
    }
    uint32_t FrameHeight() const {
        return present_.final_target.IsNull() ? present_.swapchain.Height() : present_.final_target_h;
    }

    // #207 (re)allocate per-viewport persistent R32U id targets if dims drift.
    // Called at the top of RecordFrame so each viewport's id_target_ matches
    // the current vp_w/vp_h. Destroys old targets through the rhi destroy
    // queue so any in-flight frame using the prior dims is unaffected.
    void EnsureIdTargets(uint32_t w, uint32_t h);

    // #207 (re)build the highlights texture from picking_.highlights. Called by
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

    // #220 Step 4: acquire the initial viewport slot (vp0) and pre-fill
    // the layout / active selection. Called from GreaterInit before any
    // cam_pose override walks the viewport pool. Idempotent: bails if
    // viewport 0 is already live.
    void InitInitialViewport();
    
    // #229 M0b: re-seat a default-constructed (malloc-fallback) block-backed
    // vector onto cpu_block_ + reserve. POCMA makes the empty move-assign adopt
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

    // Render-thread entry point (post commit 6). Today called synchronously
    // from draw(). Owns: rhi_.frames.Begin/End, the bump-ring EncodeDraws,
    // the compute + render-pass encode. Reads pkt + slots_[pkt.slot].
    void RecordFrame(FramePacket& pkt);

    bool initRenderPipeline();

    struct Particle {
        float position[2];
        float velocity[2];
        float color[4];
    };

    // #221 Phase 9: opportunistic SkinnedAttachment factory. Resolves the
    // scene, picks the walking clip (`SelectWalkingClip`), finds the first
    // skinned mesh (cpuSkinAttrs non-empty), allocates a skinning_.output_pool
    // slice sized to that mesh's vertex count, and returns the new SkinId.
    // Null on any miss (no skins, no skinned mesh, no clips, no pool
    // capacity left). The actor's per-frame palette uses the stored
    // clip_index + time_offset and writes deformed verts at slice.offset.
    cairns::SkinId TryCreateSkinForScene(cairns::PrefabId scene_id,
                                          float time_offset);

    // #221 Phase 5: per-frame skin pipeline (game-thread side). Walks the
    // active scene for SkinRef entities, samples each actor's clip into a
    // per-slot palette slab on the arena, buckets visible actors by mesh
    // (flat-array prefix-sum, NO map per standing rule), emits SkinBatchGpu
    // rows + flat InstanceMeta + flat palettes, publishes spans on s.pkt.
    // Today: no SkinRef in the registry => skin_batches empty. Static path
    // bit-for-bit unchanged.
    void BuildSkinFrame(uint32_t slot);

    void initAnimEvalKernel();

    // #221 Phase 5b: flatten every loaded scene's animation tables into
    // shared kDefault SSBOs and stamp per-scene base offsets into the
    // scene_headers SSBO. Called ONCE after all scenes are loaded. Bumps
    // skinning_.eval_tables_uploaded = true on success.
    // #228 H4b vk fix: (re)create skinning_.dyn_anim_eval DynamicBuffers set using
    // the current anim buffer handles. Idempotent and safe to call before
    // skinning_.eval_tables_uploaded flips true (early-returns when buffers
    // don't exist yet). vk needs this set; metal ignores dyn_set_0 in
    // DispatchAnimEval.
    bool recreateAnimDynBindings();

    // #224 L9 wedge-at-scale fix: skinning_.dyn_skin_group_b binding 1 (palettes)
    // is gated on skinning_.eval_tables_uploaded at GreaterInit. L9's empty
    // boot leaves it false, so binding 1 falls back to the kDynamic master
    // (a per-frame 64 KB ring) instead of skinning_.palette_out_buf (16 MB). The
    // skin compute then reads palette transforms out of the wrong buffer
    // -- at ~9 actors the dyn master happens to contain enough zeros to
    // pass, but at 100+ actors every joint read is garbage and the mesh
    // wedges into giant tendrils. Recreate the set after first upload so
    // binding 1 captures skinning_.palette_out_buf. Mirrors GreaterInit's gb[] tab.
    bool recreateSkinGroupB();

    // Flatten every loaded scene's animation tables into the GPU buffers the
    // anim_eval kernel reads. Pose evaluation + palette build run on the GPU -- one
    // workgroup per actor over global flattened tables -- so per-frame skinning
    // stays off the CPU and shared data is addressed by offset, not per-object
    // bindings.
    //
    // The kernel used to take 12 separate storage buffers; WebGPU guarantees only
    // 8 storage buffers per stage (Chrome caps at 10, and there is no portable tier
    // at 12), so the 10 read-only tables are folded into 3 buffers grouped by
    // element stride. The data was already offset-addressed (every
    // SceneHeader.*_off), so packing is just sharing one buffer per stride class;
    // the offsets become element offsets into the packed buffer. The 6 buffers:
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

    // #221 Phase 4: best-effort load of skin compute kernel. Failing the
    // load (missing skin.comp.spv / skin.metal) leaves skinning_.skin_kernel Null;
    // Phase 5's dispatch checks IsNull() and degenerates to "no skinning
    // this frame", preserving the static path bit-for-bit.
    void initSkinKernel();

    bool initParticles();

    // #222 Phase D.4: particle SSBOs split out of initParticles so the
    // parity DynamicBuffers can grab their handles before the kernel
    // pipeline is built (pipeline layout is sourced from parity[0]).
    bool initParticleSsbos();

    bool deinit();
    
private:
    uint32_t frame_ = 0;

    // #229 M0b: the single owning CPU memory block. All persistent + per-frame
    // CPU state is carved from here (1 GB desktop / 256 MB mobile, fail-loud).
    // Declared BEFORE every block-backed member (pools, loose vectors, entt
    // registry, prefab_arena_) so it is destroyed LAST -- those containers'
    // ChunkStdAllocator dtors deallocate into it at ~Engine teardown, so the
    // block (and its mmap-backed chunks) must outlive them. Defensive ordering;
    // the actual 100-GLB teardown crash was an oversize-Free use-after-free in
    // ChunkAllocator::Free (read header after std::free), fixed separately.
    cairns::ChunkAllocator cpu_block_;
    // #229 P3 string interning: persistent block-backed arena for the prefab
    // load tables that used to embed std::string/std::vector headers (node/clip
    // names, Node::children, Skin/Clip inner arrays). NameRef + ArenaSlice store
    // byte offsets into this slab -> the pool structs become pointer-free POD.
    // Monotonic (no per-asset free yet; reload churn bump-leaks -- bounded). Slab
    // is carved from cpu_block_, so it sits right after it (destroyed before it).
    cairns::BumpArena prefab_arena_{};

    // Loaded-prefab store: prefab/material/mesh pools + parallel id/path/asset
    // vectors + resident-texture + per-batch-skin lists + loader instruments,
    // grouped in PrefabStore (C2 S1). cpu_block_ + prefab_arena_ stay on Engine
    // (declared earlier) so they outlive the store's pools + interned slices.
    cairns::PrefabStore prefab_store_;
    // #224 L3: the instrument. Populated each LoadPrefabBatch call.
    // #224 L8: editor-chrome toggle (selection outline pass gate).
    // Default on; remixer/canvas flips to false to suppress meta-UI
    // before captures. Stylized highlight (materials) untouched.
    bool editor_chrome_enabled_ = true;
    // #224 L6: APPEND-only debug snapshot stashed between two NDJSON
    // op calls (cairns.debug.snapshotPrefabHandles ->
    // cairns.debug.assertAppendOnly). Empty until first snapshot call.
    std::vector<PrefabHandleSnapshot> last_handle_snapshot_;
    std::vector<int32_t> root_nodes_stack_cache_;


    // Skinning + animation GPU state (pool, output RangePool, kernels, folded
    // anim-table SSBOs, delta-upload cursors, dyn descriptor sets) grouped in
    // AnimSkinSystem (C2 S2).
    cairns::AnimSkinSystem skinning_;

    // Per-slot frame buffers (drawList / drawListSorted / proxies /
    // resident_textures / draw_world_matrices / pending_globals / globals_offset
    // / dt_off / FramePacket). See PerSlot above.
    // std::array (not std::vector): PerSlot holds std::atomic<bool> which
    // is not move-constructible, so vector::resize would fail to compile.
    // kFramesInFlight is compile-time anyway -- the runtime resize was
    // never needed.
    std::array<PerSlot, kFramesInFlight> slots_{};

    // #221 / multithreaded build_draws pool. Taskflow-backed persistent
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
    // scene-id trio grouped in SceneManager (C2 S4).
    cairns::SceneManager scene_mgr_;

    // #220 Step 4: handle-pilled Viewport pool. viewport_mgr_.pool owns Hot+Cold;
    // viewport_mgr_.ids[0..viewport_mgr_.active_count) carry the slot ordering
    // (preserves the [0..N) layout/indexing semantics the rest of the
    // engine uses to address PerSlot::pending_globals[], id_target_[],
    // etc.). FlyController moved into Viewport::Cold (was fly_ parallel
    // array; reason "fly is parallel so Viewport struct can grow" is moot
    // once Viewport is in a generational pool).
    //
    // viewport_mgr_.active is now the ViewportId of the focused viewport.
    // viewport_mgr_.active_index caches its position in viewport_mgr_.ids so the
    // PerSlot per-viewport arrays can still be indexed by int. Both fields
    // are updated together via setActiveViewport().
    //
    // #194: compile-time cap on simultaneous viewports. viewport_mgr_.active_count
    // (runtime) tells the engine how many slots are LIVE. Default = 1 (full-
    // frame viewport 0). Grow via cairns.viewport.open / shrink via
    // cairns.viewport.close. Layout rects on Viewport::Hot::layout_rect
    // (NDC 0..1 over the swap pane) describe where each live viewport tiles.
    // Viewport pool + id/name tables + active index/count + vpN counter
    // grouped in ViewportManager (C2 S5); kNumViewports + ViewportName now
    // live in the shared header engine/viewport_manager.hpp.
    cairns::ViewportManager viewport_mgr_;

    // #210 per-slot CPU arena capacity. #221 Phase 3 raise to 16 MiB to
    // cover the per-frame palette/InstanceMeta/SkinMeshBatch arrays the
    // skin pipeline parks here (Phase 5 will print HighWater() and we'll
    // resize from data). Stored on PerSlot (the slot IS the lock).
    // Initialized in initCpuAllocators, Reset()'d at slot Acquire (render
    // thread already drained).
    // #229 cpu_block_ / prefab_arena_ are declared EARLY (before the pools) so
    // they are destroyed LAST -- the pools' ChunkStdAllocator vectors deallocate
    // into cpu_block_ in ~ResourceManager, so the block must outlive them.

    static constexpr size_t kArenaBytesPerSlot = 16u * 1024u * 1024u;
    // Sized to the measured 100-GLB footprint (~58 MB; mostly all-clip
    // sampler/channel slices, which are load-scratch -- a follow-on can shrink
    // this a lot by not persisting non-walk clips). Platform-independent: the
    // same GLBs need the same space, so NOT budget-scaled. Oversize (> chunk) =>
    // a dedicated malloc, not carved from the 256 MB mobile chunk reservation.
    static constexpr size_t kPrefabArenaBytes = 96u * 1024u * 1024u;

    rhi::Rhi rhi_;
    // #228 H3: mesh_master_handle_ deleted. Was set in GreaterInit to
    // prefab_store_.prefab_ids[0]'s first mesh's posHandle, copied into
    // MeshDrawList::resident_buffers, never read by the recorder.
    // Per-frame render graph. Reused via Reset() across frames (vector storage
    // for passes/textures is preserved). Constructed lazily on first RecordFrame
    // because Resources& / Allocator& must already be initialized.
    std::unique_ptr<rhi::RenderGraph> graph_;

    // Swap / present / final-target / resize state grouped in PresentTargets
    // (C2 S7); declared after rhi_ so its swapchain tears down before the device.
    cairns::PresentTargets present_;
    // shaders
    ShaderHandle unlit_offscreen_ = ShaderHandle::Null;
    // #222 Phase A.1: id-less variant; selected when no consumer wants the
    // R32U id attachment this frame.
    ShaderHandle unlit_offscreen_noid_ = ShaderHandle::Null;
    // #222 Phase D.2: DynamicBuffers for unlit set 0 (pass globals UBO) +
    // set 2 (per-draw drawtmp UBO). Created post-Frames::Init with backing
    // = kDynamic master. RecordFrame stamps them on MeshDrawList +
    // Draw::dynamic_buffers; recorder reads the per-FIF set from Hot.
    rhi::Handle<rhi::DynamicBuffers> dyn_globals_;
    rhi::Handle<rhi::DynamicBuffers> dyn_drawtmp_;
    // #222 Phase D.3: skin Group B (4 bindings) + anim_eval (13 bindings)
    // through DynamicBuffers. Created post-scene-load when skinning_.palette_out_buf
    // + ae_*_buf_ + skinning_.output_pool_buffer exist. Backing buffer is set
    // per-binding (skinning_.palette_out_buf for palettes; skin_output_pool for output
    // pool; ae_*_buf_ for scene tables). kDynamic-master fallback covers
    // the per-dispatch dynamic-offset bindings (params/inst_meta/records).
    // #222 Phase D.4: particle parity DynamicBuffers. Index 0 binds
    // particles_.ssbo[0] -> binding 1 and particles_.ssbo[1] -> binding 2
    // (step_src=0). Index 1 swaps them (step_src=1). Binding 0 (UBO_DYN
    // dt) backed by kDynamic master; per-dispatch dyn offset = current
    // dt_off. Replaces the per-step vkUpdateDescriptorSets path.
    rhi::Handle<rhi::DynamicBuffers> dyn_particle_parity_[2];
    ShaderHandle composite_pip_ = ShaderHandle::Null;
    ShaderHandle depthviz_ = ShaderHandle::Null;
    // A.3: L1 single-red-triangle pipeline. Three NDC vertices from
    // gl_VertexIndex, no vertex buffer, solid red frag. Drawn via
    // DrawFullscreen (which takes 3 vertices regardless) only when
    // tiny_quad_test_ is set.
    ShaderHandle red_triangle_pip_ = ShaderHandle::Null;
    ShaderHandle outline_pip_ = ShaderHandle::Null;
    rhi::Handle<rhi::Sampler> composite_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // #207 nearest sampler for outline's id_off binding -- R32_UINT can't be
    // linearly filtered (VUID-vkCmdDraw-magFilter-04553). Outline's color
    // binding is also nearest because the fullscreen tri samples color_off
    // at native res (texel-aligned), so linear vs nearest is identical.
    rhi::Handle<rhi::Sampler> outline_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // imgui
    rhi::Handle<rhi::Shader> imgui_ = rhi::Handle<rhi::Shader>::Null;
    rhi::Handle<rhi::Texture> imgui_font_ = rhi::Handle<rhi::Texture>::Null;
    rhi::Handle<rhi::Sampler> imgui_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // particles: state grouped in ParticleSystem (C2 S3) -- kernel/shaders/
    // SSBOs + the cross-thread parity handshake (mutex/cv/counters) travel as
    // one unit. Engine's init/draw systems operate on it.
    cairns::ParticleSystem particles_;
    std::unique_ptr<cairns::RenderThread> render_thread_;


    // #207 persistent per-viewport R32U id targets. Replaces the previous
    // transient id_off graph texture so end-of-frame pick readback (a
    // vkCmdCopyImageToBuffer / metal blit on a 1x1 region) can read the
    // last-rendered id without racing the transient pool's reuse. Sized
    // (vp_w, vp_h); reallocated lazily by EnsureIdTargets when dims drift.
    std::array<rhi::Handle<rhi::Texture>, kNumViewports> id_target_{};
    uint32_t id_target_w_ = 0;
    uint32_t id_target_h_ = 0;

    // Selection/highlight/pick document state + the pick request/result
    // handshake, grouped in PickSelection (C2 S6). The per-viewport id_target
    // render targets above stay on Engine (GPU resources, kNumViewports-coupled).
    cairns::PickSelection picking_;
    static constexpr uint32_t kMaxHighlights = 64;


    // Fiedler fixed-timestep accumulator state. Game-thread only -- never
    // touched by the render thread. clock_ is WallClock in live mode,
    // FixedClock under CAIRNS_DUMP.
    std::unique_ptr<cairns::FrameClock> clock_;
    double accumulator_ = 0.0;
    uint64_t sim_frame_ = 0;
    float sim_angle_deg_ = 0.0f;
    float render_angle_deg_ = 0.0f;
    uint32_t sim_steps_this_frame_ = 0;
    // #229 P7: last per-frame SIM determinism digest (FixedClock + static scene
    // => byte-identical every frame and run-to-run). Read by test_state_hash.
    uint64_t last_sim_hash_ = 0;
    // #229 P7: last per-frame RENDER digest (the GPU-input bytes EncodeDraws
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
    // Set via SetNestedGraphMode (JS render.nestedGraph op). Currently
    // informational only -- the engine's render graph composes the right
    // shape (multiple forward passes, one per active viewport) regardless.
    // Will gate the resolved-depth path once the ReadBackBuffer salvage
    // is wired.
    bool nested_graph_mode_ = false;
    // #229 app-provided imgui panel (scenario launcher). nullptr = none.
    void (*imgui_panel_fn_)(void*) = nullptr;
    void* imgui_panel_ctx_ = nullptr;
    // Surfaceless web app opts into imgui (HUD + panel); cairns_serve doesn't.
    bool imgui_enabled_ = false;
    // A.7: stamped at end of BuildMeshOpaqueDraws every frame.
    FrameStats last_frame_stats_{};
#if CAIRNS_ALLOC_TRACE
    // #229 prev snapshot for the [STEADY-60] alloc receipt. A member, NOT a
    // function-local static -- global mutable state is banned. Trace only.
    cairns::alloc_count::Snapshot alloc_steady_prev_{};
#endif
    // A.9: G6 opt-in; render imgui into the golden capture.
    bool imgui_in_golden_ = false;
    // Diagnostic: pin every draw to 2 triangles. Draw count + submission
    // identical, geometry throughput ~700x smaller. Isolates draw-submission
    // overhead vs geometry-throughput in the forward pass cost.
    bool tiny_quad_test_ = false;
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

