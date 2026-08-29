#pragma once

#include "util/define.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <optional>
#include <thread>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numbers>
#include <variant>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <stb_image_write.h>

#include "render/worker_context.hpp"
#include "util/cpu_arena.hpp"
#include "util/cpu_pool.hpp"  // #221 Phase 3: RangePool for skin_output_pool_.
#include "util/device_caps.hpp"  // boot-invariant + HUD/skin fit predicates
#include "util/hud_stats.hpp"
#include "util/animation_runtime.hpp"  // #221 Phase 9: SelectWalkingClip + sampler.
#include "render/render_proxy.hpp"  // #221 Phase 3: SkinnedAttachment Hot/Cold.
#include "render/particle_emitter.hpp"  // A.1: ParticleRng (portable mt19937)

#include "gfx_api.hpp"
#include "rhi/init_config.hpp"
#include "rhi/swap_chain.hpp"
#include "gpu_scene_registry.hpp"
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
#include "imgui_impl_sdl3.h"

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

// Engine-level startup options. The shell (sdl-min / cairns_serve) lowers
// CAIRNS_* env knobs into this struct at startup so the engine never reads
// std::getenv directly. Empty / default-constructed values mean "use the
// engine's built-in default" so partial population is safe.
struct EngineConfig {
    // CAIRNS_DUMP: when non-empty, FixedClock + one-shot dump on
    // kGoldenDumpFrame to this path, then exit(0). Drives byte-gates.
    std::filesystem::path dump_path;

    // CAIRNS_TINY_QUAD: tiny-quad parity render path.
    bool tiny_quad = false;

    // CAIRNS_CAM_POSE: pin every viewport's fly controller to this fixed
    // (pos, yaw_rad, pitch_rad). Disables live fly input.
    struct CamPose {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float yaw = 0.0f;
        float pitch = 0.0f;
    };
    std::optional<CamPose> cam_pose;

    // CAIRNS_GLB: comma-separated list of glb names / paths. Empty =>
    // engine default (kDebugGlbs window).
    std::vector<std::string> glb_overrides;

    // Run the engine with a FixedClock (deterministic frame dt) WITHOUT the
    // CLI dump_path side effect of "dump at kGoldenDumpFrame then exit(0)."
    // The golden test harness needs the determinism without the exit -- it
    // can't be killed mid-suite. dump_path != "" still implies fixed clock
    // (CLI behaviour preserved); this bool is the only way to ask for fixed
    // clock without the dump+exit path.
    bool use_fixed_clock = false;

    // A.2: gate particle_sim + particle_draw at the source. Default OFF for
    // every ladder rung + every scenario except G1 (and G3-right viewport).
    // Particle compute writes via std::rand-shaped paths consumed downstream;
    // with this off, L1 "pipeline + clear + one draw" is actually that.
    // Set via Engine::EnableParticles(bool) at runtime; CLI app and serve
    // shell default ON via main / serve_main lowering.
    bool particles_enabled = false;
};

class Engine {
public:

    using TexHandle = rhi::Handle<rhi::Texture>;
    using BufHandle = rhi::Handle<rhi::Buffer>;
    using DynBufId = uint32_t;
    using ShaderHandle = rhi::Handle<rhi::Shader>;
    // #220 Step 1: MatId is now a generational Handle into Engine::materials_
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
        std::vector<uint8_t> arena_storage;
        cairns::BumpArena arena{};
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
    
    bool initSwapChain(const rhi::InitConfig& cfg) {
        if ( !rhi_.device.InitSwapChain(swapchain_, cfg)) {
            return false;
        }

        return true;
    }
    
    // SDL fires SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED on the event thread.
    // We don't synchronously touch any GPU state here -- ApplyPendingResize
    // (top of draw()) drains the render thread first.
    bool requestResizeFrameBuffer(uint32_t width, uint32_t height) {
        if (width == 0 || height == 0) {
            return true;
        }
        resize_pending_w_ = width;
        resize_pending_h_ = height;
        resize_pending_ = true;
        return true;
    }

    bool RequestViewportDump(const std::filesystem::path& path) {
        rhi_.frame_capture.SetDumpPath(path);
        return true;
    }

    // #269: spawn one hero entity in active_scene_ from a pre-loaded
    // scene. Returns the new entt entity id (0 on failure: bad
    // scene_idx, no active scene, prefab_ids_ not populated, etc.).
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
                                    float time_phase, bool attach_skin) {
        if (scene_idx >= prefab_ids_.size() ||
            scene_idx >= per_prefab_asset_.size()) {
            return UINT32_MAX;
        }
        cairns::Scene::Cold* wc = scenes_.GetCold(active_scene_);
        if (!wc) {
            return UINT32_MAX;
        }
        auto& reg = wc->registry;
        const entt::entity e = reg.create();
        cairns::WorldTransform wt;
        wt.world = world;
        reg.emplace<cairns::WorldTransform>(e, wt);
        cairns::AssetRef ar;
        ar.asset = per_prefab_asset_[scene_idx];
        reg.emplace<cairns::AssetRef>(e, ar);
        cairns::Renderable rdr;
        rdr.layer_mask = 0xFFFFFFFFu;
        rdr.flags = cairns::kProxyVisible;
        reg.emplace<cairns::Renderable>(e, rdr);
        if (attach_skin) {
            cairns::SkinId sid =
                TryCreateSkinForScene(prefab_ids_[scene_idx], time_phase);
            if (!sid.IsNull()) {
                reg.emplace<cairns::SkinRef>(e, cairns::SkinRef{sid});
            }
        }
        // Mark world dirty so the proxy extract picks up the new entity.
        if (auto* wh = scenes_.GetHot(active_scene_)) {
            wh->dirty = true;
        }
        return static_cast<uint32_t>(entt::to_integral(e));
    }

    // #269: how many scenes (GLBs) loaded; clients call InstantiatePrefab with
    // scene_idx in [0, NumPrefabs()). Lets the NDJSON op validate args.
    uint32_t NumPrefabs() const {
        return static_cast<uint32_t>(prefab_ids_.size());
    }

    // #224 L1: resolve the shared skin attrs buffer for a mesh from its
    // batch_id. Null on out-of-range or unset (= unskinned mesh).
    rhi::Handle<rhi::Buffer> ResolvedSharedSkin(const cairns::Mesh::Hot& mhot) const {
        if (mhot.batch_id >= per_batch_shared_skin_.size()) {
            return rhi::Handle<rhi::Buffer>::Null;
        }
        return per_batch_shared_skin_[mhot.batch_id];
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
    // into prefab_ids_ where this batch's prefabs landed. Bad parses are
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

        LoadPrefabBatchResult r{};
        r.first_prefab_idx = static_cast<uint32_t>(prefab_ids_.size());

        // #224 L3: per-stage timing. steady_clock so the trace numbers
        // are wall-clock; the byte-gate doesn't reference them.
        using Clock = std::chrono::steady_clock;
        const auto t_total = Clock::now();
        cairns::LoadTrace trace{};

        // ── parse + validate + prepare resources per glb (no GPU upload yet) ──
        const auto t_parse = Clock::now();
        cairns::ValidationReport vreport{};
        for (const std::filesystem::path& p : glbs) {
            cairns::PrefabId sid = prefabs_.Acquire();
            cairns::Prefab::Hot* shot = prefabs_.GetHot(sid);
            cairns::Prefab::Cold* scold = prefabs_.GetCold(sid);
            if (!shot || !scold ||
                !cairns::LoadPrefabFromGltf(p, *shot, *scold, meshes_)) {
                CAIRNS_PRINT_ERR("[LoadPrefabBatch] parse failed: %s\n",
                                  p.string().c_str());
                prefabs_.Release(sid);
                continue;
            }
            // #224 L2: validate against engine caps before upload.
            const uint32_t prefab_idx_for_log =
                static_cast<uint32_t>(prefab_ids_.size());
            if (!ValidatePrefab(*scold, vreport, prefab_idx_for_log)) {
                CAIRNS_PRINT_ERR(
                    "[LoadPrefabBatch] validation failed for %s -- "
                    "skipping prefab.\n", p.string().c_str());
                prefabs_.Release(sid);
                continue;
            }
            cairns::PreparePrefabResources(*shot, *scold, rhi_.resources,
                                            rhi_.alloc, materials_);
            prefab_ids_.push_back(sid);
            ++r.count;
        }
        trace.Add("parse_gltf",
                   std::chrono::duration<double, std::milli>(
                       Clock::now() - t_parse).count(),
                   0, r.count);
        if (r.count == 0) {
            trace.total_ms = std::chrono::duration<double, std::milli>(
                Clock::now() - t_total).count();
            last_load_trace_ = trace;
            return r;
        }

        // ── upload the new batch's prefabs to NEW kDefault buffers ──
        const auto t_upload = Clock::now();
        rhi::Handle<rhi::Buffer> batch_shared_skin =
            rhi::Handle<rhi::Buffer>::Null;
        std::span<const cairns::PrefabId> new_span(
            prefab_ids_.data() + r.first_prefab_idx, r.count);
        if (!cairns::rhi::LoadPrefabsGpu(new_span, prefabs_, meshes_,
                                          rhi_.resources, rhi_.alloc,
                                          &batch_shared_skin)) {
            CAIRNS_PRINT_ERR(
                "[LoadPrefabBatch] LoadPrefabsGpu failed for %u prefabs\n",
                r.count);
            trace.total_ms = std::chrono::duration<double, std::milli>(
                Clock::now() - t_total).count();
            last_load_trace_ = trace;
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
        last_load_trace_ = trace;
        last_validation_report_ = vreport;
        ++loader_counters_.batches_loaded;
        loader_counters_.prefabs_resident += r.count;
        loader_counters_.meshes_resident += batch_mesh_count;
        loader_counters_.last_batch_ms = trace.total_ms;
        if (trace.total_ms > loader_counters_.peak_batch_ms) {
            loader_counters_.peak_batch_ms = trace.total_ms;
        }
        // Phase D close: end-of-batch marker. Pair with [LOAD] begin
        // so log scanning can compute per-batch wall time without
        // hunting for the LoadTrace summary.
        CAIRNS_PRINT_ERR("[LOAD] end batch ms=%.3f count=%u\n",
                          trace.total_ms, r.count);
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
            skins_.ForEachLive(
                [&](cairns::SkinnedAttachment::Hot& h,
                    cairns::SkinnedAttachment::Cold& c) {
                    if (cairns::Prefab::Hot* sht = prefabs_.GetHot(c.scene)) {
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
            std::vector<std::string>* out_msgs = nullptr) {
        uint32_t v = 0;
        auto fail = [&](const char* what) {
            ++v;
            if (out_msgs) {
                out_msgs->emplace_back(what);
            }
        };
        const size_t n_prefabs = prefab_ids_.size();
        // (1) StampBatchSkinAndMeshIds:
        //     per_batch_shared_skin_ has at least one entry whenever any
        //     prefab is resident; every Mesh::Hot::batch_id indexes it.
        if (n_prefabs > 0 && per_batch_shared_skin_.empty()) {
            fail("per_batch_shared_skin_ empty but prefabs are resident");
        }
        const size_t n_batches = per_batch_shared_skin_.size();
        meshes_.ForEachLive(
            [&](cairns::Mesh::Hot& mh, cairns::Mesh::Cold&) {
                if (mh.batch_id >= n_batches) {
                    fail("Mesh::Hot::batch_id >= per_batch_shared_skin_.size()");
                }
            });
        // (2) BuildGroupABindGroups -- vk only; on metal skin_group_a is
        //     Null by design. Skip; not a portable invariant.
        // (3) ValidateAndCleanupTmps:
        //     every live Mesh::Cold has empty cpuPositions/cpuAttrs/
        //     cpuIndices after the batch finished.
        meshes_.ForEachLive(
            [&](cairns::Mesh::Hot&, cairns::Mesh::Cold& mc) {
                if (!mc.cpuPositions.empty() || !mc.cpuAttrs.empty() ||
                    !mc.cpuIndices.empty()) {
                    fail("Mesh::Cold cpu temporaries not cleared");
                }
            });
        // (4) BuildMaterialSet2:
        //     every live Material::Hot has non-null set2.
        materials_.ForEachLive(
            [&](cairns::Material::Hot& mat, cairns::Material::Cold&) {
                if (mat.set2.IsNull()) {
                    fail("Material::Hot::set2 is Null");
                }
            });
        // (5) BuildResidentTextures:
        //     resident_textures_.size() == sum of every live prefab's
        //     Cold.textureHandles.size().
        size_t sum_tex = 0;
        prefabs_.ForEachLive(
            [&](cairns::Prefab::Hot&, cairns::Prefab::Cold& pc) {
                sum_tex += pc.textureHandles.size();
            });
        if (resident_textures_.size() != sum_tex) {
            fail("resident_textures_.size() != sum_of_prefab_textureHandles");
        }
        // (6) StampPerPrefabAsset:
        //     per_prefab_asset_.size() == prefab_ids_.size().
        if (per_prefab_asset_.size() != n_prefabs) {
            fail("per_prefab_asset_.size() != prefab_ids_.size()");
        }
        // (7) AppendGlbPaths:
        //     glb_paths_.size() == prefab_ids_.size(). [PICK] log
        //     resolves prefab_idx -> filename via this.
        if (glb_paths_.size() != n_prefabs) {
            fail("glb_paths_.size() != prefab_ids_.size()");
        }
        // (8) AcquireSceneCells:
        //     active_scene_ valid (entt registry exists for instantiate).
        if (active_scene_.IsNull()) {
            fail("active_scene_ is Null (no entt container)");
        }
        return v;
    }

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
    // the batch's shared skin attrs buffer to per_batch_shared_skin_.
    // Also count the new meshes for the trace.
    void StampBatchSkinAndMeshIds(
            std::span<const cairns::PrefabId> new_span,
            rhi::Handle<rhi::Buffer> batch_shared_skin,
            uint32_t& batch_mesh_count_out) {
        const uint16_t batch_id =
            static_cast<uint16_t>(per_batch_shared_skin_.size());
        per_batch_shared_skin_.push_back(batch_shared_skin);
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Hot* shot = prefabs_.GetHot(sid);
            if (!shot) {
                continue;
            }
            for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
                if (cairns::Mesh::Hot* mhot = meshes_.GetHot(mid)) {
                    mhot->batch_id = batch_id;
                    ++batch_mesh_count_out;
                }
            }
        }
    }

    // Vulkan Group A descriptor set (positions slice + skin attrs slice)
    // per new skinned mesh. Metal binds buffers directly per batch in
    // DispatchSkinBatches; CreateSkinGroupA returns Null there.
    void BuildGroupABindGroups(
            std::span<const cairns::PrefabId> new_span,
            rhi::Handle<rhi::Buffer> batch_shared_skin) {
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Hot* shot = prefabs_.GetHot(sid);
            if (!shot) {
                continue;
            }
            for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
                cairns::Mesh::Hot* mhot = meshes_.GetHot(mid);
                if (!mhot || mhot->attr_skinned_alias.IsNull() ||
                    batch_shared_skin.IsNull() || mhot->vert_count == 0) {
                    continue;
                }
                cairns::rhi::BufferBinding bb[2]{};
                bb[0].slot = 0;
                bb[0].buffer = mhot->posHandle;
                bb[0].offset = mhot->global_base_vertex *
                    static_cast<uint32_t>(sizeof(glm::vec4));
                bb[0].range = mhot->vert_count *
                    static_cast<uint32_t>(sizeof(glm::vec4));
                bb[0].kind = cairns::rhi::BufferKind::kStorage;
                bb[1].slot = 1;
                bb[1].buffer = batch_shared_skin;
                bb[1].offset = mhot->skin_attr_base_vertex *
                    static_cast<uint32_t>(sizeof(cairns::PackedSkinVertex));
                bb[1].range = mhot->vert_count *
                    static_cast<uint32_t>(sizeof(cairns::PackedSkinVertex));
                bb[1].kind = cairns::rhi::BufferKind::kStorage;
                cairns::rhi::BindGroupDesc bgd{};
                bgd.debug_name = "skin_group_a";
                bgd.buffers = std::span<const cairns::rhi::BufferBinding>(
                    bb, 2);
                mhot->skin_group_a = rhi_.resources.CreateSkinGroupA(
                    rhi_.alloc, rhi_.frames, rhi_.pipelines, bgd);
            }
        }
    }

    // Per-mesh weight-sum validation + Prefab::Cold::CleanupTmps +
    // per-new-mesh cpu temp clear. Combined because they share the
    // pre-cleanup-tmps window for skin attrs.
    void ValidateAndCleanupTmps(
            std::span<const cairns::PrefabId> new_span,
            uint32_t first_prefab_idx,
            cairns::ValidationReport& vreport) {
        for (uint32_t pi = 0;
             pi < static_cast<uint32_t>(new_span.size()); ++pi) {
            const uint32_t prefab_idx = first_prefab_idx + pi;
            cairns::Prefab::Hot* shot =
                prefabs_.GetHot(prefab_ids_[prefab_idx]);
            if (!shot) {
                continue;
            }
            for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
                if (cairns::Mesh::Cold* mc = meshes_.GetCold(mid)) {
                    ValidateMeshWeights(*mc, vreport, prefab_idx);
                }
            }
        }
        for (cairns::PrefabId sid : new_span) {
            if (cairns::Prefab::Cold* sc = prefabs_.GetCold(sid)) {
                sc->CleanupTmps();
            }
        }
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Hot* shot = prefabs_.GetHot(sid);
            if (!shot) {
                continue;
            }
            for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
                if (cairns::Mesh::Cold* mc = meshes_.GetCold(mid)) {
                    mc->cpuPositions.clear();
                    mc->cpuAttrs.clear();
                    mc->cpuIndices.clear();
                }
            }
        }
    }

    // Build Material::Hot::set2 for every live material that doesn't
    // already have one. Idempotent skip-if-built so running per-batch
    // doesn't rebuild prior batches' sets.
    // (initRenderPipeline also calls this once at boot; with L9's empty
    // boot it iterates zero materials and was stranded -- L10b moved
    // the loop here.)
    void BuildMaterialSet2() {
        materials_.ForEachLive(
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

    // Append the new prefabs' textureHandles to resident_textures_ so
    // DrawMeshes' bindless sampler array can index them. APPEND-only;
    // existing entries' indices unchanged (L6 contract).
    void BuildResidentTextures(std::span<const cairns::PrefabId> new_span) {
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Cold* scold = prefabs_.GetCold(sid);
            if (!scold) {
                continue;
            }
            for (rhi::Handle<rhi::Texture> th : scold->textureHandles) {
                resident_textures_.push_back(th);
            }
        }
    }

    // Append the new batch's GLB paths to glb_paths_ so the [PICK] log
    // can name a clicked hero by filename. (Mole #6: this used to run
    // only in GreaterInit; runtime-loaded heroes logged "" until now.)
    // The order matches the prefab_ids_ append order (new_span loops
    // forward); only paths that successfully Acquired a prefab slot
    // appear -- the input `glbs` may be longer if parses failed, so we
    // slice to new_span.size().
    void AppendGlbPaths(std::span<const cairns::PrefabId> new_span,
                         std::span<const std::filesystem::path> glbs) {
        // Skipped/failed parses don't append a prefab_id, so the input
        // glbs and the resulting new_span can disagree in length. Walk
        // glbs in order, appending only the ones we know succeeded by
        // iterating new_span in lockstep.
        glb_paths_.reserve(glb_paths_.size() + new_span.size());
        for (size_t i = 0; i < new_span.size() && i < glbs.size(); ++i) {
            glb_paths_.push_back(glbs[i]);
        }
    }

    // Register each new prefab as an AssetId so InstantiatePrefab can
    // resolve per_prefab_asset_[prefab_idx] without a bounds-check fail.
    // The InstantiatePrefab bounds check at engine.hpp:223 indexes this
    // array; a forgotten append here is the L10 silent failure that
    // returned UINT32_MAX / entity:0.
    void StampPerPrefabAsset(std::span<const cairns::PrefabId> new_span) {
        per_prefab_asset_.reserve(prefab_ids_.size());
        for (cairns::PrefabId sid : new_span) {
            cairns::Prefab::Hot* shot = prefabs_.GetHot(sid);
            if (!shot || shot->meshes.empty()) {
                per_prefab_asset_.push_back(cairns::AssetId{});
                continue;
            }
            const cairns::Mesh::Hot* m0 =
                meshes_.GetHot(shot->meshes[0]);
            if (!m0) {
                per_prefab_asset_.push_back(cairns::AssetId{});
                continue;
            }
            const uint32_t prefab_idx =
                static_cast<uint32_t>(per_prefab_asset_.size());
            per_prefab_asset_.push_back(assets_.RegisterExistingScene(
                prefab_idx, sid,
                m0->posHandle, m0->attrHandle, m0->indexHandle));
        }
    }

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
            std::span<const float> per_actor_extents) {
        std::vector<glm::mat4> out;
        if (n_total == 0) {
            return out;
        }
        out.reserve(n_total);
        const uint32_t cols = static_cast<uint32_t>(
            std::max(1.0f, std::ceil(std::sqrt(
                              static_cast<float>(n_total)))));
        const uint32_t rows = (n_total + cols - 1u) / cols;
        // Camera: y-axis FOV default 90deg (matches engine.hpp:~1370).
        // Aspect = active viewport target dims; depth Z = -4 world units
        // (matches the pre-#224 GenerateDebugGridTransforms convention).
        const float fov_y = static_cast<float>(M_PI) * 0.5f;
        float aspect = 16.0f / 9.0f;
        if (final_target_h_ > 0) {
            aspect = static_cast<float>(final_target_w_) /
                     static_cast<float>(final_target_h_);
        }
        const float depth = 4.0f;
        // kFitMargin shrinks the GRID extent so the outermost characters
        // get margin between their bind-pose AABB edge and the viewport
        // edge. (Animated poses extend beyond bind extent; at large N the
        // pre-margin grid spanned the full viewport and characters at the
        // edges clipped.) cell_size's 0.85 scales the CHARACTER within
        // its cell, independent of this.
        const float kFitMargin = 0.85f;
        const float visible_h = 2.0f * std::tan(fov_y * 0.5f) * depth * kFitMargin;
        const float visible_w = visible_h * aspect;
        const float cell_w = visible_w / static_cast<float>(cols);
        const float cell_h = visible_h / static_cast<float>(rows);
        const float cell_size = std::min(cell_w, cell_h) * 0.85f;
        const float start_x = -cell_w * (static_cast<float>(cols - 1u) * 0.5f);
        const float start_y = -cell_h * (static_cast<float>(rows - 1u) * 0.5f);
        for (uint32_t i = 0; i < n_total; ++i) {
            const uint32_t row = i / cols;
            const uint32_t col = i % cols;
            const float x = start_x + static_cast<float>(col) * cell_w;
            const float y = start_y + static_cast<float>(row) * cell_h;
            // Per-actor scale: cell_size / extent so each model fills the
            // same on-screen cell regardless of its raw GLB size. Extent
            // 0 (unknown / missing AABB) falls back to a small fixed scale.
            const float extent = (i < per_actor_extents.size() &&
                                   per_actor_extents[i] > 0.0f)
                                     ? per_actor_extents[i] : 100.0f;
            const float scale = cell_size / extent;
            glm::mat4 m(1.0f);
            m = glm::translate(m, glm::vec3(x, y, -depth));
            m = glm::scale(m, glm::vec3(scale));
            out.push_back(m);
        }
        return out;
    }

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
             pi < static_cast<uint32_t>(prefab_ids_.size()); ++pi) {
            cairns::Prefab::Hot* shot = prefabs_.GetHot(prefab_ids_[pi]);
            if (!shot) {
                continue;
            }
            for (uint32_t mi = 0;
                 mi < static_cast<uint32_t>(shot->meshes.size()); ++mi) {
                cairns::Mesh::Hot* mhot = meshes_.GetHot(shot->meshes[mi]);
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
            std::span<const PrefabHandleSnapshot> prior) {
        uint32_t mismatches = 0;
        for (const PrefabHandleSnapshot& p : prior) {
            if (p.prefab_idx >= prefab_ids_.size()) {
                ++mismatches; continue;
            }
            cairns::Prefab::Hot* shot =
                prefabs_.GetHot(prefab_ids_[p.prefab_idx]);
            if (!shot || p.mesh_idx >= shot->meshes.size()) {
                ++mismatches; continue;
            }
            cairns::Mesh::Hot* mhot =
                meshes_.GetHot(shot->meshes[p.mesh_idx]);
            if (!mhot) {
                ++mismatches; continue;
            }
            if (mhot->posHandle.index != p.pos_idx ||
                mhot->posHandle.generation != p.pos_gen ||
                mhot->attrHandle.index != p.attr_idx ||
                mhot->attrHandle.generation != p.attr_gen ||
                mhot->indexHandle.index != p.idx_idx ||
                mhot->indexHandle.generation != p.idx_gen ||
                mhot->batch_id != p.batch_id ||
                mhot->global_base_vertex != p.global_base_vertex) {
                ++mismatches;
            }
        }
        return mismatches;
    }

    // #224 L3: instrument accessors.
    const cairns::LoadTrace& LastLoadTrace() const { return last_load_trace_; }
    const cairns::ValidationReport& LastValidationReport() const {
        return last_validation_report_;
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
        cairns::LoaderCounters c = loader_counters_;
        if (cairns::Scene::Cold* wc = scenes_.GetCold(active_scene_)) {
            c.actors_live = static_cast<uint32_t>(
                wc->registry.storage<entt::entity>().size());
        }
        c.textures_resident =
            static_cast<uint32_t>(resident_textures_.size());
        return c;
    }

    // #269: bind-pose extent (max axis component of aabb_max - aabb_min)
    // of the scene's first skinned mesh -- the unit a caller normalizes
    // to when picking per-actor scale so heroes occupy a uniform cell
    // on screen. Returns 0 if scene_idx is out of range, no mesh has a
    // bind-pose AABB, or the AABB is degenerate.
    float PrefabExtentMax(uint32_t scene_idx) {
        if (scene_idx >= prefab_ids_.size()) {
            return 0.0f;
        }
        cairns::Prefab::Hot* shot = prefabs_.GetHot(prefab_ids_[scene_idx]);
        if (!shot) {
            return 0.0f;
        }
        for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
            cairns::Mesh::Hot* mh = meshes_.GetHot(mid);
            if (!mh) {
                continue;
            }
            if (mh->bind_aabb_min.x > mh->bind_aabb_max.x) {
                continue;
            }
            const glm::vec3 ext = mh->bind_aabb_max - mh->bind_aabb_min;
            return std::max(ext.x, std::max(ext.y, ext.z));
        }
        return 0.0f;
    }

    // Bind-pose AABB center of the prefab's first mesh, in mesh-local space.
    // Champions have their origin at the feet, so a fit that anchors the
    // origin pushes the body out the top of frame; subtract this to center.
    glm::vec3 PrefabAabbCenter(uint32_t scene_idx) {
        if (scene_idx >= prefab_ids_.size()) {
            return glm::vec3(0.0f);
        }
        cairns::Prefab::Hot* shot = prefabs_.GetHot(prefab_ids_[scene_idx]);
        if (!shot) {
            return glm::vec3(0.0f);
        }
        for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
            cairns::Mesh::Hot* mh = meshes_.GetHot(mid);
            if (!mh) {
                continue;
            }
            if (mh->bind_aabb_min.x > mh->bind_aabb_max.x) {
                continue;
            }
            return (mh->bind_aabb_min + mh->bind_aabb_max) * 0.5f;
        }
        return glm::vec3(0.0f);
    }

    // #269: list every live entity in active_scene_'s registry. The
    // values are entt::to_integral(entity), the same encoding InstantiatePrefab
    // returns. Caller pairs them with SetEntityTransform to drive a
    // no-flash relayout when the spawn count grows.
    std::vector<uint32_t> ListActiveSceneEntities() {
        std::vector<uint32_t> out;
        cairns::Scene::Cold* wc = scenes_.GetCold(active_scene_);
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
    // Returns false if entity isn't live in active_scene_'s registry.
    bool SetEntityTransform(uint32_t entity_int, const glm::mat4& world) {
        cairns::Scene::Cold* wc = scenes_.GetCold(active_scene_);
        if (!wc) {
            return false;
        }
        auto& reg = wc->registry;
        const entt::entity e = static_cast<entt::entity>(entity_int);
        if (!reg.valid(e) || !reg.all_of<cairns::WorldTransform>(e)) {
            return false;
        }
        reg.get<cairns::WorldTransform>(e).world = world;
        if (auto* wh = scenes_.GetHot(active_scene_)) {
            wh->dirty = true;
        }
        return true;
    }

    uint32_t ClearActiveScene() {
        cairns::Scene::Cold* wc = scenes_.GetCold(active_scene_);
        if (!wc) {
            return 0;
        }
        auto& reg = wc->registry;
        const uint32_t n =
            static_cast<uint32_t>(reg.storage<entt::entity>().size());
        reg.clear();
        if (auto* wh = scenes_.GetHot(active_scene_)) {
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
    bool ReloadPrefab(uint32_t idx, const std::filesystem::path& path) {
        if (idx >= prefab_ids_.size()) {
            return false;
        }
        // Phase D / R1 instrumentation. Pair with the Instruments
        // signpost so the time profiler bands reload work distinctly
        // from steady-state frames.
        CAIRNS_PRINT_ERR("[RELOAD] begin idx=%u path=%s\n", idx,
                          path.filename().c_str());
        CAIRNS_SIGNPOST_INTERVAL_SCOPED("reload_prefab",
                                         path.filename().c_str());
        cairns::PrefabId oldId = prefab_ids_[idx];
        // Snapshot the old prefab's owned resources BEFORE LoadPrefabBatch
        // -- it may grow the prefabs_/meshes_/materials_ pools' backing
        // vectors and invalidate held Hot/Cold pointers.
        std::vector<cairns::Handle<cairns::Mesh>> old_meshes;
        std::vector<cairns::Handle<cairns::Material>> old_materials;
        std::vector<rhi::Handle<rhi::Texture>> old_textures;
        std::vector<rhi::Handle<rhi::Sampler>> old_samplers;
        {
            cairns::Prefab::Hot* oldH = prefabs_.GetHot(oldId);
            cairns::Prefab::Cold* oldC = prefabs_.GetCold(oldId);
            if (!oldH || !oldC) {
                return false;
            }
            old_meshes = oldH->meshes;
            old_materials = oldH->materials;
            old_textures = oldC->textureHandles;
            old_samplers = oldC->samplerHandles;
        }
        std::array<std::filesystem::path, 1> single_path{path};
        LoadPrefabBatchResult r = RuntimeLoadBatch(single_path);
        if (r.count != 1) {
            return false;
        }
        cairns::PrefabId newId = prefab_ids_.back();
        // Re-fetch after the load: the pool's backing vector may have
        // grown, invalidating any pointer obtained pre-RuntimeLoadBatch.
        cairns::Prefab::Hot* oldH = prefabs_.GetHot(oldId);
        cairns::Prefab::Cold* oldC = prefabs_.GetCold(oldId);
        cairns::Prefab::Hot* newH = prefabs_.GetHot(newId);
        cairns::Prefab::Cold* newC = prefabs_.GetCold(newId);
        if (!oldH || !oldC || !newH || !newC) {
            return false;
        }
        *oldH = std::move(*newH);
        *oldC = std::move(*newC);
        prefabs_.Release(newId);
        prefab_ids_.pop_back();
        per_prefab_asset_.pop_back();
        glb_paths_.pop_back();
        glb_paths_[idx] = path;
        // DeferFree the snapshotted old resources. F1 (v2) stamps
        // retire_frame = current_frame_index + kFIF; drain happens at
        // frame >= retire_frame's start, after the fence proves the
        // last-referencing frame is GPU-done.
        for (rhi::Handle<rhi::Texture> th : old_textures) {
            rhi_.resources.DeferFree(rhi_.alloc, th);
        }
        for (rhi::Handle<rhi::Sampler> sh : old_samplers) {
            rhi_.resources.DeferFree(sh);
        }
        for (cairns::Handle<cairns::Mesh> mh : old_meshes) {
            cairns::Mesh::Hot* mhot = meshes_.GetHot(mh);
            if (mhot) {
                rhi_.resources.DeferFree(rhi_.alloc, mhot->posHandle);
                rhi_.resources.DeferFree(rhi_.alloc, mhot->attrHandle);
                rhi_.resources.DeferFree(rhi_.alloc, mhot->indexHandle);
                if (!mhot->skin_group_a.IsNull()) {
                    rhi_.resources.DeferFree(mhot->skin_group_a);
                }
            }
            meshes_.Release(mh);
        }
        for (cairns::Handle<cairns::Material> matid : old_materials) {
            cairns::Material::Hot* mathot = materials_.GetHot(matid);
            if (mathot && !mathot->set2.IsNull()) {
                rhi_.resources.DeferFree(mathot->set2);
            }
            materials_.Release(matid);
        }
        // resident_textures_ was populated by AppendGlbPaths' sibling
        // BuildResidentTextures during boot/load. The old prefab's
        // texture handles are now stale (Release happens at F1 drain
        // kFIF frames later); per-frame draw uses resident_textures_
        // verbatim, so we must rebuild it from the current set of live
        // prefab textureHandles before the next render reads it.
        resident_textures_.clear();
        for (cairns::PrefabId pid : prefab_ids_) {
            cairns::Prefab::Cold* pc = prefabs_.GetCold(pid);
            if (!pc) {
                continue;
            }
            for (rhi::Handle<rhi::Texture> th : pc->textureHandles) {
                resident_textures_.push_back(th);
            }
        }
        CAIRNS_PRINT_ERR("[RELOAD] end idx=%u path=%s ok\n", idx,
                          path.filename().c_str());
        return true;
    }

    // Convenience: look up |path| in glb_paths_ and reload the matching
    // prefab in place. Matches by full path OR by basename so callers
    // can pass "aatrox.glb" instead of the resolved absolute path that
    // glb_paths_ stores.
    bool ReloadPrefabByPath(const std::filesystem::path& path) {
        const std::filesystem::path lookup_name = path.filename();
        for (uint32_t i = 0; i < glb_paths_.size(); ++i) {
            if (glb_paths_[i] == path ||
                glb_paths_[i].filename() == lookup_name) {
                return ReloadPrefab(i, glb_paths_[i]);
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
    bool ReloadPipelineByName(const std::string& name) {
        const std::string shader_dir = cairns::GetBasePathSafe();
        if (name == "anim_eval") {
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "anim_eval";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "anim_eval";
            desc.layout = rhi::ComputePipelineLayout::kAnimEval;
            rhi::Handle<rhi::Kernel> next =
                rhi_.pipelines.CreateComputePipeline(rhi_.resources,
                                                       rhi_.frames, desc);
            if (next.IsNull()) {
                CAIRNS_PRINT_ERR("[R2] anim_eval reload failed -- keeping "
                                 "last-good\n");
                return false;
            }
            if (!anim_eval_kernel_.IsNull()) {
                rhi_.resources.DeferFree(anim_eval_kernel_);
            }
            anim_eval_kernel_ = next;
            return true;
        }
        if (name == "skin") {
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "skin";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "skin_compute";
            desc.layout = rhi::ComputePipelineLayout::kSkin;
            rhi::Handle<rhi::Kernel> next =
                rhi_.pipelines.CreateComputePipeline(rhi_.resources,
                                                       rhi_.frames, desc);
            if (next.IsNull()) {
                CAIRNS_PRINT_ERR("[R2] skin reload failed -- keeping "
                                 "last-good\n");
                return false;
            }
            if (!skin_kernel_.IsNull()) {
                rhi_.resources.DeferFree(skin_kernel_);
            }
            skin_kernel_ = next;
            return true;
        }
        if (name == "particle") {
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "particle_compute";
            desc.layout = rhi::ComputePipelineLayout::kParticle;
            desc.dyn_set_0 = dyn_particle_parity_[0];
            rhi::Handle<rhi::Kernel> next =
                rhi_.pipelines.CreateComputePipeline(rhi_.resources,
                                                       rhi_.frames, desc);
            if (next.IsNull()) {
                CAIRNS_PRINT_ERR("[R2] particle reload failed -- keeping "
                                 "last-good\n");
                return false;
            }
            if (!particle_kernel_.IsNull()) {
                rhi_.resources.DeferFree(particle_kernel_);
            }
            particle_kernel_ = next;
            return true;
        }
        return false;
    }

    // #228 F2: Evict. Manifest 'remove' verb over the WHOLE batch span --
    // drops every currently resident prefab, defer-frees its GPU
    // resources (textures, samplers, meshes' position/index/attr buffers,
    // materials' bind groups), releases pool slots, and resets every
    // state array touched by the manifest (per_prefab_asset_, glb_paths_,
    // resident_textures_, per_batch_shared_skin_) plus the anim cursors
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
    uint32_t UnloadAllPrefabs() {
        const uint32_t n = static_cast<uint32_t>(prefab_ids_.size());
        if (n == 0) {
            return 0;
        }
        for (cairns::PrefabId pid : prefab_ids_) {
            cairns::Prefab::Hot* phot = prefabs_.GetHot(pid);
            cairns::Prefab::Cold* pcold = prefabs_.GetCold(pid);
            if (pcold) {
                for (rhi::Handle<rhi::Texture> th : pcold->textureHandles) {
                    rhi_.resources.DeferFree(rhi_.alloc, th);
                }
                for (rhi::Handle<rhi::Sampler> sh : pcold->samplerHandles) {
                    rhi_.resources.DeferFree(sh);
                }
            }
            if (phot) {
                for (cairns::Handle<cairns::Mesh> mh : phot->meshes) {
                    cairns::Mesh::Hot* mhot = meshes_.GetHot(mh);
                    if (mhot) {
                        rhi_.resources.DeferFree(rhi_.alloc, mhot->posHandle);
                        rhi_.resources.DeferFree(rhi_.alloc, mhot->attrHandle);
                        rhi_.resources.DeferFree(rhi_.alloc, mhot->indexHandle);
                        if (!mhot->skin_group_a.IsNull()) {
                            rhi_.resources.DeferFree(mhot->skin_group_a);
                        }
                    }
                    meshes_.Release(mh);
                }
                for (cairns::Handle<cairns::Material> matid : phot->materials) {
                    cairns::Material::Hot* mathot = materials_.GetHot(matid);
                    if (mathot && !mathot->set2.IsNull()) {
                        rhi_.resources.DeferFree(mathot->set2);
                    }
                    materials_.Release(matid);
                }
            }
            prefabs_.Release(pid);
        }
        for (rhi::Handle<rhi::Buffer> sb : per_batch_shared_skin_) {
            if (!sb.IsNull()) {
                rhi_.resources.DeferFree(rhi_.alloc, sb);
            }
        }
        per_batch_shared_skin_.clear();
        prefab_ids_.clear();
        per_prefab_asset_.clear();
        glb_paths_.clear();
        resident_textures_.clear();
        // Anim: reset cursors so the next upload starts fresh against
        // unallocated capacity. The 4x growth pad still holds so the
        // first post-Unload load triggers FULL once, then DELTA after.
        anim_uploaded_prefab_count_ = 0;
        anim_cur_ = {};
        anim_eval_tables_uploaded_ = false;
        anim_dyn_dirty_ = true;
        return n;
    }

    // P1 input surface for main.cpp. Both no-op under CAIRNS_CAM_POSE so a
    // byte-gate dump can't be perturbed by an event that snuck through.

    // move_input.x = right(+) / left(-), .y = up(+) / down(-),
    // .z = forward(+) / back(-). Caller multiplies by dt + speed.
    void ApplyFlyMovement(const glm::vec3& move_input) {
        if (cam_pose_override_) {
            return;
        }
        cairns::FlyController& fc =
            viewports_.GetCold(active_viewport_)->fly;
        const float cy = std::cos(fc.yaw);
        const float sy = std::sin(fc.yaw);
        const float cp = std::cos(fc.pitch);
        const float sp = std::sin(fc.pitch);
        const glm::vec3 forward(-cp * sy, sp, -cp * cy);
        // right = normalize(cross(forward, world_up)). Closed-form with
        // world_up=(0,1,0): right = (cy, 0, -sy) (independent of pitch).
        // At yaw=0,pitch=0 this is (1,0,0): +X is screen-right while looking
        // down -Z. Backend-agnostic -- vk's negative-height viewport flips
        // only Y, not X, so metal and vk see the same horizontal motion.
        const glm::vec3 right(cy, 0.0f, -sy);
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        fc.position += right * move_input.x + up * move_input.y +
                        forward * move_input.z;
    }

    void ApplyMouseLook(float dyaw, float dpitch) {
        if (cam_pose_override_) {
            return;
        }
        cairns::FlyController& fc =
            viewports_.GetCold(active_viewport_)->fly;
        fc.yaw += dyaw;
        // Clamp pitch just inside +/-pi/2 so forward never becomes degenerate.
        constexpr float kPitchLimit = 1.55334f;
        fc.pitch = std::clamp(fc.pitch + dpitch, -kPitchLimit, kPitchLimit);
    }

    bool CamPoseOverridden() const { return cam_pose_override_; }

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
    void EnableParticles(bool on) { particles_enabled_ = on; }
    // A.3 single red NDC triangle (no scene): pipeline + clear + one draw.
    void SetTinyTriangle(bool on) { tiny_quad_test_ = on; }
    // G4: compose color + resolved-depth + extra-camera passes (the viewports
    // supplying the extra cameras are opened/aimed by the caller in JS).
    void SetNestedGraphMode(bool on) { nested_graph_mode_ = on; }
    bool ParticlesEnabled() const { return particles_enabled_; }

    // A.9: drop the implicit "no imgui in golden" gate. G6 imgui stability
    // scenario flips this on so the HUD/overlay renders into the golden
    // capture. Implicit constraint: SetInjectedHudStats should be called
    // alongside this so the rendered HUD numbers are byte-stable
    // (HudStats::Mock() = 60 fps flat).
    void SetImguiInGolden(bool on) { imgui_in_golden_ = on; }
    bool ImguiInGolden() const { return imgui_in_golden_; }

    // A.12: read the currently-bound particle ssbo bytes for the G1
    // cross-platform buffer SECTION. Gated on A.10 (Resources::ReadBackBuffer)
    // being implemented -- today returns false so G1 buffer SECTION SKIPs
    // honestly. When ReadBackBuffer lands, this resolves
    // particle_ssbo_[latest_parity_out_] and copies its bytes into `out`.
    bool ReadParticleBuffer(std::vector<uint8_t>& out) {
        if (particle_ssbo_[latest_parity_out_].IsNull()) {
            return false;
        }
        return rhi_.resources.ReadBackBuffer(
            rhi_.alloc, particle_ssbo_[latest_parity_out_],
            kParticleCount * static_cast<uint32_t>(sizeof(Particle)), out);
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
        return index == 1 ? secondary_scene_ : primary_scene_;
    }
    // Retarget where subsequent InstantiatePrefab* spawn (0 primary, 1 secondary).
    void UseScene(uint32_t index) { active_scene_ = SceneByIndex(index); }
    bool SetViewportScene(int vp, uint32_t index) {
        if (vp < 0 || vp >= active_viewport_count_) {
            return false;
        }
        if (cairns::Viewport::Hot* vh = viewports_.GetHot(viewport_ids_[vp])) {
            vh->scene = SceneByIndex(index);
            vh->camera_dirty = true;
            return true;
        }
        return false;
    }
    bool SetViewportParticles(int vp, bool on) {
        if (vp < 0 || vp >= active_viewport_count_) {
            return false;
        }
        if (cairns::Viewport::Cold* vc = viewports_.GetCold(viewport_ids_[vp])) {
            vc->particles_enabled = on;
            return true;
        }
        return false;
    }
    bool SetViewportCamera(int vp, const glm::vec3& pos, float yaw,
                           float pitch) {
        if (vp < 0 || vp >= active_viewport_count_) {
            return false;
        }
        if (cairns::Viewport::Cold* vc = viewports_.GetCold(viewport_ids_[vp])) {
            vc->fly.position = pos;
            vc->fly.yaw = yaw;
            vc->fly.pitch = pitch;
        }
        if (cairns::Viewport::Hot* vh = viewports_.GetHot(viewport_ids_[vp])) {
            vh->camera_dirty = true;
        }
        return true;
    }
    // Load + normalize-to-frame + center `instances` actors cycling over `glbs`,
    // spawned into the active scene. The general fit-place primitive (#224).
    bool SpawnFitted(const std::vector<std::string>& glbs, uint32_t instances,
                     bool animated) {
        if (glbs.empty() || instances == 0) {
            return true;
        }
        std::vector<uint32_t> pidx;
        pidx.reserve(glbs.size());
        for (const std::string& name : glbs) {
            const uint32_t idx =
                cairns::headless::RuntimeLoadGlbPath(this, name);
            if (idx == UINT32_MAX) {
                return false;
            }
            pidx.push_back(idx);
        }
        std::vector<float> extents(instances);
        for (uint32_t i = 0; i < instances; ++i) {
            extents[i] = PrefabExtentMax(pidx[i % pidx.size()]);
        }
        const std::vector<glm::mat4> worlds =
            FitGridToViewport(instances, extents);
        for (uint32_t i = 0; i < instances; ++i) {
            const uint32_t scene_idx = pidx[i % pidx.size()];
            const glm::vec3 center = PrefabAabbCenter(scene_idx);
            const glm::mat4 world =
                worlds[i] * glm::translate(glm::mat4(1.0f), -center);
            const uint32_t out = animated
                ? InstantiatePrefab(scene_idx, world, /*time_phase=*/0.0f)
                : InstantiatePrefabNoSkin(scene_idx, world);
            if (out == UINT32_MAX) {
                return false;
            }
        }
        return true;
    }
    bool AdvanceFrames(uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) {
            if (!RenderHeadlessFrame()) {
                return false;
            }
        }
        return true;
    }

    // #194 runtime viewport management. #220 Step 4: handle-pilled +
    // vpN wire-name layer.
    int ActiveViewportCount() const { return active_viewport_count_; }

    // Returns the engine-assigned monotonic name counter ("vp{N}" without
    // the prefix) or UINT32_MAX if at kNumViewports cap. Names are never
    // reused for the lifetime of the engine process (locked sub-decision
    // in plan #220 Step 4). The RPC layer formats the result as "vp{N}"
    // on the wire.
    uint32_t OpenViewport() {
        if (active_viewport_count_ >= kNumViewports) {
            return UINT32_MAX;
        }
        cairns::ViewportId id = viewports_.Acquire();
        // Reused-slot trap: re-init both halves so a previously-released
        // slot doesn't carry over.
        if (auto* h = viewports_.GetHot(id)) {
            *h = cairns::Viewport::Hot{};
            h->scene = active_scene_;
        }
        if (auto* c = viewports_.GetCold(id)) {
            *c = cairns::Viewport::Cold{};
        }
        const int idx = active_viewport_count_++;
        viewport_ids_[idx] = id;
        // Default rect: uniform tile across the swap pane until the agent
        // calls setLayout. Tiles add up to the full pane.
        const float w = 1.0f / static_cast<float>(active_viewport_count_);
        for (int v = 0; v < active_viewport_count_; ++v) {
            viewports_.GetHot(viewport_ids_[v])->layout_rect =
                glm::vec4(w * static_cast<float>(v), 0.0f, w, 1.0f);
        }
        const uint32_t name = next_viewport_name_++;
        // Append-sorted: next_viewport_name_ is monotonic so the new
        // counter is always the largest seen.
        viewport_names_[viewport_names_count_++] = ViewportName{name, id};
        return name;
    }

    // Close the highest-index viewport. Returns false if at 1 (cannot drop
    // below 1). Existing RPC takes no argument; this targets the last
    // opened viewport for backward compat with the protocol.
    bool CloseViewport() {
        if (active_viewport_count_ <= 1) {
            return false;
        }
        const int last_idx = active_viewport_count_ - 1;
        cairns::ViewportId id = viewport_ids_[last_idx];
        viewports_.GetHot(id)->layout_rect = glm::vec4(0.0f);
        viewport_ids_[last_idx] = cairns::ViewportId::Null;
        --active_viewport_count_;
        // Drop the name table entry for this id. The counter itself
        // remains burned (never reused) per the locked sub-decision.
        for (uint8_t i = 0; i < viewport_names_count_; ++i) {
            if (viewport_names_[i].id.index == id.index &&
                viewport_names_[i].id.generation == id.generation) {
                for (uint8_t j = i; j + 1 < viewport_names_count_; ++j) {
                    viewport_names_[j] = viewport_names_[j + 1];
                }
                --viewport_names_count_;
                break;
            }
        }
        viewports_.Release(id);
        const float w = 1.0f / static_cast<float>(active_viewport_count_);
        for (int v = 0; v < active_viewport_count_; ++v) {
            viewports_.GetHot(viewport_ids_[v])->layout_rect =
                glm::vec4(w * static_cast<float>(v), 0.0f, w, 1.0f);
        }
        return true;
    }

    // Old int-indexed setLayout kept as a slot-position API for in-engine
    // callers; the wire layer uses SetViewportLayoutByName.
    bool SetViewportLayout(int viewport, glm::vec4 rect) {
        if (viewport < 0 || viewport >= active_viewport_count_) {
            return false;
        }
        viewports_.GetHot(viewport_ids_[viewport])->layout_rect = rect;
        return true;
    }

    // #220 Step 4 wire-name layer. Parse "vp{N}" -> counter -> binary
    // search the sorted name table -> ViewportId. Returns Null on miss.
    cairns::ViewportId ResolveViewportName(uint32_t counter) const {
        int lo = 0;
        int hi = static_cast<int>(viewport_names_count_);
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            const uint32_t k = viewport_names_[mid].counter;
            if (k == counter) {
                return viewport_names_[mid].id;
            }
            if (k < counter) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return cairns::ViewportId::Null;
    }

    int FindViewportSlot(cairns::ViewportId id) const {
        for (int i = 0; i < active_viewport_count_; ++i) {
            if (viewport_ids_[i].index == id.index &&
                viewport_ids_[i].generation == id.generation) {
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
        viewports_.GetHot(id)->layout_rect = rect;
        return true;
    }

    void SetActiveViewportSlot(int idx) {
        if (idx < 0 || idx >= active_viewport_count_) {
            return;
        }
        active_viewport_index_ = idx;
        active_viewport_ = viewport_ids_[idx];
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

    const std::vector<cairns::SelectionTarget>& Selection() const { return selection_; }
    const std::vector<cairns::SelectionTarget>& Highlights() const { return highlights_; }
    uint32_t SelectionRevision() const { return selection_rev_; }

    void ClearSelection() {
        if (!selection_.empty()) {
            selection_.clear();
            ++selection_rev_;
        }
    }
    void SetSelection(std::vector<cairns::SelectionTarget>&& targets) {
        selection_ = std::move(targets);
        ++selection_rev_;
    }
    void AddSelection(const cairns::SelectionTarget& t) {
        for (const auto& s : selection_) {
            if (s == t) {
                return;
            }
        }
        selection_.push_back(t);
        ++selection_rev_;
    }
    void RemoveSelection(const cairns::SelectionTarget& t) {
        for (size_t i = 0; i < selection_.size(); ++i) {
            if (selection_[i] == t) {
                selection_.erase(selection_.begin() + static_cast<long>(i));
                ++selection_rev_;
                return;
            }
        }
    }

    void ClearHighlights() {
        if (!highlights_.empty()) {
            highlights_.clear();
            ++highlights_rev_;
        }
    }
    void SetHighlights(std::vector<cairns::SelectionTarget>&& targets) {
        highlights_ = std::move(targets);
        ++highlights_rev_;
    }

    // Window-pixel coords. Engine doesn't resolve the pick yet -- the GPU
    // ID buffer + readback land in a follow-up; this just records the
    // request so a future RecordFrame can copy the texel out and a future
    // tick can deliver the resolved entity.
    void RequestPick(int viewport, uint32_t x, uint32_t y) {
        pick_pending_ = true;
        pick_viewport_ = viewport;
        pick_x_ = x;
        pick_y_ = y;
    }
    bool PickPending() const { return pick_pending_; }
    int PendingPickViewport() const { return pick_viewport_; }
    uint32_t PendingPickX() const { return pick_x_; }
    uint32_t PendingPickY() const { return pick_y_; }

    // Last resolved pick. Updated by ResolvePendingPick once per frame when
    // pick_pending_ was true at the top of the frame. PickResolved() flips
    // true on the frame the readback completes; ConsumePickResult()
    // atomically reads + clears so each request returns exactly one result.
    struct PickResult {
        int viewport = 0;
        uint32_t x = 0;
        uint32_t y = 0;
        cairns::SelectionType type = cairns::SelectionType::kEntity;
        uint32_t id = 0;
        // Today's stub source: final_target_ BGRA at (x, y). Swap to the
        // R32U id_target once #206 lands the dedicated ID buffer; the
        // {type, id} decode swaps with it.
        uint32_t raw = 0;
    };
    bool PickResolved() const { return pick_resolved_; }
    PickResult ConsumePickResult() {
        pick_resolved_ = false;
        return last_pick_result_;
    }

    // Click-to-focus: caller passes the window-x of the LMB click. Engine
    // picks the half of the swap target the click lands in. fly_/keyboard
    // input is then routed to that viewport on subsequent iterates.
    void SetActiveViewportFromClickX(float window_x) {
        const float half = static_cast<float>(FrameWidth()) /
                            static_cast<float>(std::max(1, active_viewport_count_));
        SetActiveViewportSlot((window_x < half) ? 0 : 1);
    }
    int ActiveViewport() const { return active_viewport_index_; }

    // Override the deterministic-particles seed (default kept at 42 to match
    // the existing CAIRNS_DUMP byte-gate). Must be called before
    // GreaterInit's initParticles for the change to take effect.
    void SetRandomSeed(uint32_t seed) { random_seed_ = seed; }
    uint32_t GetRandomSeed() const { return random_seed_; }

    uint32_t GetFinalTargetWidth() const { return final_target_w_; }
    uint32_t GetFinalTargetHeight() const { return final_target_h_; }

    // Current logical frame dims. Windowed: tracks the swapchain. Surfaceless:
    // tracks final_target_. Single source of truth for aspect / screen_params /
    // ImGui DPI scaling -- all of which used to read swapchain_ directly and
    // were wrong by construction in surfaceless mode.
    uint32_t FrameWidth() const {
        return final_target_.IsNull() ? swapchain_.Width() : final_target_w_;
    }
    uint32_t FrameHeight() const {
        return final_target_.IsNull() ? swapchain_.Height() : final_target_h_;
    }

    // #207 (re)allocate per-viewport persistent R32U id targets if dims drift.
    // Called at the top of RecordFrame so each viewport's id_target_ matches
    // the current vp_w/vp_h. Destroys old targets through the rhi destroy
    // queue so any in-flight frame using the prior dims is unaffected.
    void EnsureIdTargets(uint32_t w, uint32_t h) {
        if (w == id_target_w_ && h == id_target_h_ &&
            !id_target_[0].IsNull()) {
            return;
        }
        for (int v = 0; v < kNumViewports; ++v) {
            if (!id_target_[v].IsNull()) {
                rhi_.resources.Destroy(rhi_.alloc, id_target_[v]);
                id_target_[v] = rhi::Handle<rhi::Texture>::Null;
            }
        }
        id_target_w_ = w;
        id_target_h_ = h;
        for (int v = 0; v < kNumViewports; ++v) {
            rhi::TextureDesc td{};
            td.debug_name = "id_target";
            td.dimensions = {static_cast<int32_t>(w),
                             static_cast<int32_t>(h), 1};
            td.format = rhi::Format::kR32Uint;
            td.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled |
                       rhi::kTexUsageTransferSrc;
            td.memory = rhi::Memory::kDefault;
            id_target_[v] = rhi_.resources.CreateTexture(rhi_.alloc, td);
        }
    }

    // #207 (re)build the highlights texture from highlights_. Called by
    // RecordFrame each frame; if the rev hasn't changed, no-op. On change,
    // destroys the prior texture through the rhi destroy queue and creates
    // a fresh 65x1 R32U with the new pack. Empty highlight set still
    // produces a valid texture (count=0) so the outline frag's descriptor
    // binding is always satisfied -- the early-out in id_in_highlights
    // keeps it cheap.
    void EnsureHighlightsTex() {
        if (!highlights_tex_.IsNull() &&
            highlights_tex_rev_ == highlights_rev_) {
            return;
        }
        if (!highlights_tex_.IsNull()) {
            rhi_.resources.Destroy(rhi_.alloc, highlights_tex_);
            highlights_tex_ = rhi::Handle<rhi::Texture>::Null;
        }
        std::array<uint32_t, kMaxHighlights + 1> pack{};
        const uint32_t n =
            static_cast<uint32_t>(std::min<size_t>(highlights_.size(),
                                                   kMaxHighlights));
        pack[0] = n;
        for (uint32_t i = 0; i < n; ++i) {
            pack[i + 1] = highlights_[i].id;
        }
        rhi::TextureDesc td{};
        td.debug_name = "highlights_tex";
        td.dimensions = {static_cast<int32_t>(kMaxHighlights + 1), 1, 1};
        td.format = rhi::Format::kR32Uint;
        td.usage = rhi::kTexUsageSampled | rhi::kTexUsageTransferDst;
        td.memory = rhi::Memory::kDefault;
        td.initial_data = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(pack.data()),
            sizeof(uint32_t) * pack.size());
        highlights_tex_ = rhi_.resources.CreateTexture(rhi_.alloc, td);
        highlights_tex_rev_ = highlights_rev_;
    }

    // Reallocate final_target_ at the new dimensions. Surfaceless mode only.
    bool ResizeFinalTarget(uint32_t w, uint32_t h) {
        if (final_target_.IsNull()) {
            return false;
        }
        rhi_.resources.Destroy(rhi_.alloc, final_target_);
        final_target_ = rhi::Handle<rhi::Texture>::Null;
        final_target_w_ = w;
        final_target_h_ = h;
        rhi::TextureDesc td{};
        td.debug_name = "final_target";
        td.dimensions = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
        td.format = rhi::Format::kBgra8Unorm;
        td.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled |
                   rhi::kTexUsageTransferSrc | rhi::kTexUsageTransferDst;
        td.memory = rhi::Memory::kDefault;
        final_target_ = rhi_.resources.CreateTexture(rhi_.alloc, td);
        return !final_target_.IsNull();
    }

    // Surfaceless (cairns_serve) one-frame render: drives the windowed draw()
    // path once. Swap pass writes into final_target_; the engine builds a
    // SwapResolveTarget with no drawable so Frames neither acquires a
    // drawable nor presents one. Synchronous: render thread (if used)
    // drained before return; the metal/vulkan Frames::End waitUntilCompleted's
    // the render-to-texture path. Returns false if not surfaceless.
    bool RenderHeadlessFrame() {
        if (final_target_.IsNull()) {
            return false;
        }
        return draw();
    }

    // Test seam: surfaceless byte readback of the offscreen target. The
    // golden ladder hashes this buffer and compares to a per-platform ref.
    // Mirrors DumpFinalTarget's pipeline but skips the PNG encode.
    bool ReadFinalTargetRgba(std::vector<uint8_t>& rgba, uint32_t& w,
                              uint32_t& h) {
        if (final_target_.IsNull()) {
            return false;
        }
        return rhi_.resources.ReadBackTextureRgba(final_target_, rgba, w, h);
    }

    // Headless texture readback: blit final_target_ -> Shared buffer ->
    // PNG. Mirrors the windowed dump in metal/frames.cpp::End() but reads
    // from the offscreen target instead of the swapchain drawable. Apple
    // origin is top-left so no Y-flip needed (matches the windowed dump's
    // contract). BGRA -> RGBA swizzle on the host side.
    bool DumpFinalTarget(const std::filesystem::path& path) {
        if (final_target_.IsNull()) {
            return false;
        }
        std::vector<uint8_t> rgba;
        uint32_t w = 0;
        uint32_t h = 0;
        if (!rhi_.resources.ReadBackTextureRgba(final_target_, rgba, w, h)) {
            return false;
        }
        return stbi_write_png(path.string().c_str(), static_cast<int>(w),
                              static_cast<int>(h), 4, rgba.data(),
                              static_cast<int>(w * 4)) != 0;
    }
    
    bool initCpuAllocators() {
        for (PerSlot& s : slots_) {
            s.arena_storage.assign(kArenaBytesPerSlot, 0);
            s.arena.Init(s.arena_storage.data(), kArenaBytesPerSlot);
        }
        return true;
    }

    // #220 Step 4: acquire the initial viewport slot (vp0) and pre-fill
    // the layout / active selection. Called from GreaterInit before any
    // cam_pose override walks the viewport pool. Idempotent: bails if
    // viewport 0 is already live.
    void InitInitialViewport() {
        if (active_viewport_count_ > 0 && !viewport_ids_[0].IsNull()) {
            return;
        }
        cairns::ViewportId id = viewports_.Acquire();
        if (auto* h = viewports_.GetHot(id)) {
            *h = cairns::Viewport::Hot{};
            h->layout_rect = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
            h->scene = active_scene_;
        }
        if (auto* c = viewports_.GetCold(id)) {
            *c = cairns::Viewport::Cold{};
        }
        viewport_ids_[0] = id;
        active_viewport_count_ = 1;
        active_viewport_ = id;
        active_viewport_index_ = 0;
        const uint32_t name = next_viewport_name_++;
        viewport_names_[viewport_names_count_++] = ViewportName{name, id};
    }
    
    bool initResourceManagers() {
        using namespace cairns;
        using namespace cairns::rhi;
        // 4 because we're only pretending to be a real UGC engine at this point
        return true;
    }
    
    bool GreaterInit(const rhi::InitConfig& cfg, const EngineConfig& ecfg) {
        engine_cfg_ = ecfg;

        // #221 build_draws worker pool. Cap at 4 so M-series fan-out stays
        // on P-cores (M2 Max has 8P + 4E; hardware_concurrency() returns 12,
        // which pushed half the workers onto E-cores and ate the win).
        // hardware_concurrency() may return 0 if the OS can't report it --
        // clamp to 1 so the fan-out degenerates to a single-threaded pass.
        const unsigned hw = std::thread::hardware_concurrency();
        unsigned n = hw > 0 ? hw : 1u;
        if (n > 4u) {
            n = 4u;
        }
        build_pool_ = std::make_unique<cairns::WorkerPool>(n);
        // Clock selection: a dump_path OR an explicit use_fixed_clock => the
        // engine runs with FixedClock + readback enabled (golden_=true). Only
        // a non-empty dump_path also turns on the dump-frame-then-exit(0)
        // path (dump_and_exit_) -- so the test harness can ask for the
        // determinism without being killed.
        dump_and_exit_ = !engine_cfg_.dump_path.empty();
        golden_ = engine_cfg_.use_fixed_clock || dump_and_exit_;
        tiny_quad_test_ = engine_cfg_.tiny_quad;
        particles_enabled_ = engine_cfg_.particles_enabled;

        // #220 Step 4: viewport pool must be set up BEFORE the cam_pose
        // override walks it. InitInitialViewport acquires vp0 and primes
        // its layout / active_viewport_ / name table.
        InitInitialViewport();

        // Pin every viewport's fly controller to the override pose so byte-
        // gate dumps are deterministic. Pre-P1 reference pose is
        // (0,0,0,0,0). Diverging viewports for multi-pose byte-gates is the
        // P3 follow-up.
        if (engine_cfg_.cam_pose.has_value()) {
            const EngineConfig::CamPose& p = *engine_cfg_.cam_pose;
            for (int vi = 0; vi < active_viewport_count_; ++vi) {
                cairns::FlyController& fc =
                    viewports_.GetCold(viewport_ids_[vi])->fly;
                fc.position = glm::vec3(p.x, p.y, p.z);
                fc.yaw = p.yaw;
                fc.pitch = p.pitch;
            }
            cam_pose_override_ = true;
        }
        if (golden_) {
            clock_ = std::make_unique<cairns::FixedClock>(cairns::kFixedDt);
        } else {
            clock_ = std::make_unique<cairns::WallClock>();
        }

        // these initializations are wrong.
        // there is a dependency graph
        // alloc gpu mem -> upload cpu to gpu mem -> draw on gpu
        // but it should be:
        // generate commands to alloc gpu mem -> upload cpu to gpu mem -> generate draw commands
        // and this can be parallelized:
        // thread 1: generate commands to alloc gpu mem -> signal fence1 -> generate draw commands -> wait for fence2 -> execute draw commands
        // thread 2: wait for fence1 -> upload cpu to gpu mem -> signal fence2
        
        if ( !initCpuAllocators() ) {
            CAIRNS_PRINT("GreaterInit: initCpuAllocators failed\n");
            return false;
        }
        if ( !initResourceManagers() ) {
            CAIRNS_PRINT("GreaterInit: initResourceManagers failed\n");
            return false;
        }
        if (!rhi_.device.Init(cfg)) {
            CAIRNS_PRINT("GreaterInit: device.Init failed\n");
            return false;
        }
        // Boot device-cap invariant. The 2026-06-17 S22 garble (Adreno 730
        // maxStorageBufferRange = 256 MB, 1 GB pool bound past it -> silent
        // no-op writes) would have aborted right here with the exact log
        // line the bisect spent ~4 hours speculating toward. Android already
        // sizes kSkinOutputBytes at 256 MB (the landed fix); this is the
        // belt-and-braces check that survives a desktop-pool slip onto a
        // mobile device.
        {
// Mobile + iOS (incl. sim) report a 256 MB MTLDevice maxBufferLength /
// Adreno 730 reports 256 MB maxStorageBufferRange. Desktop reports >= 2 GB.
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
            constexpr uint32_t kSkinOutputBytesCheck = 128u * 1024u * 1024u;
#else
            constexpr uint32_t kSkinOutputBytesCheck = 1024u * 1024u * 1024u;
#endif
            if (!cairns::SkinPoolFitsDevice(kSkinOutputBytesCheck,
                                            rhi_.device.caps)) {
                CAIRNS_PRINT_ERR(
                    "[FATAL] kSkinOutputBytes=%u exceeds device "
                    "max_storage_buffer_range=%u. The skin pool would "
                    "be bound past the addressable range and writes "
                    "past it would silently no-op (Adreno 730 floor "
                    "is 256 MB).\n",
                    kSkinOutputBytesCheck,
                    rhi_.device.caps.max_storage_buffer_range);
                std::abort();
            }
        }
        if (!rhi_.alloc.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: alloc.Init failed\n");
            return false;
        }
        if (!rhi_.resources.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: resources.Init failed\n");
            return false;
        }
        // #221 Skinning Phase 3: persistent 256 MB skin output pool. Buffer
        // is private-heap (kDefault); RangePool measures slices in vec4
        // vertex units. Sized once at init; Phase 5 fails loudly on
        // exhaustion (Alloc returns invalid slice). >kHeapBlockBytes (128
        // MB) drops into the dedicated-block path in
        // MemoryAllocator::AllocBuffer, so we land in our own VkDeviceMemory.
        {
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
            // 128 MB: Adreno 730 (Samsung S22) actually reports
            // maxStorageBufferRange = 128 MB at runtime (the vk spec
            // floor), not the 256 MB earlier docs implied. iOS sim
            // MTLDevice maxBufferLength is 256 MB, but 128 fits all of
            // them.
            static constexpr uint32_t kSkinOutputBytes =
                128u * 1024u * 1024u;
#else
            static constexpr uint32_t kSkinOutputBytes =
                1024u * 1024u * 1024u;
#endif
            rhi::BufferDesc bd{};
            bd.byte_size = kSkinOutputBytes;
            bd.usage = rhi::kUsageStorage | rhi::kUsageVertex;
            bd.memory = rhi::Memory::kDefault;
            skin_output_pool_buffer_ = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
            if (skin_output_pool_buffer_.IsNull()) {
                CAIRNS_PRINT("GreaterInit: skin_output_pool_buffer_ alloc failed\n");
                return false;
            }
            // Slices in vec4 units (16 B). Capacity = total / 16.
            static_assert(sizeof(glm::vec4) == cairns::kSkinVertexStride,
                          "skin vertex stride must equal sizeof(glm::vec4); "
                          "RangePool offsets are scaled by kSkinVertexStride "
                          "on bind.");
            skin_output_pool_.Init(kSkinOutputBytes / 16u);
        }
        // #221 Phase 5b: persistent palette out + world scratch for GPU
        // palette eval. 1024 actors * 256 mat4 = 16 MB each.
        // #222 Phase 0.2: caps hoisted to class scope (kAnimActorsCap etc).
        {
            rhi::BufferDesc bd{};
            bd.usage = rhi::kUsageStorage;
            bd.memory = rhi::Memory::kDefault;
            bd.byte_size = kAnimActorsCap * kAnimMaxJoints * 64u;
            palette_out_buf_ = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
            bd.byte_size = kAnimActorsCap * kAnimMaxNodes * 64u;
            world_scratch_buf_ = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
            if (palette_out_buf_.IsNull() || world_scratch_buf_.IsNull()) {
                CAIRNS_PRINT("GreaterInit: anim_eval persistent buffers alloc failed\n");
                return false;
            }
        }
        // #222 Phase F.1/F.3/F.4: sibling subsystems init before frames.
        // Pipelines moves up here too (owns descriptor set layouts post-F.4).
        if (!rhi_.gpu_profiler.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: gpu_profiler.Init failed\n");
            return false;
        }
        rhi_.offscreen_targets.Init(rhi_.device);
        if (!rhi_.pipelines.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: pipelines.Init failed\n");
            return false;
        }
        if (!rhi_.frames.Init(rhi_.device, rhi_.pipelines)) {
            CAIRNS_PRINT("GreaterInit: frames.Init failed\n");
            return false;
        }
        // #222 Phase D.2: dyn_globals_ + dyn_drawtmp_ — per-FIF descriptor
        // sets for unlit's set 0 + set 2, backed by the kDynamic master.
        // Must run AFTER frames.Init (needs descriptor_pool_) and BEFORE
        // initRenderPipeline (the unlit PSO references their layout).
        {
            cairns::rhi::DynamicBinding gb{};
            gb.slot = 0;
            gb.kind = cairns::rhi::BufferKind::kUniform;
            gb.max_range = sizeof(cairns::rhi::RenderPassGlobals);
            gb.stages = static_cast<cairns::rhi::ShaderStage>(
                cairns::rhi::kStageVertex | cairns::rhi::kStageFragment);
            cairns::rhi::DynamicBuffersDesc gd{};
            gd.debug_name = "dyn_globals";
            gd.bindings =
                std::span<const cairns::rhi::DynamicBinding>(&gb, 1);
            dyn_globals_ = rhi_.resources.CreateDynamicBuffers(rhi_.alloc,
                                                                 rhi_.frames, gd);
            cairns::rhi::DynamicBinding db{};
            db.slot = 0;
            db.kind = cairns::rhi::BufferKind::kUniform;
            db.max_range = sizeof(cairns::rhi::DrawTmp);
            db.stages = static_cast<cairns::rhi::ShaderStage>(
                cairns::rhi::kStageVertex | cairns::rhi::kStageFragment);
            cairns::rhi::DynamicBuffersDesc dd{};
            dd.debug_name = "dyn_drawtmp";
            dd.bindings =
                std::span<const cairns::rhi::DynamicBinding>(&db, 1);
            dyn_drawtmp_ = rhi_.resources.CreateDynamicBuffers(rhi_.alloc,
                                                                 rhi_.frames, dd);
            if (dyn_globals_.IsNull() || dyn_drawtmp_.IsNull()) {
                CAIRNS_PRINT("GreaterInit: DynamicBuffers create failed\n");
                return false;
            }
        }
        // #222 Phase F.4: pipelines.Init moved earlier (now happens
        // before frames.Init); second call is the idempotent guard.
        if (!cfg.surfaceless) {
            if ( !initSwapChain(cfg)) {
                CAIRNS_PRINT("GreaterInit: initSwapChain failed\n");
                return false;
            }
        }
        { // init debug assets
            // #228 H3: glb_paths_ now appended by LoadPrefabBatch via
            // AppendGlbPaths (the manifest line). GreaterInit just
            // builds a local list of paths from CAIRNS_GLB overrides
            // (if any) and hands them to LoadPrefabBatch; the
            // [PICK] log's name-by-filename works for both boot-
            // override prefabs AND runtime cairns.prefab.load.
            std::vector<std::filesystem::path> glb_paths;
            if (!engine_cfg_.glb_overrides.empty()) {
                for (const std::string& tok : engine_cfg_.glb_overrides) {
                    if (tok.empty()) {
                        continue;
                    }
                    std::filesystem::path p(tok);
                    if (p.is_absolute()) {
                        glb_paths.push_back(p);
                    } else {
                        std::filesystem::path resolved;
                        if (!cairns::GetStaticResourceFilepath(tok, resolved)) {
                            return false;
                        }
                        glb_paths.push_back(resolved);
                    }
                }
            }
            // #224 L9: NO IMPLICIT BOOT LOAD. Default (empty
            // glb_overrides) leaves the prefab pool empty; the agent
            // calls cairns.prefab.load when it needs a Prefab. Boot is
            // sub-second.
            if (!glb_paths.empty()) {
                LoadPrefabBatch(std::span<const std::filesystem::path>(
                    glb_paths.data(), glb_paths.size()));
            }

            {
                uint64_t total_skin_verts = 0;
                uint32_t skin_mesh_count = 0;
                uint32_t max_vert_per_mesh = 0;
                meshes_.ForEachLive(
                    [&](cairns::Mesh::Hot& mhot, cairns::Mesh::Cold&) {
                        if (mhot.attr_skinned_alias.IsNull() ||
                            ResolvedSharedSkin(mhot).IsNull() ||
                            mhot.vert_count == 0) {
                            return;
                        }
                        total_skin_verts += mhot.vert_count;
                        ++skin_mesh_count;
                        if (mhot.vert_count > max_vert_per_mesh) {
                            max_vert_per_mesh = mhot.vert_count;
                        }
                    });
                uint32_t max_joints = 0;
                uint32_t max_nodes = 0;
                uint32_t scene_count = 0;
                prefabs_.ForEachLive(
                    [&](cairns::Prefab::Hot&, cairns::Prefab::Cold& c) {
                        ++scene_count;
                        if (c.nodes.size() > max_nodes) {
                            max_nodes = static_cast<uint32_t>(c.nodes.size());
                        }
                        for (const cairns::Skin& s : c.skins) {
                            if (s.jointNodes.size() > max_joints) {
                                max_joints = static_cast<uint32_t>(
                                    s.jointNodes.size());
                            }
                        }
                    });
                CAIRNS_PRINT(
                    "[WORKLOAD] scenes=%u skinned_meshes=%u total_skin_verts=%llu "
                    "max_vert_per_mesh=%u max_joints=%u max_nodes=%u\n",
                    scene_count, skin_mesh_count,
                    static_cast<unsigned long long>(total_skin_verts),
                    max_vert_per_mesh, max_joints, max_nodes);
            }

            // #224 L1: CleanupTmps + per-mesh CPU clear moved into
            // LoadPrefabBatch so subsequent batches get the same hygiene.
        }
        // #228 H3: mesh_master_handle_ deleted (mole #5). The field's
        // only consumer was the unread `MeshDrawList::resident_buffers`
        // field, itself dead code. Field + reader gone; this gate
        // (which was the last if(!prefab_ids_.empty()) block at engine
        // init) vanishes.

        // EnTT scene-layer path. Acquire active_scene_ + secondary_scene_
        // ALWAYS (regardless of prefab count), because InstantiatePrefab
        // looks up scenes_.GetCold(active_scene_) and bails to entity:0
        // if it's null. #224 L9 follow-up: was gated by
        // `if (!prefab_ids_.empty())` which is now false at boot.
        // Pre-allocate hot/cold cells up to kMaxScenes so Acquire doesn't
        // trigger a vector growth that would move Scene::Cold and
        // invalidate any cached pointers. The unique_ptr<entt::registry>
        // inside Cold is the second safety layer.
        for (uint32_t w = 0; w < kMaxScenes; ++w) {
            cairns::SceneId tmp = scenes_.Acquire();
            scenes_.Release(tmp);
        }
        active_scene_ = scenes_.Acquire();
        primary_scene_ = active_scene_;  // index-0; UseScene may move active_
        if (cairns::Scene::Hot* wh = scenes_.GetHot(active_scene_)) {
            if (cairns::Scene::Cold* wc =
                    scenes_.GetCold(active_scene_)) {
                *wc = cairns::Scene::Cold{};
                wh->proxy_slot = 0;
                wh->dirty = true;
            }
        }
        // InitInitialViewport() acquired vp0 BEFORE active_scene_ existed, so
        // its scene handle is stale-null. Bind it now that active_scene_ is
        // real -- the per-viewport draw fan-out (#195) extracts each viewport's
        // bound scene, so a stale bind renders nothing.
        for (int v = 0; v < active_viewport_count_; ++v) {
            if (cairns::Viewport::Hot* vh =
                    viewports_.GetHot(viewport_ids_[v])) {
                vh->scene = active_scene_;
            }
        }
        scene_proxies_.resize(1);

        // P6 multi-scene coexistence: secondary slot is acquired but
        // left empty. Pre-#269 it received half of the debug-grid.
        secondary_scene_ = scenes_.Acquire();
        if (cairns::Scene::Hot* wh2 = scenes_.GetHot(secondary_scene_)) {
            if (cairns::Scene::Cold* wc2 =
                    scenes_.GetCold(secondary_scene_)) {
                *wc2 = cairns::Scene::Cold{};
                wh2->proxy_slot = 1;
                wh2->dirty = true;
            }
        }
        scene_proxies_.resize(2);
        // Surfaceless mode: allocate the offscreen final_target_ and CONTINUE
        // through normal init. The engine -- not the RHI -- is the one that
        // decides which texture the swap pass writes into each frame: in
        // surfaceless mode it builds a SwapResolveTarget pointing at
        // final_target_; in windowed mode it pulls one out of swapchain_.
        // SwapChain and Frames have no notion of "headless" mode.
        if (cfg.surfaceless) {
            final_target_w_ = cfg.width;
            final_target_h_ = cfg.height;
            rhi::TextureDesc td{};
            td.debug_name = "final_target";
            td.dimensions = {static_cast<int32_t>(cfg.width),
                             static_cast<int32_t>(cfg.height), 1};
            td.format = rhi::Format::kBgra8Unorm;
            td.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled |
                       rhi::kTexUsageTransferSrc | rhi::kTexUsageTransferDst;
            td.memory = rhi::Memory::kDefault;
            final_target_ = rhi_.resources.CreateTexture(rhi_.alloc, td);
            if (final_target_.IsNull()) {
                CAIRNS_PRINT("GreaterInit: final_target_ create failed\n");
                return false;
            }
        }
        if ( !initRenderPipeline() ) {
            CAIRNS_PRINT("GreaterInit: initRenderPipeline failed\n");
            return false;
        }
        const uint32_t init_w = cfg.surfaceless ? cfg.width : swapchain_.Width();
        const uint32_t init_h = cfg.surfaceless ? cfg.height : swapchain_.Height();
        if ( !rhi_.frames.InitTargets(rhi_.resources, rhi_.alloc, init_w, init_h) ) {
            CAIRNS_PRINT("GreaterInit: frames.InitTargets failed\n");
            return false;
        }
        initSkinKernel();  // best-effort; missing shader doesn't fail GreaterInit.
        initAnimEvalKernel();  // best-effort; failure -> GPU palette eval off.
        uploadAnimTablesGpu();  // flattens + uploads all scene tables.
        // #222 Phase H.5: skins were Acquired BEFORE this call, so their
        // cached gpu_prefab_header_idx (UINT32_MAX) is stale. Backfill from
        // each skin's scene now that the headers exist.
        skins_.ForEachLive(
            [&](cairns::SkinnedAttachment::Hot& h,
                cairns::SkinnedAttachment::Cold& c) {
                cairns::Prefab::Hot* sht = prefabs_.GetHot(c.scene);
                if (sht) {
                    h.gpu_prefab_header_idx = sht->gpu_prefab_header_idx;
                }
            });
        // #222 Phase H.6: textureHandles never change after scene load;
        // build the engine-side resident_textures_ once here. Per-frame
        // draw() drops its arena alloc + copy and just points the packet
        // span at this vector. Same content, just hoisted.
        resident_textures_.clear();
        for (cairns::PrefabId sid : prefab_ids_) {
            cairns::Prefab::Cold* scold = prefabs_.GetCold(sid);
            if (!scold) {
                continue;
            }
            for (rhi::Handle<rhi::Texture> th : scold->textureHandles) {
                resident_textures_.push_back(th);
            }
        }
        // #222 Phase D.3: skin Group B + anim_eval routed through
        // DynamicBuffers. Created post-scene-load (now: backing handles
        // ready). Descriptor sets allocated here are layout-compatible
        // with the pipeline's set 0 layout (built earlier from
        // frames.plat.skin_group_b_layout_ / anim_eval_layout_) because
        // the per-binding (type, count, stage) tuple matches exactly.
        // #221 Phase 5b: binding 1 (palettes) backed by persistent
        // palette_out_buf_ (anim_eval writes it); per-batch dynamic
        // offset still selects the bucket's palette window. When
        // anim_eval_tables_uploaded_ is false, no backing => kDynamic
        // master fallback.
        if (!skin_kernel_.IsNull() && !skin_output_pool_buffer_.IsNull()) {
            cairns::rhi::DynamicBinding gb[4]{};
            for (uint32_t i = 0; i < 4; ++i) {
                gb[i].stages = cairns::rhi::kStageCompute;
            }
            gb[0].slot = 0;
            gb[0].kind = cairns::rhi::BufferKind::kUniform;
            gb[0].max_range = 64u;
            gb[0].has_dynamic_offset = true;
            gb[1].slot = 1;
            gb[1].kind = cairns::rhi::BufferKind::kStorage;
            gb[1].max_range = 1u << 20;
            gb[1].has_dynamic_offset = true;
            if (anim_eval_tables_uploaded_) {
                gb[1].backing = palette_out_buf_;
            }
            gb[2].slot = 2;
            gb[2].kind = cairns::rhi::BufferKind::kStorage;
            gb[2].max_range = 16384u;
            gb[2].has_dynamic_offset = true;
            gb[3].slot = 3;
            gb[3].kind = cairns::rhi::BufferKind::kStorage;
            gb[3].max_range = 0;  // VK_WHOLE_SIZE
            gb[3].has_dynamic_offset = false;
            gb[3].backing = skin_output_pool_buffer_;
            cairns::rhi::DynamicBuffersDesc gd{};
            gd.debug_name = "dyn_skin_group_b";
            gd.bindings =
                std::span<const cairns::rhi::DynamicBinding>(gb, 4);
            dyn_skin_group_b_ =
                rhi_.resources.CreateDynamicBuffers(rhi_.alloc, rhi_.frames, gd);
            if (dyn_skin_group_b_.IsNull()) {
                CAIRNS_PRINT("GreaterInit: dyn_skin_group_b create failed\n");
                return false;
            }
        }
        // #228 H4b: dyn_anim_eval_ creation moved into recreateAnimDynBindings()
        // so the same path runs at GreaterInit AND after the first runtime
        // load (anim_eval_tables_uploaded_ flips false->true) AND after any
        // anim buffer is destroyed+recreated on growth. Post-L9 / H1, this
        // call at GreaterInit is a no-op (anim_eval_tables_uploaded_ is
        // false at boot); the helper is called from uploadAnimTablesGpu
        // once buffers exist.
        if (!recreateAnimDynBindings()) {
            return false;
        }
        // #237 fix: globals + drawtmp DYNAMIC UBO descriptors point at
        // the master kDynamic buffer with sizeof(struct) range; per-pass
        // bind supplies the offset. Write once here.
        rhi_.frames.WriteUnlitDescriptors(rhi_.resources, rhi_.alloc);
        if ( !initParticles() ) {
            CAIRNS_PRINT("GreaterInit: initParticles failed\n");
            return false;
        }
        render_thread_ = std::make_unique<cairns::RenderThread>(
            [this](cairns::FramePacket& pkt) { this->RecordFrame(pkt); });
        return true;
    }

    bool BuildMeshOpaqueDraws(uint32_t slot) {
        PerSlot& s = slots_[slot];
        // CPU-side scene build only. NO bump allocations -- those happen in
        // EncodeDraws() on the render-thread side post-split. Stable-index
        // writes (resize + index assignment) so the output is independent of
        // walk/execution order.
        // Rotation reads sim, not wall: render_angle_deg_ is sim_angle_deg_
        // plus an interpolation in [0, kFixedDt) toward the next sim step.
        const float angle_degs = render_angle_deg_;
        const float angle_rads = angle_degs * std::numbers::pi / 180.0f;
        const glm::mat4 rot_matrix = glm::rotate(glm::mat4(1.0f), angle_rads, glm::vec3(0, 1.0, 0));

        // Per-viewport camera resolve. Each viewport gets its own
        // RenderPassGlobals (uploaded at a distinct bump offset by
        // EncodeDraws); RecordFrame issues one forward pass per viewport
        // bound against the matching offset. Aspect is (vp_w / vp_h) where
        // vp_w = FrameWidth() / kNumViewportsPerSlot (side-by-side split).
        const float vp_w = static_cast<float>(FrameWidth()) /
                            static_cast<float>(std::max(1, active_viewport_count_));
        const float vp_h = static_cast<float>(FrameHeight());
        const float aspect_ratio = vp_w / vp_h;
        const float fov = 90 * (std::numbers::pi / 180.0f);
        const float near_z = 0.1f;
        const float far_z = 100.0f;
        // proj built per-viewport below since CameraComponent may override
        // fov / near_z / far_z.
        // Per-viewport camera resolve. Camera role #1 (FlyController):
        // viewport.camera_entity == entt::null; FlyController state drives
        // pose. Camera role #2 (placed CameraComponent entity): the entity
        // owns a WorldTransform (pose) + CameraComponent (intrinsics) in
        // the active scene's registry. WorldTransform.world is the camera-
        // to-world matrix; the view matrix is its inverse.
        cairns::Scene::Cold* wc_cam = scenes_.GetCold(active_scene_);
        for (int v = 0; v < active_viewport_count_; ++v) {
            cairns::Viewport::Cold* vpc =
                viewports_.GetCold(viewport_ids_[v]);
            const entt::entity vp_cam_entity = vpc->camera_entity;
            const bool entity_cam =
                vp_cam_entity != entt::null && wc_cam &&
                wc_cam->registry.all_of<cairns::WorldTransform,
                                         cairns::CameraComponent>(vp_cam_entity);
            glm::vec3 camera_pos;
            glm::vec3 camera_dir;
            glm::mat4 view_matrix;
            float vp_fov = fov;
            float vp_near = near_z;
            float vp_far = far_z;
            if (entity_cam) {
                const cairns::CameraComponent& cc =
                    wc_cam->registry.get<cairns::CameraComponent>(vp_cam_entity);
                const cairns::WorldTransform& wt =
                    wc_cam->registry.get<cairns::WorldTransform>(vp_cam_entity);
                vp_fov = cc.fov_y_rad;
                vp_near = cc.near_z;
                vp_far = cc.far_z;
                view_matrix = glm::inverse(wt.world);
                camera_pos = glm::vec3(wt.world[3]);
                // -Z in local space is the camera's forward in world space.
                camera_dir = glm::normalize(glm::vec3(-wt.world[2]));
            } else {
                const cairns::FlyController& fc = vpc->fly;
                const float cy = std::cos(fc.yaw);
                const float sy = std::sin(fc.yaw);
                const float cp = std::cos(fc.pitch);
                const float sp = std::sin(fc.pitch);
                camera_pos = fc.position;
                camera_dir = glm::vec3(-cp * sy, sp, -cp * cy);
                view_matrix = glm::lookAtRH(camera_pos,
                                             camera_pos + camera_dir,
                                             glm::vec3(0, 1, 0));
            }
            const glm::mat4 vp_proj =
                glm::perspectiveRH_ZO(vp_fov, aspect_ratio, vp_near, vp_far);
            const glm::mat4 view_proj = vp_proj * view_matrix;
            s.pending_globals[v] = cairns::rhi::RenderPassGlobals {
                .view_proj = view_proj,
                .inv_view_proj = glm::inverse(view_proj),
                .camera_pos = glm::vec4(camera_pos, 1.0f /*exposure */),
                .camera_dir = glm::vec4(camera_dir, vp_near),
                .screen_params = glm::vec4(vp_w, vp_h, 1.0f / vp_w, 1.0f / vp_h)
            };
            s.pending_view_matrix[v] = view_matrix;
            s.pending_near_z[v] = vp_near;
            s.pending_far_z[v] = vp_far;
        }

        // Set the active scene's root_transform, run TRS hierarchy
        // propagation (no-op when no entity carries a Transform; the
        // current scene-load emplaces WorldTransform directly), then
        // extract. Extract composes node.globalTransform * (world *
        // root_transform).
        // Fan-out: extract from EVERY world that any viewport binds to (set
        // built from viewports_[].world; deduped via the scenes_ pool's
        // contiguous slot indices). The active_scene_'s extract result lives
        // in s.proxies (the per-slot single draw list); secondary scenes'
        // proxies land in scene_proxies_[wh->proxy_slot] for downstream
        // per-viewport draw consumers (#194 / #190's two-viewport path uses
        // these). Today s.proxies still drives BuildMeshOpaqueDraws's draw
        // list -- per-viewport draw fan-out lands when the multi-pass split
        // does (depends on #206's per-pass globals being per-viewport too).
        // #219 Chunk B: bind s.proxies' meshes + primitives lists to this
        // slot's BumpArena. Capacity headroom for the 3300-hero benchmark
        // (~3300 / ~11220); pushes beyond cap assert. Other 6 ProxyArrays
        // stay on default heap (untouched in current code).
        // #195 multi-scene fan-out: extract EVERY distinct scene any viewport
        // binds into the ONE s.proxies union (appended), recording each scene's
        // [mesh) range. Viewports on the same scene share its range (dedup).
        // The draw build later carves a per-viewport [draw) sub-range from
        // these, so two viewports on two scenes render different content.
        s.proxies.Reset(s.arena);
        s.scene_ranges_count = 0;
        for (int v = 0; v < active_viewport_count_; ++v) {
            s.viewport_scene_idx[v] = -1;
        }
        auto extract_scene_once = [&](cairns::SceneId sid) -> int {
            for (uint32_t k = 0; k < s.scene_ranges_count; ++k) {
                if (s.scene_ranges[k].scene.index == sid.index) {
                    return static_cast<int>(k);
                }
            }
            cairns::Scene::Hot* wh = scenes_.GetHot(sid);
            cairns::Scene::Cold* wc = scenes_.GetCold(sid);
            if (!wh || !wc) {
                return -1;
            }
            if (s.scene_ranges_count >=
                static_cast<uint32_t>(PerSlot::kMaxScenesPerSlot)) {
                return -1;
            }
            wh->root_transform = rot_matrix;
            cairns::PropagateTransforms(*wc, glm::mat4(1.0f));
            const uint32_t mesh_lo =
                static_cast<uint32_t>(s.proxies.meshes.size());
            cairns::ExtractFromScene(*wc, wh->root_transform, assets_,
                                     prefabs_, meshes_, s.proxies,
                                     /*append=*/true);
            const uint32_t mesh_hi =
                static_cast<uint32_t>(s.proxies.meshes.size());
            const uint32_t k = s.scene_ranges_count++;
            PerSlot::SceneDrawRange& r = s.scene_ranges[k];
            r.scene = sid;
            r.mesh_lo = mesh_lo;
            r.mesh_hi = mesh_hi;
            r.draw_lo = 0;
            r.draw_hi = 0;
            return static_cast<int>(k);
        };
        for (int v = 0; v < active_viewport_count_; ++v) {
            const cairns::SceneId wid =
                viewports_.GetHot(viewport_ids_[v])->scene;
            s.viewport_scene_idx[v] = extract_scene_once(wid);
        }

        // Counting pass -> total_draws.
        uint32_t total_draws = 0;
        for (const cairns::MeshProxy& mp : s.proxies.meshes) {
            total_draws += mp.primitive_count;
        }
        // #219 Chunk A: count-then-allocate on the per-slot BumpArena. The
        // arena was reset at slot Acquire and is exclusively ours until
        // the render thread submits this slot's frame. No std::vector
        // heap; no .resize() pump-and-shrink.
        using DrawKeyPair = std::pair<cairns::DrawKey, uint32_t>;
        s.drawList = {
            s.arena.AllocateArray<cairns::Draw>(total_draws), total_draws};
        s.drawListSorted = {
            s.arena.AllocateArray<DrawKeyPair>(total_draws), total_draws};
        s.draw_world_matrices = {
            s.arena.AllocateArray<glm::mat4>(total_draws), total_draws};
        s.draw_entity_ids = {
            s.arena.AllocateArray<uint32_t>(total_draws), total_draws};

        // Per-proxy starting stable_idx via prefix sum over primitive_count.
        // Lives on the per-slot arena so the workers can read it concurrently
        // without further allocation.
        const uint32_t n_proxies =
            static_cast<uint32_t>(s.proxies.meshes.size());
        uint32_t* proxy_first_draw =
            s.arena.AllocateArray<uint32_t>(n_proxies ? n_proxies : 1);
        {
            uint32_t acc = 0;
            for (uint32_t i = 0; i < n_proxies; ++i) {
                proxy_first_draw[i] = acc;
                acc += s.proxies.meshes[i].primitive_count;
            }
        }
        // #195: convert each scene's [mesh) range into its [draw) range via the
        // shared, spec-tested CarveSceneDrawRanges, so each viewport's sorted
        // sub-span covers only its own scene.
        {
            std::array<cairns::SceneDrawSpan, PerSlot::kMaxScenesPerSlot>
                spans{};
            for (uint32_t k = 0; k < s.scene_ranges_count; ++k) {
                spans[k].mesh_lo = s.scene_ranges[k].mesh_lo;
                spans[k].mesh_hi = s.scene_ranges[k].mesh_hi;
            }
            cairns::CarveSceneDrawRanges(
                std::span<const uint32_t>(proxy_first_draw, n_proxies),
                total_draws,
                std::span<cairns::SceneDrawSpan>(spans.data(),
                                                 s.scene_ranges_count));
            for (uint32_t k = 0; k < s.scene_ranges_count; ++k) {
                s.scene_ranges[k].draw_lo = spans[k].draw_lo;
                s.scene_ranges[k].draw_hi = spans[k].draw_hi;
            }
        }

        // #221 multithreaded fill. Each worker takes a disjoint proxy range
        // [proxy_lo, proxy_hi) and writes draws starting at
        // proxy_first_draw[proxy_lo]. Subchunks are non-overlapping by
        // construction so there's no shared writer state -- the only shared
        // reads are materials_.GetHot / rhi_.resources.BufferBaseOffset,
        // both pure ResourceManager indexed loads with no internal mutation.
        auto fill_chunk = [&](uint32_t proxy_lo, uint32_t proxy_hi) {
            for (uint32_t i = proxy_lo; i < proxy_hi; ++i) {
                const cairns::MeshProxy& mp = s.proxies.meshes[i];
                const BufHandle pos = mp.pos;
                [[maybe_unused]] const BufHandle attr = mp.attr;
                const BufHandle index = mp.index;
                const glm::mat4& world_mat = mp.world_matrix;
                const uint32_t index_base_off =
                    rhi_.resources.BufferBaseOffset(rhi_.alloc, index);
                // #221 Phase 9c: skinned-branch resolves once per proxy.
                // mp.skin packs {generation, index} into uint32; resolve via
                // skins_ pool. F5 fix: pos stream rebinds to the per-actor
                // skin_output_pool_ slice (mesh-local), attr stream rebinds
                // to mhot->attr_skinned_alias (pre-offset by
                // global_base_vertex * sizeof(VertexAttribute)), and
                // draw.vertex_offset becomes mesh-local
                // (prim.vertex_offset - global_base_vertex).
                bool skinned = false;
                BufHandle skin_pos_buf{};
                BufHandle skin_attr_buf{};
                uint32_t skin_global_base_vertex = 0;
                if (mp.skin != cairns::kInvalidSkin) {
                    cairns::SkinId sid{
                        static_cast<uint16_t>(mp.skin & 0xFFFFu),
                        static_cast<uint16_t>((mp.skin >> 16) & 0xFFFFu)};
                    const cairns::SkinnedAttachment::Hot* sh =
                        skins_.GetHot(sid);
                    if (sh) {
                        const cairns::Mesh::Hot* mhot_s =
                            meshes_.GetHot(sh->mesh);
                        if (mhot_s &&
                            !mhot_s->attr_skinned_alias.IsNull() &&
                            !sh->pos_stream.IsNull()) {
                            skinned = true;
                            // #222 Phase E.6: pos_stream is the per-actor
                            // pre-offset alias of skin_output_pool_buffer_.
                            skin_pos_buf = sh->pos_stream;
                            skin_attr_buf = mhot_s->attr_skinned_alias;
                            skin_global_base_vertex =
                                mhot_s->global_base_vertex;
                        }
                    }
                }
                uint32_t stable_idx = proxy_first_draw[i];
                for (uint32_t p = 0; p < mp.primitive_count; ++p) {
                    const cairns::PrimitiveProxy& prim =
                        s.proxies.primitives[mp.first_primitive + p];
                    const MatId mat_id = prim.material_id;

                    cairns::Draw draw{};
                    // #220 Step 1: bind group lives in Material::Hot.
                    draw.bind_groups[1] = materials_.GetHot(mat_id)->set2;
                    // #222 Phase D.2: route set 2 (per-draw drawtmp UBO).
                    draw.dynamic_buffers = dyn_drawtmp_;
                    draw.index_buffer = index;
                    draw.index_offset =
                        index_base_off + (prim.first_index * sizeof(uint32_t));
                    if (skinned) {
                        draw.vertex_offset =
                            prim.vertex_offset -
                            static_cast<int32_t>(skin_global_base_vertex);
                        // #222 Phase E.6: stream-0 alias pre-baked; no
                        // side-channel pos_buffer_byte_offset.
                        draw.vertex_buffers
                            [cairns::Draw::kVertexBufferPosSlot] =
                            skin_pos_buf;
                        draw.vertex_buffers
                            [cairns::Draw::kVertexBufferAttrSlot] =
                            skin_attr_buf;
                    } else {
                        draw.vertex_offset = prim.vertex_offset;
                        draw.vertex_buffers
                            [cairns::Draw::kVertexBufferPosSlot] = pos;
                        draw.vertex_buffers
                            [cairns::Draw::kVertexBufferAttrSlot] = attr;
                    }
                    draw.instance_offset = 0;
                    draw.instance_count = 1;
                    // filled by EncodeDraws.
                    draw.dynamic_buffer_offsets[0] = UINT32_MAX;
                    draw.dynamic_buffer_offsets[1] = UINT32_MAX;
                    assert(prim.index_count % 3 == 0);
                    draw.triangle_count =
                        tiny_quad_test_ ? 2 : prim.index_count / 3;

                    // P2: depth_q dropped from the sort key (pass 0). Including
                    // it would make the sort camera-dependent and force a per-
                    // viewport re-sort. Material + pipeline ordering still
                    // preserves batching across both viewports.
                    s.drawListSorted[stable_idx] = std::make_pair(
                        // #220 Step 1: BuildDrawKey wants a uint32 material id;
                        // feed it Handle::index (uint16; 0x3FFFFFFF mask is a
                        // no-op but kept for shape parity with prior code).
                        cairns::BuildDrawKey(
                            static_cast<uint32_t>(mat_id.index) & 0x3FFFFFFFu,
                            /*depth=*/0, kMockTranslucency, kMockViewport,
                            kMockViewportLayer, kMockFullscreenLayer),
                        stable_idx);
                    s.drawList[stable_idx] = draw;
                    s.draw_world_matrices[stable_idx] = world_mat;
                    // Always emit the real entity id; outline.frag does the
                    // highlight-set filter via the highlights texture so pick
                    // can readback the real id from id_target_ regardless of
                    // outline state.
                    s.draw_entity_ids[stable_idx] = mp.entity_id;
                    ++stable_idx;
                }
            }
        };

        if (n_proxies > 0) {
            uint32_t n_workers = build_pool_->num_workers();
            if (n_workers > n_proxies) {
                n_workers = n_proxies;
            }
            // Range for worker w is [w*N/W, (w+1)*N/W) -- arithmetic
            // partition, no scratch table, no `per+rem` ceremony.
            const uint32_t n_proxies_local = n_proxies;
            const uint32_t n_workers_local = n_workers;
            build_pool_->RunIndices(n_workers, [&](uint32_t w) {
                const uint32_t lo =
                    (n_proxies_local * w) / n_workers_local;
                const uint32_t hi =
                    (n_proxies_local * (w + 1)) / n_workers_local;
                fill_chunk(lo, hi);
            });
        }

        // A.7: stamp FrameStats at end of build. cull_stage_implemented stays
        // false until a real per-proxy frustum cull lands (Phase G). G5 reads
        // this flag and SKIPs honestly when cull isn't real.
        {
            FrameStats fs{};
            fs.submitted = n_proxies;
            fs.draw_calls = static_cast<uint32_t>(s.drawList.size());
            uint64_t verts = 0;
            for (const cairns::Draw& d : s.drawList) {
                verts += static_cast<uint64_t>(d.triangle_count) * 3u;
            }
            fs.verts_processed = verts;
            fs.culled = 0;
            fs.cull_stage_implemented = false;
            last_frame_stats_ = fs;
        }

        return true;
    }

    // Render-side bump of all per-frame UBOs. Order matters: globals first,
    // per-draw (material, draw_tmp) in stable_idx order, fixed_dt last.
    // Writes s.globals_offset, s.drawList[*].dynamic_buffer_offsets[0..1],
    // s.dt_off. Compute kernel sees pkt.fixed_dt (constant sim dt), not wall.
    void EncodeDraws(const FramePacket& pkt) {
        PerSlot& s = slots_[pkt.slot];
        // 1. globals UBO -- one per viewport, distinct bump offsets. The
        // forward pass for viewport v binds s.globals_offset[v].
        for (int v = 0; v < active_viewport_count_; ++v) {
            void* gptr = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::RenderPassGlobals), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &s.globals_offset[v]);
            assert(gptr && "bump alloc failed: render pass globals");
            memcpy(gptr, &s.pending_globals[v], sizeof(cairns::rhi::RenderPassGlobals));
        }
        if (frame_ <= 6) {
            const glm::mat4& vp = s.pending_globals[active_viewport_index_].view_proj;
            const float vp_w = static_cast<float>(FrameWidth()) /
                                static_cast<float>(std::max(1, active_viewport_count_));
            const float aspect_ratio = vp_w / static_cast<float>(FrameHeight());
            size_t entity_count = 0;
            if (auto* wc = scenes_.GetCold(active_scene_)) {
                entity_count = wc->registry.storage<entt::entity>().size();
            }
            fprintf(stderr,
                    "[FLAKE] frame=%u w=%u h=%u aspect=%.9f vp00=%.9f vp11=%.9f "
                    "vp22=%.9f vp32=%.9f goff[0]=%u par_in=%u par_out=%u "
                    "entities=%zu meshes=%zu prims=%zu\n",
                    frame_, FrameWidth(), FrameHeight(), aspect_ratio,
                    vp[0][0], vp[1][1], vp[2][2], vp[3][2],
                    s.globals_offset[0],
                    pkt.particle_parity_in, pkt.particle_parity_out,
                    entity_count, s.proxies.meshes.size(),
                    s.proxies.primitives.size());
        }

        // 2. Per-draw material + draw_tmp UBOs in stable_idx order.
        const cairns::rhi::MaterialGpu material_gpu {};
        for (size_t i = 0; i < s.drawList.size(); ++i) {
            uint32_t material_offset = 0;
            void* mptr = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::MaterialGpu), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &material_offset);
            assert(mptr && "bump alloc failed: material");
            memcpy(mptr, &material_gpu, sizeof(material_gpu));

            cairns::rhi::DrawTmp draw_tmp { .model_matrix = s.draw_world_matrices[i] };
            draw_tmp.entity_id = s.draw_entity_ids[i];
            uint32_t drawtmp_offset = 0;
            void* tptr = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::DrawTmp), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &drawtmp_offset);
            assert(tptr && "bump alloc failed: draw tmp");
            memcpy(tptr, &draw_tmp, sizeof(draw_tmp));

            s.drawList[i].dynamic_buffer_offsets[0] = material_offset;
            s.drawList[i].dynamic_buffer_offsets[1] = drawtmp_offset;
        }

        // 3. delta_time UBO.
        float* dt_ptr = static_cast<float*>(
            rhi_.alloc.BumpAllocate(sizeof(float), rhi_.alloc.UboAlign(),
                                    rhi::Memory::kDynamic, &s.dt_off));
        assert(dt_ptr && "bump alloc failed: delta time");
        // Compute kernel sees the fixed sim dt, NOT wall dt -- particles step
        // at a constant rate regardless of frame timing.
        *dt_ptr = pkt.fixed_dt;
    }

    // Drain in-flight work, settle GPU, invalidate any caches keyed on the
    // old swap dims, then resize per-viewport / final_target state. Called
    // at the top of draw(); a no-op when no SDL resize is pending and the
    // observed swap dims haven't drifted (Vk's WSI may auto-recreate the
    // swapchain on OUT_OF_DATE without ever calling this path).
    void ApplyPendingResize() {
        const uint32_t cur_w = final_target_.IsNull() ? swapchain_.Width()
                                                       : final_target_w_;
        const uint32_t cur_h = final_target_.IsNull() ? swapchain_.Height()
                                                       : final_target_h_;
        const bool dims_drifted = (cur_w != last_seen_swap_w_) ||
                                   (cur_h != last_seen_swap_h_);
        if (!resize_pending_ && !dims_drifted) {
            return;
        }
        if (render_thread_) {
            render_thread_->Drain();
        }
        rhi_.device.WaitIdle();
        rhi_.offscreen_targets.FlushFramebuffers();
        if (!final_target_.IsNull() &&
            (resize_pending_w_ != final_target_w_ ||
             resize_pending_h_ != final_target_h_) &&
            resize_pending_w_ != 0 && resize_pending_h_ != 0) {
            ResizeFinalTarget(resize_pending_w_, resize_pending_h_);
        }
        last_seen_swap_w_ = final_target_.IsNull() ? swapchain_.Width()
                                                    : final_target_w_;
        last_seen_swap_h_ = final_target_.IsNull() ? swapchain_.Height()
                                                    : final_target_h_;
        resize_pending_ = false;
    }

    bool draw() {
        // Phase D: steady-frame marker. Throttled to once per 60 frames
        // so the log scanner can see "engine is in steady state" without
        // drowning out the [LOAD]/[RELOAD] markers. Pair with the
        // Instruments signpost on this scope; the time profiler shows
        // each frame as a 16ms band under "Points of Interest".
        CAIRNS_SIGNPOST_INTERVAL_SCOPED("frame", "draw");
        if ((frame_ % 60) == 0) {
            size_t ec = 0;
            if (auto* wc = scenes_.GetCold(active_scene_)) {
                ec = wc->registry.storage<entt::entity>().size();
            }
            CAIRNS_PRINT_ERR("[STEADY] frame=%u entities=%zu prefabs=%zu\n",
                              frame_, ec, prefab_ids_.size());
        }
#if CAIRNS_VULKAN
        if (rhi_.frames.plat.recreate_pending_.load(std::memory_order_acquire)) {
            if (render_thread_) {
                render_thread_->Drain();
            }
            rhi_.device.WaitIdle();
            if (final_target_.IsNull()) {
                swapchain_.plat.RecreateSwapChain();
                rhi_.offscreen_targets.FlushFramebuffers();
            }
            rhi_.frames.plat.recreate_pending_.store(false, std::memory_order_release);
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                slots_[i].present_ready = false;
            }
            prev_present_slot_ = -1;
            present_queue_.clear();
        }
#endif
        ApplyPendingResize();
        if (final_target_.IsNull() &&
            (swapchain_.Width() == 0 || swapchain_.Height() == 0)) {
            if (render_thread_) {
                render_thread_->Drain();
            }
            return false;
        }

        frame_++;
        const uint32_t slot = (frame_ - 1) % kFramesInFlight;

        // Acquire BEFORE touching slot storage -- this is the backpressure
        // gate, blocks if the render thread is still holding slot S.
        PerSlot& s = slots_[slot];
        // std::unique_lock FIRST. The mutex is the actual ownership
        // claim; render_thread_->Acquire then blocks on the state
        // machine. Lock-first means dtor runs cleanly via stack
        // unwinding even if anything between this point and Submit
        // throws.
        std::unique_lock<std::mutex> slot_lock(s.slot_mutex);
        render_thread_->Acquire(slot);
        // #210 reset this slot's CPU bump arena. Safe here because
        // Acquire blocked until the render thread finished its prior
        // use of slot S -- no reader still inside the bytes.
        s.arena.Reset();

        const uint64_t cpu_now_ns = cairns::timestamp_ns();
        if (cpu_last_frame_ns_ != 0) {
            cpu_ms_last_ = static_cast<float>(cpu_now_ns - cpu_last_frame_ns_) / 1.0e6f;
            cpu_ms_history_[cpu_ms_head_] = cpu_ms_last_;
            cpu_ms_head_ = (cpu_ms_head_ + 1) % kCpuMsHistory;
        }
        cpu_last_frame_ns_ = cpu_now_ns;
        s.pkt.request_dump = false;
        s.pkt.dump_path.clear();
        if (dump_and_exit_ && !dump_emitted_ &&
            sim_frame_ >= cairns::kGoldenDumpFrame) {
            s.pkt.request_dump = true;
            s.pkt.dump_path = engine_cfg_.dump_path.empty()
                                   ? std::filesystem::path("/tmp/cairns_dump.png")
                                   : engine_cfg_.dump_path;
            dump_emitted_ = true;
            dump_emit_frame_ = frame_;
        }
        if (dump_and_exit_ && dump_emitted_ &&
            frame_ >= dump_emit_frame_ + 2) {
            std::exit(0);  // headless byte-gate: dump flushed, exit. Tests
                           // set use_fixed_clock without dump_path, so
                           // dump_and_exit_=false here and tests survive.
        }

        cairns::Timer t_frame("frame", 0);

        // Fiedler fixed-timestep accumulator. clock_ is FixedClock under
        // CAIRNS_DUMP (1 step/frame, alpha=0) or WallClock live. wall_dt is
        // clamped to kMaxFrameDt to avoid spiral-of-death on big stalls.
        const double wall_dt = clock_->Tick();
        accumulator_ += std::min(wall_dt, cairns::kMaxFrameDt);
        sim_steps_this_frame_ = 0;
        while (accumulator_ >= cairns::kFixedDt &&
               sim_steps_this_frame_ < cairns::kMaxStepsPerFrame) {
            sim_angle_deg_ += cairns::kRotDegPerSec * static_cast<float>(cairns::kFixedDt);
            ++sim_frame_;
            accumulator_ -= cairns::kFixedDt;
            ++sim_steps_this_frame_;
        }
        const float alpha = static_cast<float>(accumulator_ / cairns::kFixedDt);
        render_angle_deg_ = sim_angle_deg_ +
                            alpha * cairns::kRotDegPerSec *
                                static_cast<float>(cairns::kFixedDt);

        if (frame_ <= 5) {
            fprintf(stderr,
                    "[FCLK] frame=%u wall_dt=%.4f acc=%.4f steps=%u alpha=%.3f "
                    "sim_frame=%llu sim_deg=%.3f\n",
                    frame_, wall_dt, accumulator_, sim_steps_this_frame_,
                    alpha, static_cast<unsigned long long>(sim_frame_),
                    sim_angle_deg_);
        }


        cairns::Timer t_build("build_draws", 1);
        if (!BuildMeshOpaqueDraws(slot)) {
            return false;
        }
        t_build.End();
        // #195: sort PER-SCENE-RANGE so each viewport's sorted sub-span orders
        // (and indexes) only its own scene's draws.
        for (uint32_t k = 0; k < s.scene_ranges_count; ++k) {
            const PerSlot::SceneDrawRange& r = s.scene_ranges[k];
            std::sort(s.drawListSorted.begin() + r.draw_lo,
                      s.drawListSorted.begin() + r.draw_hi);
        }

        // #219 Chunk A: count-then-allocate the resident-textures gather on
        // the per-slot BumpArena. prefabs_ + textureHandles are persistent
        // engine state, so two-pass costs nothing.
        // #222 Phase H.6: resident_textures hoisted to the engine-owned
        // resident_textures_ vector built once at scene-load (uploadAnim
        // TablesGpu). draw() drops the per-frame arena alloc + copy.

        // #221 Skinning P5/P8: BuildSkinFrame populates the per-frame skin
        // payload (palettes, InstanceMeta, SkinBatchGpu list) on the per-
        // slot arena and publishes spans on s.pkt. Today (no skinned content
        // + skin_kernel_ Null) it writes empty spans -- the static path
        // stays bit-for-bit; the call site is wired so a future content
        // commit (load CesiumMan + attach SkinRef) flips the switch
        // without touching draw().
        {
            cairns::Timer t_skin("skin_eval", 8);
            BuildSkinFrame(slot);
        }

        // Fill packet header (the view into per-slot storage).
        s.pkt.frame_idx = frame_;
        s.pkt.slot = slot;
        s.pkt.view = s.pending_view_matrix[active_viewport_index_];
        s.pkt.proj = glm::mat4(1.0f);  // not used downstream; view_proj baked into pending_globals
        s.pkt.near_z = s.pending_near_z[active_viewport_index_];
        s.pkt.far_z = s.pending_far_z[active_viewport_index_];
        s.pkt.sim_steps_this_frame = sim_steps_this_frame_;
        s.pkt.fixed_dt = static_cast<float>(cairns::kFixedDt);
        // Wait for the previous frame's render-thread-published parity. In
        // steady state Acquire(slot) already established the happens-after,
        // so this rarely actually blocks.
        {
            std::unique_lock<std::mutex> lk(parity_m_);
            parity_cv_.wait(lk, [&] {
                return latest_parity_frame_ + 1 >= frame_;
            });
            s.pkt.particle_parity_in = latest_parity_out_;
        }
        s.pkt.draws = std::span<const cairns::Draw>(s.drawList.data(), s.drawList.size());
        s.pkt.sorted = std::span<const std::pair<cairns::DrawKey, uint32_t>>(
            s.drawListSorted.data(), s.drawListSorted.size());
        s.pkt.resident_textures = std::span<const rhi::Handle<rhi::Texture>>(
            resident_textures_.data(), resident_textures_.size());

        // A.9: imgui-in-golden opt-in. The original guard skipped imgui
        // whenever golden_=true OR when final_target_ was non-null
        // (surfaceless). Tests now ask for imgui in golden mode (G6) via
        // SetImguiInGolden(true). The SDL3 NewFrame call still gets skipped
        // in surfaceless because cairns_serve doesn't init SDL3; ImGui
        // proper runs (CreateContext done by test harness, NewFrame on
        // ImGui itself, font atlas already built).
        const bool surfaceless = !final_target_.IsNull();
        const bool draw_imgui =
            (!golden_ && !surfaceless) ||
            (golden_ && imgui_in_golden_);
        if (draw_imgui) {
            if (!surfaceless) {
                ImGui_ImplSDL3_NewFrame();
            } else {
                // surfaceless skips ImGui_ImplSDL3_NewFrame, which sets
                // DisplaySize; NewFrame asserts on the default (-1,-1).
                ImGuiIO& io = ImGui::GetIO();
                io.DisplaySize = ImVec2(static_cast<float>(FrameWidth()),
                                        static_cast<float>(FrameHeight()));
                io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            }
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
            ImGui::Begin("cairns", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            // Golden mode injects fixed HudStats so the overlay is byte-stable;
            // live cpu_ms_last_ / Timer slots are wall-clock and would flake.
            const bool hud_injected = injected_hud_stats_.has_value();
            const float cpu_ms_disp =
                hud_injected ? injected_hud_stats_->cpu_ms : cpu_ms_last_;
            const float fps =
                hud_injected
                    ? injected_hud_stats_->fps
                    : (cpu_ms_last_ > 0.0f ? 1000.0f / cpu_ms_last_ : 0.0f);
            const float* graph_data = hud_injected
                                          ? injected_hud_stats_->frame_ms.data()
                                          : cpu_ms_history_;
            const int graph_count =
                hud_injected ? static_cast<int>(cairns::HudStats::kGraph)
                             : kCpuMsHistory;
            const int graph_head =
                hud_injected ? static_cast<int>(injected_hud_stats_->graph_head)
                             : cpu_ms_head_;
            float ms_max = 1.0f;
            float ms_avg = 0.0f;
            int ms_n = 0;
            for (int i = 0; i < graph_count; ++i) {
                const float v = graph_data[i];
                if (!std::isfinite(v) || v < 0.0f || v > 1.0e6f) {
                    continue;
                }
                ms_max = v > ms_max ? v : ms_max;
                ms_avg += v;
                ++ms_n;
            }
            ms_avg /= static_cast<float>(ms_n > 0 ? ms_n : 1);
            ImGui::Text("CPU %6.2f ms   |   %3.0f FPS", cpu_ms_disp, fps);
            ImGui::Text("avg %6.2f ms   |   peak %6.2f ms", ms_avg, ms_max);
            if (!hud_injected) {
                auto slot_avg_ms = [this](int s) -> float {
                    const uint64_t n = cairns::Timer::accum_itrs_[s];
                    if (n == 0) {
                        return slot_ms_cache_[s];
                    }
                    const double v =
                        cairns::Timer::accum_times_[s] /
                        static_cast<double>(n) / 1000.0;
                    if (!std::isfinite(v) || v < 0.0 || v > 1.0e6) {
                        return slot_ms_cache_[s];
                    }
                    slot_ms_cache_[s] = static_cast<float>(v);
                    return slot_ms_cache_[s];
                };
                const uint32_t gpu_mask = cairns::TimerStorage::GpuSlotMask();
                float gpu_frame_ms = 0.0f;
                for (uint32_t s = 0; s < cairns::Timer::kMaxSlots; ++s) {
                    if (gpu_mask & (1u << s)) {
                        gpu_frame_ms += slot_avg_ms(s);
                    }
                }
                ImGui::Text("%-12s %5.2f ms", "gpu_frame", gpu_frame_ms);
                for (uint32_t s = 0; s < cairns::Timer::kMaxSlots; ++s) {
                    const char* nm = cairns::Timer::slot_names_[s];
                    if (!nm) {
                        continue;
                    }
                    if (cairns::Timer::accum_itrs_[s] == 0 &&
                        slot_ms_cache_[s] == 0.0f) {
                        continue;
                    }
                    if (std::strcmp(nm, "set up render pass globals") == 0 ||
                        std::strcmp(nm, "build opaque draw list") == 0 ||
                        std::strcmp(nm, "particle_sim") == 0 ||
                        std::strcmp(nm, "forward") == 0) {
                        continue;
                    }
                    ImGui::Text("%-12s %6.2f ms", nm, slot_avg_ms(s));
                }
            }
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%.2f ms", cpu_ms_disp);
            ImGui::PlotLines("##cpuhist", graph_data, graph_count,
                             graph_head, overlay, 0.0f, ms_max * 1.15f,
                             ImVec2(300.0f, 110.0f));
            ImGui::End();
            ImGui::PopStyleColor(4);
            ImGui::Render();
            auto free_snapshot = [](ImDrawData* d) {
                if (!d) return;
                for (int i = 0; i < d->CmdLists.Size; ++i) {
                    IM_DELETE(d->CmdLists[i]);
                }
                IM_DELETE(d);
            };
            free_snapshot(s.pkt.imgui_snapshot);
            ImDrawData* src = ImGui::GetDrawData();
            ImDrawData* dst = IM_NEW(ImDrawData)();
            dst->Valid = src->Valid;
            dst->DisplayPos = src->DisplayPos;
            dst->DisplaySize = src->DisplaySize;
            dst->FramebufferScale = src->FramebufferScale;
            dst->OwnerViewport = src->OwnerViewport;
            dst->Textures = src->Textures;
            // #222 windowed-crash fix: ImDrawData::AddDrawList ->
            // AddDrawListToDrawDataEx asserts _VtxWritePtr == VtxBuffer.Data
            // + VtxBuffer.Size on the input draw list. ImDrawList::CloneOutput()
            // only copies CmdBuffer/IdxBuffer/VtxBuffer/Flags -- it does NOT
            // restore _VtxWritePtr / _IdxWritePtr / _VtxCurrentIdx on the
            // freshly-constructed clone, so the assertion fires. Bypass
            // AddDrawList and replicate its bookkeeping ourselves.
            for (int i = 0; i < src->CmdLists.Size; ++i) {
                ImDrawList* cloned = src->CmdLists[i]->CloneOutput();
                dst->CmdLists.push_back(cloned);
                dst->CmdListsCount++;
                dst->TotalVtxCount += cloned->VtxBuffer.Size;
                dst->TotalIdxCount += cloned->IdxBuffer.Size;
            }
            s.pkt.imgui_snapshot = dst;
        } else {
            if (s.pkt.imgui_snapshot != nullptr) {
                for (int i = 0; i < s.pkt.imgui_snapshot->CmdLists.Size; ++i) {
                    IM_DELETE(s.pkt.imgui_snapshot->CmdLists[i]);
                }
                IM_DELETE(s.pkt.imgui_snapshot);
                s.pkt.imgui_snapshot = nullptr;
            }
        }

        // Hand the slot to the render thread BEFORE main-thread Submit.
        // Unlock the mutex first so render thread's RecordFrame can take
        // its own std::lock_guard without blocking on main.
        slot_lock.unlock();
        render_thread_->Submit(slot, &s.pkt);

        present_queue_.push_back(static_cast<int32_t>(slot));
        {
            cairns::Timer t_pw("present_wait", 9);
            while (!present_queue_.empty()) {
                const int32_t head = present_queue_.front();
                PerSlot& ps = slots_[head];
                rhi::FrameContext present_fc{};
                rhi::SwapResolveTarget present_target{};
                bool ready = false;
                {
                    std::unique_lock<std::mutex> lk(present_m_);
                    if (ps.present_ready) {
                        present_fc = ps.present_fc;
                        present_target = ps.present_target;
                        ps.present_ready = false;
                        ready = true;
                    }
                }
                if (!ready) {
                    break;
                }
                present_queue_.pop_front();
                rhi_.frames.Present(present_target, rhi_.frame_capture,
                                    present_fc);
            }
        }
        prev_present_slot_ = static_cast<int32_t>(slot);

        // Under CAIRNS_DUMP, collapse to depth-1 pipelining: wait for the
        // render thread to fully complete this frame before the next iteration
        // queues another. Keeps frame 5's dump output byte-identical regardless
        // of threading (Drain forces same parity sequence as single-threaded).
        //
        // Also drain in surfaceless mode (cairns_serve) so the next
        // io.dumpTexture op sees the rendered pixels rather than reading
        // final_target_ while the render thread is still working on it.
        // #207 also drain when a pick is pending so the id_target_ readback
        // sees the just-rendered frame -- windowed sdl-min normally lets
        // the render thread run async, but Shift+LMB stalls one frame to
        // resolve the pick (acceptable cost for an interactive event).
        if (golden_ || !final_target_.IsNull() || pick_pending_) {
            render_thread_->Drain();
        }

        // #207 pick: read one R32U texel from id_target_[vp]. The forward
        // pass writes entt::to_integral(entity)+1 there; value 0 = clear
        // background (clicked empty space). Drop the result into highlights_
        // so the outline pass activates on the next frame.
        if (pick_pending_ && pick_viewport_ < kNumViewports &&
            !id_target_[pick_viewport_].IsNull()) {
            uint32_t entity_plus_one = 0;
            const bool ok = rhi_.resources.ReadBackTextureR32UTexel(
                id_target_[pick_viewport_], pick_x_, pick_y_,
                entity_plus_one);
            // #267: resolve hero name + world AABB from the clicked id so
            // the [PICK] line answers "which hero + where" in one printf.
            const char* hero_name = "<none>";
            std::string hero_name_storage;
            uint32_t hero_scene_idx = 0xFFFFFFFFu;
            glm::vec3 hero_min(0.0f);
            glm::vec3 hero_max(0.0f);
            bool hero_has_aabb = false;
            bool hero_animated = false;
            if (ok && entity_plus_one != 0u && !prefab_ids_.empty()) {
                const uint32_t eid = entity_plus_one - 1u;
                hero_scene_idx =
                    eid % static_cast<uint32_t>(prefab_ids_.size());
                if (hero_scene_idx < glb_paths_.size()) {
                    hero_name_storage =
                        glb_paths_[hero_scene_idx].filename().string();
                    hero_name = hero_name_storage.c_str();
                }
                // Resolve world AABB via the entity's WorldTransform + the
                // scene's first mesh bind-pose AABB (matches the cull path).
                if (cairns::Scene::Cold* wcc =
                        scenes_.GetCold(active_scene_)) {
                    entt::entity ent{eid};
                    if (wcc->registry.valid(ent)) {
                        // #222: animated? entity gets a SkinRef when
                        // TryCreateSkinForScene succeeded at init.
                        // Absence -> static bind pose; check [SKIN-FAIL]
                        // logs at init time for the reason.
                        hero_animated =
                            wcc->registry.all_of<cairns::SkinRef>(ent);
                        const auto* wt =
                            wcc->registry.try_get<cairns::WorldTransform>(ent);
                        cairns::Prefab::Hot* sh =
                            prefabs_.GetHot(prefab_ids_[hero_scene_idx]);
                        if (wt && sh && !sh->meshes.empty()) {
                            cairns::Mesh::Hot* mh =
                                meshes_.GetHot(sh->meshes[0]);
                            if (mh && mh->bind_aabb_min.x <=
                                          mh->bind_aabb_max.x) {
                                glm::vec3 wmin(1.0e30f);
                                glm::vec3 wmax(-1.0e30f);
                                const glm::vec3& mn = mh->bind_aabb_min;
                                const glm::vec3& mx = mh->bind_aabb_max;
                                for (int i = 0; i < 8; ++i) {
                                    const glm::vec3 corner(
                                        (i & 1) ? mx.x : mn.x,
                                        (i & 2) ? mx.y : mn.y,
                                        (i & 4) ? mx.z : mn.z);
                                    const glm::vec4 wc4 =
                                        wt->world * glm::vec4(corner, 1.0f);
                                    const glm::vec3 wc(wc4 / wc4.w);
                                    wmin = glm::min(wmin, wc);
                                    wmax = glm::max(wmax, wc);
                                }
                                hero_min = wmin;
                                hero_max = wmax;
                                hero_has_aabb = true;
                            }
                        }
                    }
                }
            }
            CAIRNS_PRINT(
                    "[PICK] vp=%d xy=(%u,%u) tex_dims=(%u,%u) ok=%d "
                    "id+1=%u hero=%s scene_idx=%u animated=%d "
                    "aabb=[%s%.3f,%.3f,%.3f]-[%.3f,%.3f,%.3f]\n",
                    pick_viewport_, pick_x_, pick_y_, id_target_w_,
                    id_target_h_, ok ? 1 : 0, entity_plus_one,
                    hero_name, hero_scene_idx,
                    hero_animated ? 1 : 0,
                    hero_has_aabb ? "" : "n/a:",
                    hero_min.x, hero_min.y, hero_min.z,
                    hero_max.x, hero_max.y, hero_max.z);
            if (ok) {
                last_pick_result_.viewport = pick_viewport_;
                last_pick_result_.x = pick_x_;
                last_pick_result_.y = pick_y_;
                last_pick_result_.type = cairns::SelectionType::kEntity;
                last_pick_result_.id = entity_plus_one;
                last_pick_result_.raw = entity_plus_one;
                pick_resolved_ = true;
                if (entity_plus_one != 0u) {
                    std::vector<cairns::SelectionTarget> next;
                    next.push_back({cairns::SelectionType::kEntity,
                                    entity_plus_one, 0u});
                    SetHighlights(std::move(next));
                } else {
                    ClearHighlights();
                }
            }
            pick_pending_ = false;
        }

        t_frame.End();
        if (frame_ % 120 == 0) {
            const size_t loaded = prefab_ids_.size();
            size_t entities = 0;
            if (auto* wc = scenes_.GetCold(active_scene_)) {
                entities = wc->registry.storage<entt::entity>().size();
            }
            const size_t slices = loaded > 0 ? entities / loaded : 0;
            CAIRNS_PRINT("============\n");
            CAIRNS_PRINT("draws %zu | %zu GLBs x %zu slices = %zu entities | resolution %u x %u\n",
                         s.drawList.size(), loaded, slices, entities,
                         FrameWidth(), FrameHeight());
            cairns::Timer::PrintReport();
            cairns::Timer::Reset();
        }
        return true;
    }

    // Pick the per-frame swap target. SwapChain and Frames are
    // app-mode-agnostic -- the engine is the one place that knows which
    // texture the swap pass writes into this frame.
    rhi::SwapResolveTarget AcquireFrameSwapTarget() {
        if (final_target_.IsNull()) {
            return swapchain_.AcquireForFrame();
        }
        return rhi_.resources.MakeSurfacelessSwapResolveTarget(
            final_target_, final_target_w_, final_target_h_);
    }

    // Render-thread entry point (post commit 6). Today called synchronously
    // from draw(). Owns: rhi_.frames.Begin/End, the bump-ring EncodeDraws,
    // the compute + render-pass encode. Reads pkt + slots_[pkt.slot].
    void RecordFrame(FramePacket& pkt) {
        [[maybe_unused]] cairns::TaskGuard task_guard;

        PerSlot& s = slots_[pkt.slot];
        // Render-thread RAII slot lock. Main unlocked the slot's mutex
        // before Submit so this lock_guard takes ownership cleanly.
        // Released on scope exit (after EndSubmit + PresentPacket push).
        std::lock_guard<std::mutex> slot_lock(s.slot_mutex);

        // Publish parity early -- a pure function of pkt fields (no GPU
        // dependency) so the game thread's parity_cv wait clears immediately.
        // With N steps per frame, parity_out = parity_in ^ (N & 1).
        pkt.particle_parity_out =
            pkt.particle_parity_in ^ (pkt.sim_steps_this_frame & 1u);
        {
            std::lock_guard<std::mutex> lk(parity_m_);
            latest_parity_out_ = pkt.particle_parity_out;
            latest_parity_frame_ = pkt.frame_idx;
        }
        parity_cv_.notify_all();

        if (pkt.request_dump) {
            rhi_.frame_capture.SetDumpPath(pkt.dump_path);
        }

        // Engine -- not the RHI -- picks the per-frame swap target. Windowed:
        // pull the next drawable from the SwapChain. Surfaceless: hand Frames
        // the engine-owned offscreen, with no drawable so it doesn't present.
        rhi::SwapResolveTarget swap_target = AcquireFrameSwapTarget();
        rhi::FrameContext fc = rhi_.frames.Begin(
            rhi_.resources, rhi_.alloc, rhi_.gpu_profiler,
            rhi_.offscreen_targets, swap_target);
        if (fc.skip_frame) {
            std::lock_guard<std::mutex> lk(present_m_);
            s.present_fc = fc;
            s.present_target = swap_target;
            s.present_ready = true;
            present_cv_.notify_all();
            return;
        }

        cairns::Timer t_record("record", 2);
        EncodeDraws(pkt);

        if (!graph_) {
            graph_ = std::make_unique<rhi::RenderGraph>(rhi_.resources, rhi_.alloc);
            // #210 wire per-slot scratch arenas into the graph's slot table.
            // Bake(slot) resolves slot_arenas_[slot] -> this slot's BumpArena.
            for (uint32_t s = 0; s < kFramesInFlight; ++s) {
                graph_->BindSlotArena(s, slots_[s].arena);
            }
        }
        graph_->Reset();

        // Per-viewport MeshDrawList: same draws, distinct globals_offset.
        // (Same world for both viewports this commit; multi-scene content
        // lands in #195.)
        // #222 Phase A.1: id MRT only when something consumes it (outline
        // overlay or a pending pick this frame). Default path uses the
        // no-id PSO and a single-color forward render pass, saving the
        // R32U store + flat-interp on every visible fragment.
        const bool id_path = !highlights_.empty() || pick_pending_;
        const rhi::Handle<rhi::Shader> forward_pso =
            id_path ? unlit_offscreen_ : unlit_offscreen_noid_;
        std::array<rhi::MeshDrawList, kNumViewports> mls{};
        for (int v = 0; v < active_viewport_count_; ++v) {
            // #195 per-viewport scene: draws stays the FULL list (sorted_draws
            // holds global indices into it); sorted_draws is the sub-span for
            // this viewport's bound scene, so each viewport renders only its
            // own scene's content.
            mls[v].draws = pkt.draws;
            const int sk = s.viewport_scene_idx[v];
            if (sk >= 0 && static_cast<uint32_t>(sk) < s.scene_ranges_count) {
                const PerSlot::SceneDrawRange& r = s.scene_ranges[sk];
                mls[v].sorted_draws =
                    pkt.sorted.subspan(r.draw_lo, r.draw_hi - r.draw_lo);
            } else {
                mls[v].sorted_draws = pkt.sorted;
            }
            mls[v].pipeline = forward_pso;
            // #222 Phase D.2: route set 0 (pass globals) through dyn_globals_.
            mls[v].dyn_globals = dyn_globals_;
            mls[v].globals_offset = s.globals_offset[v];
            mls[v].resident_textures = pkt.resident_textures;
            // #228 H3: resident_buffers left empty (was a 1-element span
            // over mesh_master_handle_ -- field deleted, span never read
            // by the recorder).
            mls[v].resident_buffers = {};
        }

        rhi::PointDraw pd{};
        // #222 Phase A.1 fix: particle PSO must match the forward pass's
        // attachment count -- the id_path branch already swapped the
        // forward unlit PSO; mirror it for the points pipeline.
        pd.pipeline = id_path ? particle_render_offscreen_
                              : particle_render_offscreen_noid_;
        pd.vertex_buffer = particle_ssbo_[pkt.particle_parity_out];
        pd.vertex_offset = 0;
        pd.vertex_count = kParticleCount;

        const float clear[4] = {41.0f / 255.0f, 42.0f / 255.0f, 48.0f / 255.0f, 1.0f};
        const uint32_t fb_w = swap_target.width;
        const uint32_t fb_h = swap_target.height;
        // #194 vp_w/vp_h were sized off kNumViewports (uniform horizontal
        // tiling cap). Now active_viewport_count_ at runtime; layout_rect
        // owns the per-viewport region. Today's default keeps vp_w = full
        // when active=1 -- byte-identical to the pre-#194 single-viewport
        // path.
        const int n_live = std::max(1, active_viewport_count_);
        const uint32_t vp_w = fb_w / static_cast<uint32_t>(n_live);
        const uint32_t vp_h = fb_h;
        // #222 Phase A.1: id targets only allocated when this frame writes
        // them (outline overlay or pending pick). The lazy alloc inside
        // EnsureIdTargets is cheap to skip when no one consumes it.
        if (!highlights_.empty() || pick_pending_) {
            EnsureIdTargets(vp_w, vp_h);
        }
        EnsureHighlightsTex();

        // #221 Skinning P5: pre-skin compute pass. Added BEFORE particle_sim
        // so its output ssbo is ready when the forward pass binds stream 0
        // as a vertex stream (free vertex-fetch sync via the existing
        // compute -> graphics semaphore @ VERTEX_INPUT on Vulkan; encoder
        // boundary handles it on Metal). Gated on non-empty batches AND
        // a valid skin kernel -- absent either, the pass is omitted and
        // the static path is bit-for-bit unchanged. The graph timer wraps
        // this pass with the "skinning_compute" Timer slot (README ratchet).
        if (!pkt.skin_batches.empty() && !skin_kernel_.IsNull() &&
            !skin_output_pool_buffer_.IsNull()) {
            graph_->AddPass(
                "skinning_compute", rhi::PassType::kCompute,
                [&](rhi::PassBuilder& b) {
                    rhi::GraphBufferDesc bd{};
                    bd.usage = rhi::kUsageStorage;
                    rhi::GraphBuffer pool =
                        b.ImportBuffer(skin_output_pool_buffer_, bd);
                    b.WriteBuffer(pool);
                },
                [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                    // #221 Phase 5b: dispatch anim_eval first to fill the
                    // persistent palette_out_buf_ + world_scratch_buf_.
                    // Then SkinDispatchBatch reads palettes from binding 1
                    // pointing at palette_out_buf_, with per-batch dynamic
                    // offset = batch.first_palette_mat4 * sizeof(mat4).
                    PerSlot& s2 = slots_[pkt.slot];
                    const uint32_t n_batches =
                        static_cast<uint32_t>(pkt.skin_batches.size());
                    if (n_batches == 0) {
                        return;
                    }
                    const uint32_t ubo_align = rhi_.alloc.UboAlign();
                    const uint32_t ssbo_align = rhi_.alloc.StorageAlign();
                    // Upload ActorRecords into kDynamic; bind as DYNAMIC_UBO.
                    const uint32_t n_actors =
                        static_cast<uint32_t>(pkt.actor_records.size());
                    if (n_actors > 0 && !anim_eval_kernel_.IsNull() &&
                        anim_eval_tables_uploaded_) {
                        // #222 Phase 0.2: belt-and-braces. BuildSkinFrame
                        // clamps; this catches any future caller that skips
                        // the clamp.
                        assert(n_actors <= kAnimActorsCap);
                        const uint32_t records_bytes = n_actors *
                            static_cast<uint32_t>(sizeof(cairns::GpuActorRecord));
                        uint32_t records_off = 0;
                        void* records_ptr = rhi_.alloc.BumpAllocate(
                            records_bytes, ubo_align,
                            rhi::Memory::kDynamic, &records_off);
                        if (records_ptr) {
                            memcpy(records_ptr, pkt.actor_records.data(),
                                   records_bytes);
                            rhi::CommandRecorder::AnimEvalArgs ae{};
                            ae.scene_headers = scene_headers_buf_;
                            ae.parent_buf = ae_parent_buf_;
                            ae.topo_buf = ae_topo_buf_;
                            ae.bind_pose_buf = ae_bind_pose_buf_;
                            ae.channels_buf = ae_channels_buf_;
                            ae.samplers_buf = ae_samplers_buf_;
                            ae.times_buf = ae_times_buf_;
                            ae.values_buf = ae_values_buf_;
                            ae.joint_nodes_buf = ae_joint_nodes_buf_;
                            ae.inverse_binds_buf = ae_inverse_binds_buf_;
                            ae.world_scratch = world_scratch_buf_;
                            ae.palette_out = palette_out_buf_;
                            // #222 Phase D.3: dyn_set_0 = anim_eval per-FIF
                            // DynamicBuffers set (binding 0 dyn UBO + 1..12
                            // SSBO over backing). vk reads it; metal ignored.
                            ae.dyn_set_0 = dyn_anim_eval_;
                            ae.records_byte_offset = records_off;
                            ae.actor_count = n_actors;
                            cmd.DispatchAnimEval(rhi_.resources, rhi_.alloc,
                                                  anim_eval_kernel_, ae);
                        }
                    }
                    rhi::SkinDispatchBatch* dbatches =
                        s2.arena.AllocateArray<rhi::SkinDispatchBatch>(
                            n_batches);
                    for (uint32_t bi = 0; bi < n_batches; ++bi) {
                        const cairns::SkinBatchGpu& sbg =
                            pkt.skin_batches[bi];
                        const cairns::Mesh::Hot* mhot =
                            meshes_.GetHot(sbg.mesh);
                        rhi::SkinDispatchBatch& db = dbatches[bi];
                        db = rhi::SkinDispatchBatch{};
                        const rhi::Handle<rhi::Buffer> mesh_skin_buf =
                            mhot ? ResolvedSharedSkin(*mhot)
                                  : rhi::Handle<rhi::Buffer>::Null;
                        if (!mhot ||
                            mhot->posHandle.IsNull() ||
                            mesh_skin_buf.IsNull()) {
                            continue;
                        }
                        struct SkinParamsCpu {
                            uint32_t instance_count;
                            uint32_t vertex_count;
                            uint32_t joint_count;
                            uint32_t mode;
                        };
                        SkinParamsCpu params{};
                        params.instance_count = sbg.instance_count;
                        params.vertex_count = sbg.vertex_count;
                        params.joint_count = sbg.joint_count;
                        params.mode = 0u;
                        uint32_t params_off = 0;
                        void* params_ptr = rhi_.alloc.BumpAllocate(
                            sizeof(SkinParamsCpu), ubo_align,
                            rhi::Memory::kDynamic, &params_off);
                        if (!params_ptr) {
                            continue;
                        }
                        memcpy(params_ptr, &params, sizeof(SkinParamsCpu));

                        // #221 Phase 5b: palettes live in palette_out_buf_;
                        // per-batch dynamic offset selects the bucket window
                        // in mat4 stride. instance_meta.x stays
                        // bucket-relative (cursor * joint_count).
                        const uint32_t pal_off =
                            sbg.first_palette_mat4 *
                            static_cast<uint32_t>(sizeof(glm::mat4));
                        // InstanceMeta window for this batch.
                        const uint32_t meta_start = sbg.first_meta;
                        const uint32_t meta_count = sbg.instance_count;
                        const uint32_t meta_bytes = meta_count *
                            static_cast<uint32_t>(sizeof(glm::uvec2));
                        uint32_t meta_off = 0;
                        void* meta_ptr = rhi_.alloc.BumpAllocate(
                            meta_bytes ? meta_bytes : 8u, ssbo_align,
                            rhi::Memory::kDynamic, &meta_off);
                        if (meta_bytes > 0 && meta_ptr) {
                            memcpy(meta_ptr,
                                   pkt.instance_meta.data() + meta_start,
                                   meta_bytes);
                        }

                        // Group A descriptor set (Vulkan); null on Metal.
                        db.mesh_set = mhot->skin_group_a;
                        db.pos_buffer = mhot->posHandle;
                        db.pos_byte_offset =
                            mhot->global_base_vertex *
                            static_cast<uint32_t>(sizeof(glm::vec4));
                        db.skin_attr_buffer = mesh_skin_buf;
                        db.skin_attr_byte_offset =
                            mhot->skin_attr_base_vertex *
                            static_cast<uint32_t>(
                                sizeof(cairns::PackedSkinVertex));
                        db.params_byte_offset = params_off;
                        db.palettes_byte_offset = pal_off;
                        db.instance_meta_byte_offset = meta_off;
                        db.workgroups = sbg.workgroups;
                        db.instance_count = sbg.instance_count;
                    }
                    // #222 Phase D.3: palette_out_buf_ threaded as param;
                    // SkinDispatchBatch::palette_buffer retired.
                    // dyn_skin_group_b_ owns the per-FIF set (vk) +
                    // is ignored on metal.
                    cmd.DispatchSkinBatches(
                        rhi_.resources, rhi_.alloc, skin_kernel_,
                        skin_output_pool_buffer_, palette_out_buf_,
                        dyn_skin_group_b_,
                        std::span<const rhi::SkinDispatchBatch>(
                            dbatches, n_batches));
                });
        }

        // pass 1: particle_sim kCompute. import the writer ssbo so prune keeps
        // it (external side effect -- game thread reads particle_parity_out).
        // A.2 gate: when particles_enabled_=false the pass is omitted entirely;
        // sim_out stays default-null, no readers downstream so prune drops it.
        rhi::GraphBuffer sim_out;
        if (particles_enabled_) {
        graph_->AddPass(
            "particle_sim", rhi::PassType::kCompute,
            [&](rhi::PassBuilder& b) {
                rhi::GraphBufferDesc bd{};
                bd.usage = rhi::kUsageStorage;
                sim_out = b.ImportBuffer(
                    particle_ssbo_[pkt.particle_parity_out], bd);
                b.WriteBuffer(sim_out);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                rhi::ComputeDispatch cd{};
                cd.kernel = particle_kernel_;
                cd.groups_x = kParticleCount / 256;
                cd.local_x = 256;
                // Metal: sequential ComputeCommandEncoders self-hazard on R/W
                // ordering (MTLHazardTrackingModeTracked). Vulkan: recorder
                // emits a compute->compute pipeline barrier between dispatches.
                for (uint32_t k = 0; k < pkt.sim_steps_this_frame; ++k) {
                    const uint32_t step_src =
                        pkt.particle_parity_in ^ (k & 1u);
                    // #222 Phase D.4: parity DynamicBuffers holds bindings
                    // 1+2 pre-bound; dyn offset carries dt only.
                    cd.dyn_set_0 = dyn_particle_parity_[step_src];
                    cd.dyn_offset_0 = s.dt_off;
                    cd.step_index = k;
                    cmd.Dispatch(rhi_.resources, rhi_.alloc, cd);
                }
            });
        }  // particles_enabled_

        // pass 2: forward, ONCE PER VIEWPORT. Each pass writes to a private
        // half-width color+depth target. Particles render into both viewports
        // (compute step ran once above; particle render is a graphics
        // submission that draws into each forward pass's encoder).
        std::array<rhi::GraphTexture, kNumViewports> color_off{};
        std::array<rhi::GraphTexture, kNumViewports> depth_off{};
        std::array<rhi::GraphTexture, kNumViewports> id_off{};
        for (int v = 0; v < active_viewport_count_; ++v) {
            const int vp_idx = v;
            const char* pass_name = (vp_idx == 0) ? "forward_vp0" : "forward_vp1";
            graph_->AddPass(
                pass_name, rhi::PassType::kGraphics,
                [&, vp_idx](rhi::PassBuilder& b) {
                    rhi::GraphTextureDesc cd{};
                    cd.width = vp_w;
                    cd.height = vp_h;
                    cd.format = rhi::Format::kBgra8Unorm;
                    cd.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled;
                    color_off[vp_idx] = b.CreateColorTarget(cd);
                    rhi::GraphTextureDesc dd{};
                    dd.width = vp_w;
                    dd.height = vp_h;
                    dd.format = rhi::Format::kD32F;
                    dd.usage = rhi::kTexUsageDepthTarget | rhi::kTexUsageSampled;
                    depth_off[vp_idx] = b.CreateDepthTarget(dd);
                    b.AddColorOutput("color", color_off[vp_idx], rhi::LoadOp::kClear, clear);
                    if (id_path) {
                        // #207 R32U id buffer (MRT). Persistent (engine-owned via
                        // id_target_[vp]) so end-of-frame pick can copyImageToBuffer
                        // a 1x1 region after the render thread drains. Importing
                        // skips the transient pool aliasing race that would
                        // otherwise reuse the texture before readback.
                        // #222 Phase A.1: only attached when outline or pick
                        // wants it -- frees the per-frag R32U store otherwise.
                        rhi::GraphTextureDesc id_desc{};
                        id_desc.width = vp_w;
                        id_desc.height = vp_h;
                        id_desc.format = rhi::Format::kR32Uint;
                        id_desc.usage = rhi::kTexUsageColorTarget |
                                         rhi::kTexUsageSampled |
                                         rhi::kTexUsageTransferSrc;
                        id_off[vp_idx] = b.ImportTexture(id_target_[vp_idx], id_desc);
                        const float id_clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        b.AddColorOutput("id", id_off[vp_idx], rhi::LoadOp::kClear, id_clear);
                    }
                    b.AddDepthOutput("fwd_depth", depth_off[vp_idx], rhi::LoadOp::kClear, 1.0f);
                },
                [&, vp_idx](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                    cmd.DrawMeshes(rhi_.resources, rhi_.alloc, mls[vp_idx]);
                    // A.2 + A.5 gate: global particles_enabled_ gates the
                    // compute sim above; per-viewport
                    // Viewport::Cold::particles_enabled gates each
                    // viewport's particle draw. G3 drives the per-vp split.
                    bool vp_particles = true;
                    if (auto* vc = viewports_.GetCold(viewport_ids_[vp_idx])) {
                        vp_particles = vc->particles_enabled;
                    }
                    if (particles_enabled_ && vp_particles) {
                        cmd.DrawPoints(rhi_.resources, rhi_.alloc, pd);
                    }
                    // A.3: L1 single red triangle. tiny_quad_test_ flips this
                    // on; with no glbs loaded DrawMeshes is a no-op, so the
                    // captured frame is pipeline + clear + this one draw.
                    // Hard-coded NDC triangle from gl_VertexIndex; the shader
                    // ignores its texture/sampler bindings but DrawFullscreen
                    // unconditionally dereferences the sampler handle, so we
                    // pass composite_sampler_ (already created).
                    if (tiny_quad_test_) {
                        cmd.DrawFullscreen(rhi_.resources, red_triangle_pip_,
                            std::span<const rhi::Handle<rhi::Texture>>{},
                            composite_sampler_);
                    }
                });
        }

        // #207 pass 2.5: outline. Per viewport, fullscreen tri samples
        // color_off + id_off, edge-detects on the id channel, tints yellow on
        // discontinuities. Today unlit emits id=0 for every fragment so
        // outline is a structural no-op (every pixel passes the centre==0
        // early-out); when per-draw {type|id} encoding lands this pass
        // produces visible silhouettes. We only insert the pass when the
        // engine carries highlights, so the no-op cost is zero by default.
        std::array<rhi::GraphTexture, kNumViewports> outline_off{};
        // #224 L8: editor-chrome separation. The selection outline IS
        // editor chrome -- meta-UI that marks "this entity is selected
        // *in the editor*", drawn on top of the scene. Stylized
        // highlight (rim light / toon / ink) is IN-CANVAS ART, lives
        // in the material path, and is unaffected by this gate. The
        // remixer canvas calls cairns.editor.chrome({on:false}) before
        // a capture or scroll so the selection outline drops out but
        // the stylized look survives. selection STATE (highlights_) is
        // preserved -- only the outline-pass DRAWING is suppressed.
        const bool outline_on =
            editor_chrome_enabled_ && !highlights_.empty();
        if (outline_on) {
            for (int v = 0; v < active_viewport_count_; ++v) {
                const int vp_idx = v;
                const char* pass_name =
                    (vp_idx == 0) ? "outline_vp0" : "outline_vp1";
                graph_->AddPass(
                    pass_name, rhi::PassType::kGraphics,
                    [&, vp_idx](rhi::PassBuilder& b) {
                        rhi::GraphTextureDesc od{};
                        od.width = vp_w;
                        od.height = vp_h;
                        od.format = rhi::Format::kBgra8Unorm;
                        od.usage = rhi::kTexUsageColorTarget |
                                   rhi::kTexUsageSampled;
                        outline_off[vp_idx] = b.CreateColorTarget(od);
                        b.AddAttachmentInput(color_off[vp_idx]);
                        b.AddAttachmentInput(id_off[vp_idx]);
                        // Import highlights_tex_ as a graph input so its
                        // SHADER_READ_ONLY layout transition is emitted by
                        // BeginRenderPass before DrawFullscreen samples it.
                        rhi::GraphTextureDesc hd{};
                        hd.width = kMaxHighlights + 1;
                        hd.height = 1;
                        hd.format = rhi::Format::kR32Uint;
                        hd.usage = rhi::kTexUsageSampled;
                        rhi::GraphTexture hg =
                            b.ImportTexture(highlights_tex_, hd);
                        b.AddAttachmentInput(hg);
                        const float oclear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        b.AddColorOutput("outline_color", outline_off[vp_idx],
                                         rhi::LoadOp::kClear, oclear);
                    },
                    [&, vp_idx](rhi::CommandRecorder& cmd,
                                const rhi::PassResources& res) {
                        const rhi::Handle<rhi::Texture> srcs[3] = {
                            res.Resolve(color_off[vp_idx]),
                            res.Resolve(id_off[vp_idx]),
                            highlights_tex_,
                        };
                        cmd.SetViewport(0.0f, 0.0f, static_cast<float>(vp_w),
                                        static_cast<float>(vp_h));
                        cmd.SetScissor(0, 0, vp_w, vp_h);
                        cmd.DrawFullscreen(
                            rhi_.resources, outline_pip_,
                            std::span<const rhi::Handle<rhi::Texture>>(srcs, 3),
                            outline_sampler_);
                    });
            }
        }

        // pass 3: composite + ui kGraphics. Composite samples color_off full-
        // screen, depth_off PIP in the bottom-right; ImGui draws on top into the
        // same encoder. Both backends use MSAA swapchain renderpasses where the
        // MSAA color attachment storeOp is "resolve + don't-keep-MSAA" (Adreno
        // tile-residency optimization). Two back-to-back render passes targeting
        // the same swap framebuffer would either clear or load undefined MSAA
        // between passes, so we keep them under one encoder. The graph still
        // expresses the dependencies (this pass reads color_off + depth_off
        // produced by forward) -- "ui" is conceptually a separate phase that
        // physically shares the swap encoder for MSAA reasons.
        rhi::GraphTexture swap_tex;
        graph_->AddPass(
            "swap", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                rhi::GraphTextureDesc td{};
                td.width = fb_w;
                td.height = fb_h;
                // Surfaceless: import final_target_ as the swap output so
                // BeginRenderPass routes through the offscreen-target-cache
                // (target.IsNull() == false). Windowed: import null and let
                // BeginRenderPass take the is_swapchain branch.
                const rhi::Handle<rhi::Texture> swap_handle =
                    final_target_.IsNull() ? rhi::Handle<rhi::Texture>::Null
                                            : final_target_;
                swap_tex = b.ImportTexture(swap_handle, td);
                b.AddColorOutput("swapchain", swap_tex, rhi::LoadOp::kClear, clear);
                for (int v = 0; v < active_viewport_count_; ++v) {
                    // #207 swap reads outline_off when the outline pass ran
                    // this frame, else color_off. Both are sampled-readonly.
                    b.AddAttachmentInput(outline_on ? outline_off[v]
                                                    : color_off[v]);
                    b.AddAttachmentInput(depth_off[v]);
                }
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources& res) {
                std::array<rhi::Handle<rhi::Texture>, kNumViewports> vp_color{};
                std::array<rhi::Handle<rhi::Texture>, kNumViewports> vp_depth{};
                for (int v = 0; v < active_viewport_count_; ++v) {
                    vp_color[v] = res.Resolve(outline_on ? outline_off[v]
                                                          : color_off[v]);
                    vp_depth[v] = res.Resolve(depth_off[v]);
                }
                // #194 composite each LIVE viewport into its layout_rect
                // region of the swap pane. layout_rect = (x,y,w,h) in NDC
                // [0..1]. Default for vp 0 is full pane (1,1); follow-up
                // viewports set their own rects via cairns.viewport.setLayout.
                // Skip zero-area rects (uninitialised / disabled).
                const float fb_fw = static_cast<float>(fb_w);
                const float fb_fh = static_cast<float>(fb_h);
                for (int v = 0; v < active_viewport_count_; ++v) {
                    const glm::vec4& rect =
                        viewports_.GetHot(viewport_ids_[v])->layout_rect;
                    if (rect.z <= 0.0f || rect.w <= 0.0f) {
                        continue;
                    }
                    const float rx = rect.x * fb_fw;
                    const float ry = rect.y * fb_fh;
                    const float rw = rect.z * fb_fw;
                    const float rh = rect.w * fb_fh;
                    cmd.SetViewport(rx, ry, rw, rh);
                    cmd.SetScissor(static_cast<int32_t>(rx),
                                   static_cast<int32_t>(ry),
                                   static_cast<uint32_t>(rw),
                                   static_cast<uint32_t>(rh));
                    cmd.DrawFullscreen(rhi_.resources, composite_pip_,
                                       std::span<const rhi::Handle<rhi::Texture>>(&vp_color[v], 1),
                                       composite_sampler_);
                }
                // Active viewport's depth PIP (bottom-right 25% of the active
                // viewport's layout_rect). A.3: skip in golden mode -- it's
                // a debug overlay that polluted every L1..L7 capture with a
                // black corner (depthviz of an empty depth buffer = 0
                // brightness).
                if (!golden_) {
                    const glm::vec4& av_rect =
                        viewports_.GetHot(active_viewport_)->layout_rect;
                    const float av_w = av_rect.z * fb_fw;
                    const float av_h = av_rect.w * fb_fh;
                    const float vp_x0 = av_rect.x * fb_fw;
                    const float vp_y0 = av_rect.y * fb_fh;
                    const float pip_x = vp_x0 + 0.75f * av_w;
                    const float pip_y = vp_y0 + 0.75f * av_h;
                    const float pip_w = 0.25f * av_w;
                    const float pip_h = 0.25f * av_h;
                    cmd.SetViewport(pip_x, pip_y, pip_w, pip_h);
                    cmd.SetScissor(static_cast<int32_t>(pip_x),
                                   static_cast<int32_t>(pip_y),
                                   static_cast<uint32_t>(pip_w),
                                   static_cast<uint32_t>(pip_h));
                    cmd.DrawFullscreen(rhi_.resources, depthviz_,
                                       std::span<const rhi::Handle<rhi::Texture>>(&vp_depth[active_viewport_index_], 1),
                                       composite_sampler_);
                }
                // Restore full extent before the ui draw.
                cmd.SetViewport(0.0f, 0.0f, static_cast<float>(fb_w),
                                static_cast<float>(fb_h));
                cmd.SetScissor(0, 0, fb_w, fb_h);
                if (pkt.imgui_snapshot) {
                    cmd.DrawImGui(rhi_.resources, rhi_.alloc, imgui_, imgui_font_,
                                  imgui_sampler_, pkt.imgui_snapshot);
                }
            });

        graph_->SetOutput(swap_tex);
        if (!graph_->Bake(pkt.slot) || !graph_->Execute(fc, swap_target)) {
            t_record.End();
            rhi_.frames.EndSubmit(swap_target, rhi_.frame_capture, fc);
            {
                std::lock_guard<std::mutex> lk(present_m_);
                s.present_fc = fc;
                s.present_target = swap_target;
                s.present_ready = true;
            }
            present_cv_.notify_all();
            return;
        }
        if (frame_ <= 6) {
            fprintf(stderr, "[FLAKE-R] frame=%u slot=%u img=%u steps=%u",
                    frame_, pkt.slot, fc.swapchain_image_index,
                    pkt.sim_steps_this_frame);
            for (int v = 0; v < active_viewport_count_; ++v) {
                const rhi::Handle<rhi::Texture> coff =
                    graph_->ResolveTexture(color_off[v]);
                const rhi::Handle<rhi::Texture> doff =
                    graph_->ResolveTexture(depth_off[v]);
                fprintf(stderr, " vp%d_color=%u/%u vp%d_depth=%u/%u", v,
                        coff.index, coff.generation, v,
                        doff.index, doff.generation);
            }
            fprintf(stderr, "\n");
        }
        t_record.End();
        rhi_.frames.EndSubmit(swap_target, rhi_.frame_capture, fc);
        {
            std::lock_guard<std::mutex> lk(present_m_);
            s.present_fc = fc;
            s.present_target = swap_target;
            s.present_ready = true;
        }
        present_cv_.notify_all();
    }

    bool initRenderPipeline() {
        {
            // #220 Step 1: set-2 per-material bind groups now live IN the
            // material's Hot record (cairns::ResourceManager<Material>).
            // Walk every live material; build its BindGroup from Cold's
            // texture+sampler; store into Hot.set2. Replaces the parallel
            // material_bind_groups_ vector that had a fragile size-parity
            // invariant with materials_.
            materials_.ForEachLive(
                [&](cairns::Material::Hot& hot,
                    cairns::Material::Cold& cold) {
                    const rhi::TextureBinding tb{0, cold.color};
                    const rhi::SamplerBinding sb{0, cold.sampler};
                    rhi::BindGroupDesc bgd{};
                    bgd.textures = std::span<const rhi::TextureBinding>(&tb, 1);
                    bgd.samplers = std::span<const rhi::SamplerBinding>(&sb, 1);
                    hot.set2 = rhi_.resources.CreateBindGroup(bgd);
                });
        }

        {  // unlit graphics pipeline via rhi
            const std::string shader_dir = cairns::GetBasePathSafe();
            const rhi::VertexInputAttribute vtx_attrs[2] = {
                {0, cairns::kMeshPosBindSlot, rhi::Format::kRgba32F, 0},
                // stream 1: uv at offset 48 in the 64-byte VertexAttribute.
                {1, cairns::kMeshAttrVertexBindSlot, rhi::Format::kRg32F, 48},
            };
            const rhi::VertexBufferLayout vtx_layouts[2] = {
                {cairns::kMeshPosBindSlot, static_cast<uint32_t>(sizeof(glm::vec4))},
                {cairns::kMeshAttrVertexBindSlot, 64},
            };
            rhi::GraphicsPipelineDesc desc{};
            desc.logical_shader = "unlit";
            desc.shader_dir = shader_dir.c_str();
            desc.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(vtx_attrs, 2);
            desc.vertex_buffers =
                std::span<const rhi::VertexBufferLayout>(vtx_layouts, 2);
            desc.topology = rhi::PrimitiveTopology::kTriangleList;
            desc.cull = rhi::CullMode::kBack;
            desc.front_face = rhi::FrontFace::kCounterClockwise;
            desc.depth_test = true;
            desc.depth_write = true;
            desc.depth_compare = rhi::CompareOp::kLess;
            desc.color_format = rhi::Format::kBgra8Unorm;
            desc.depth_format = rhi::Format::kD32F;
            desc.sample_count = sampleCount;
            desc.push_constant_bytes = 0;  // base_vertex no longer needed (attrs are a vertex stream)
            // #242: unlit_ swap-PSO retired -- it used the swapchain
            // renderpass (1 color) with unlit.frag (writes outId at
            // location 1), tripping VUID Undefined-Value-ShaderOutputNotConsumed.
            // The forward pass uses the offscreen variants
            // (unlit_offscreen_ / unlit_offscreen_noid_) instead; this
            // PSO never made it to a vkCmdDraw.

            // Offscreen variant: single-sample, no swapchain compat. Same shaders
            // + vertex layout as unlit; targets a render-graph color_off+depth_off.
            // #206 MRT: forward pass writes {BGRA color, R32U id} -- pipeline
            // declares both formats so vk renderpass compat matches the
            // 2-attachment offscreen renderpass cache key.
            rhi::GraphicsPipelineDesc ofd = desc;
            ofd.logical_shader = "unlit_offscreen";
            ofd.sample_count = 1;
            ofd.swap_chain = nullptr;
            ofd.color_formats[0] = rhi::Format::kBgra8Unorm;
            ofd.color_formats[1] = rhi::Format::kR32Uint;
            ofd.color_count = 2;
            ofd.debug_name = "unlit_offscreen";
            unlit_offscreen_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, ofd);
            if (unlit_offscreen_.IsNull()) {
                std::exit(0);
            }

            // #222 Phase A.1: id-less variant. Single color attachment, no
            // R32U write in the fragment shader. Selected by RecordFrame
            // when no consumer wants the id channel this frame.
            rhi::GraphicsPipelineDesc nid = desc;
            nid.logical_shader = "unlit_offscreen_noid";
            nid.sample_count = 1;
            nid.swap_chain = nullptr;
            nid.color_formats[0] = rhi::Format::kBgra8Unorm;
            nid.color_count = 1;
            nid.debug_name = "unlit_offscreen_noid";
            unlit_offscreen_noid_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, nid);
            if (unlit_offscreen_noid_.IsNull()) {
                std::exit(0);
            }

            // composite_pip: full-screen tri, samples 1 color tex, writes the
            // swap target. Surfaceless: final_target_ is 1-sample / no-depth,
            // so the pipeline is compat with a 1-sample / no-depth renderpass.
            // Windowed: swapchain renderpass is MSAA + depth attachments.
            const bool surfaceless_pipe = !final_target_.IsNull();
            rhi::GraphicsPipelineDesc cpd{};
            cpd.logical_shader = "composite_pip";
            cpd.shader_dir = shader_dir.c_str();
            cpd.topology = rhi::PrimitiveTopology::kTriangleList;
            cpd.cull = rhi::CullMode::kNone;
            cpd.depth_test = false;
            cpd.depth_write = false;
            cpd.color_format = rhi::Format::kBgra8Unorm;
            cpd.depth_format = surfaceless_pipe ? rhi::Format::kUndefined
                                                : rhi::Format::kD32F;
            cpd.sample_count = surfaceless_pipe ? 1u : sampleCount;
            cpd.debug_name = "composite_pip";
            cpd.swap_chain = surfaceless_pipe ? nullptr : &swapchain_;
            composite_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, cpd);

            // depthviz: same shape, samples 1 depth tex, blue silhouette.
            rhi::GraphicsPipelineDesc dvd = cpd;
            dvd.logical_shader = "depthviz";
            dvd.debug_name = "depthviz";
            depthviz_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, dvd);

            // A.3: red_triangle. Drawn inside the forward pass so attachment
            // shape must match unlit's (BGRA8 + D32F + sampleCount MSAA).
            // depth_test/write off so we don't actually touch the depth
            // buffer; cull=none because gl_VertexIndex winding is fixed.
            rhi::GraphicsPipelineDesc rtd{};
            rtd.logical_shader = "red_triangle";
            rtd.debug_name = "red_triangle";
            rtd.shader_dir = shader_dir.c_str();
            rtd.topology = rhi::PrimitiveTopology::kTriangleList;
            rtd.cull = rhi::CullMode::kNone;
            rtd.depth_test = false;
            rtd.depth_write = false;
            rtd.color_format = rhi::Format::kBgra8Unorm;
            rtd.depth_format = rhi::Format::kD32F;
            rtd.sample_count = sampleCount;
            red_triangle_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, rtd);

            // #207 outline: same shape; samples color_off + id_off (2 textures
            // via the shared composite descriptor layout), renders into
            // outline_off (BGRA, same dims as color_off). Surfaceless: targets
            // a 1-sample / no-depth render pass. Windowed: still 1-sample /
            // no-depth because the outline pass writes to a graph color
            // target, NOT directly to the MSAA swapchain renderpass.
            rhi::GraphicsPipelineDesc opd = cpd;
            opd.logical_shader = "outline";
            opd.debug_name = "outline";
            opd.depth_format = rhi::Format::kUndefined;
            opd.sample_count = 1u;
            opd.swap_chain = nullptr;
            outline_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, opd);

            if (composite_pip_.IsNull() || depthviz_.IsNull() ||
                outline_pip_.IsNull() || red_triangle_pip_.IsNull()) {
                std::exit(0);
            }

            rhi::SamplerDesc sd{};
            sd.min_filter = rhi::Filter::kLinear;
            sd.mag_filter = rhi::Filter::kLinear;
            sd.mip_filter = rhi::Filter::kLinear;
            sd.address_mode = rhi::AddressMode::kClampToEdge;
            composite_sampler_ = rhi_.resources.CreateSampler(sd);

            rhi::SamplerDesc nsd{};
            nsd.min_filter = rhi::Filter::kNearest;
            nsd.mag_filter = rhi::Filter::kNearest;
            nsd.mip_filter = rhi::Filter::kNearest;
            nsd.address_mode = rhi::AddressMode::kClampToEdge;
            outline_sampler_ = rhi_.resources.CreateSampler(nsd);
        }

        return true;
    }

    struct Particle {
        float position[2];
        float velocity[2];
        float color[4];
    };

    // #221 Phase 9: opportunistic SkinnedAttachment factory. Resolves the
    // scene, picks the walking clip (`SelectWalkingClip`), finds the first
    // skinned mesh (cpuSkinAttrs non-empty), allocates a skin_output_pool_
    // slice sized to that mesh's vertex count, and returns the new SkinId.
    // Null on any miss (no skins, no skinned mesh, no clips, no pool
    // capacity left). The actor's per-frame palette uses the stored
    // clip_index + time_offset and writes deformed verts at slice.offset.
    cairns::SkinId TryCreateSkinForScene(cairns::PrefabId scene_id,
                                          float time_offset) {
        // #222: loud reason for every Null return so we don't silently
        // drop heroes to bind pose. Names the scene so the user can map
        // back to a GLB filename via prefab_ids_[scene_idx].
        auto fail = [&](const char* why) -> cairns::SkinId {
            CAIRNS_PRINT_ERR(
                "[SKIN-FAIL] scene_id=(idx=%u,gen=%u) reason=%s\n",
                static_cast<unsigned>(scene_id.index),
                static_cast<unsigned>(scene_id.generation), why);
            return cairns::SkinId::Null;
        };
        cairns::Prefab::Hot* shot = prefabs_.GetHot(scene_id);
        cairns::Prefab::Cold* scold = prefabs_.GetCold(scene_id);
        if (!shot || !scold) {
            return fail("scene handle dead");
        }
        if (scold->skins.empty()) {
            return fail("scold.skins empty");
        }
        if (scold->clips.empty()) {
            return fail("scold.clips empty");
        }
        const int clip_idx =
            cairns::SelectWalkingClip(scold->clips);
        if (clip_idx < 0) {
            return fail("SelectWalkingClip returned -1");
        }
        cairns::Handle<cairns::Mesh> skinned_mesh;
        uint32_t vert_count = 0;
        for (cairns::Handle<cairns::Mesh> mid : shot->meshes) {
            cairns::Mesh::Hot* mhot = meshes_.GetHot(mid);
            if (mhot && !mhot->attr_skinned_alias.IsNull() &&
                mhot->vert_count > 0) {
                skinned_mesh = mid;
                vert_count = mhot->vert_count;
                break;
            }
        }
        if (skinned_mesh.IsNull() || vert_count == 0) {
            return fail("no mesh with attr_skinned_alias + vert_count");
        }
        cairns::PoolSlice slice = skin_output_pool_.Alloc(vert_count);
        if (!slice.IsValid()) {
            CAIRNS_PRINT_ERR(
                "[FATAL] skin_output_pool_ exhausted at 256 MB cap "
                "(Adreno maxStorageBufferRange floor). vert_count=%u. "
                "Reduce hero count or bake skin output offline.\n",
                vert_count);
            std::abort();
        }
        cairns::SkinId sid = skins_.Acquire();
        cairns::Prefab::Hot* scene_hot = prefabs_.GetHot(scene_id);
        // #222 Phase E.6: build the per-actor pos_stream alias of
        // skin_output_pool_buffer_, pre-offset to slice.offset * 16 B.
        // Skinned draws point Draw::vertex_buffers[0] at this handle;
        // Draw::pos_buffer_byte_offset retires.
        rhi::Handle<rhi::Buffer> pos_stream_h =
            rhi::Handle<rhi::Buffer>::Null;
        if (!skin_output_pool_buffer_.IsNull()) {
            // CRITICAL: snapshot pool fields BEFORE the next Acquire.
            // ResourceManager::Acquire does hot_.emplace_back() which may
            // reallocate the underlying std::vector -- any Hot* fetched
            // earlier becomes dangling. The skinned-rendering "exploded
            // triangles" regression was exactly this UB read.
            uint16_t pool_heap_idx = 0;
            uint32_t pool_off = 0;
            {
                rhi::Buffer::Hot* pool_hot =
                    rhi_.resources.GetHot(skin_output_pool_buffer_);
                if (!pool_hot) {
                    return cairns::SkinId::Null;
                }
                pool_heap_idx = pool_hot->heap_buffer_index;
                pool_off = pool_hot->offset_in_heap;
            }
            pos_stream_h = rhi_.resources.buffers.Acquire();
            rhi::Buffer::Hot* alias_hot =
                rhi_.resources.buffers.GetHot(pos_stream_h);
            alias_hot->heap_buffer_index = pool_heap_idx;
            alias_hot->offset_in_heap =
                pool_off +
                slice.offset * static_cast<uint32_t>(sizeof(glm::vec4));
        }
        if (auto* h = skins_.GetHot(sid)) {
            *h = cairns::SkinnedAttachment::Hot{};
            h->slice_offset = slice.offset;
            h->joint_count =
                static_cast<uint32_t>(scold->skins[0].jointNodes.size());
            h->time_offset = time_offset;
            h->time_scale = 1.0f;
            h->mesh = skinned_mesh;
            // #222 Phase H.5: cache the per-frame double-resolve.
            h->gpu_prefab_header_idx =
                scene_hot ? scene_hot->gpu_prefab_header_idx : UINT32_MAX;
            h->gpu_clip_duration =
                (scold->gpu_clip_duration > 0.0f) ? scold->gpu_clip_duration
                                                   : 1.0f;
            h->pos_stream = pos_stream_h;
        }
        if (auto* c = skins_.GetCold(sid)) {
            *c = cairns::SkinnedAttachment::Cold{};
            c->scene = scene_id;
            c->skin_index = 0;
            c->clip_index = clip_idx;
            c->slice = slice;  // #222 Phase H.5 finish: Free metadata here.
        }
        return sid;
    }

    // #221 Phase 5: per-frame skin pipeline (game-thread side). Walks the
    // active scene for SkinRef entities, samples each actor's clip into a
    // per-slot palette slab on the arena, buckets visible actors by mesh
    // (flat-array prefix-sum, NO map per standing rule), emits SkinBatchGpu
    // rows + flat InstanceMeta + flat palettes, publishes spans on s.pkt.
    // Today: no SkinRef in the registry => skin_batches empty. Static path
    // bit-for-bit unchanged.
    void BuildSkinFrame(uint32_t slot) {
        PerSlot& s = slots_[slot];
        s.pkt.skin_batches = std::span<const cairns::SkinBatchGpu>{};
        s.pkt.palettes = std::span<const glm::mat4>{};
        s.pkt.instance_meta = std::span<const glm::uvec2>{};
        s.pkt.actor_records = std::span<const cairns::GpuActorRecord>{};

        if (skin_kernel_.IsNull() || skin_output_pool_buffer_.IsNull()) {
            return;
        }
        if (anim_eval_kernel_.IsNull() || !anim_eval_tables_uploaded_) {
            return;
        }
        cairns::Scene::Cold* wc = scenes_.GetCold(active_scene_);
        if (!wc) {
            return;
        }
        // #222 Phase S.3 step 2: cull skinned actors against the active
        // viewport frustum. Extract 6 planes from view_proj (clip-space
        // boundary planes mapped back: row3 ± rowK). Per-actor: transform
        // bind-pose AABB corners by WorldTransform.world, pad by 1.5x for
        // animation motion, test each plane. Skip the actor's anim_eval
        // record AND skinning_compute batch when fully outside.
        const glm::mat4& vp_for_cull =
            s.pending_globals[active_viewport_index_].view_proj;
        glm::vec4 cull_planes[6];
        {
            const glm::mat4 m = glm::transpose(vp_for_cull);
            cull_planes[0] = m[3] + m[0];  // left
            cull_planes[1] = m[3] - m[0];  // right
            cull_planes[2] = m[3] + m[1];  // bottom
            cull_planes[3] = m[3] - m[1];  // top
            cull_planes[4] = m[3] + m[2];  // near (ZO)
            cull_planes[5] = m[3] - m[2];  // far
            for (int i = 0; i < 6; ++i) {
                const float L = glm::length(glm::vec3(cull_planes[i]));
                if (L > 0.0f) {
                    cull_planes[i] /= L;
                }
            }
        }
        auto aabb_outside =
            [&](const glm::vec3& mn, const glm::vec3& mx) -> bool {
                for (int i = 0; i < 6; ++i) {
                    const glm::vec3 n(cull_planes[i]);
                    const float d = cull_planes[i].w;
                    const glm::vec3 p(
                        n.x >= 0.0f ? mx.x : mn.x,
                        n.y >= 0.0f ? mx.y : mn.y,
                        n.z >= 0.0f ? mx.z : mn.z);
                    if (glm::dot(n, p) + d < 0.0f) {
                        return true;
                    }
                }
                return false;
            };
        auto actor_world_aabb =
            [&](const cairns::Mesh::Hot* mhot, const glm::mat4& world,
                glm::vec3* out_min, glm::vec3* out_max) {
                if (mhot->bind_aabb_min.x > mhot->bind_aabb_max.x) {
                    *out_min = glm::vec3(-1.0e30f);
                    *out_max = glm::vec3( 1.0e30f);
                    return;
                }
                const glm::vec3 c =
                    0.5f * (mhot->bind_aabb_min + mhot->bind_aabb_max);
                const glm::vec3 h =
                    1.5f * 0.5f * (mhot->bind_aabb_max - mhot->bind_aabb_min);
                glm::vec3 wmin( 1.0e30f);
                glm::vec3 wmax(-1.0e30f);
                for (int i = 0; i < 8; ++i) {
                    const glm::vec3 corner(
                        c.x + ((i & 1) ? h.x : -h.x),
                        c.y + ((i & 2) ? h.y : -h.y),
                        c.z + ((i & 4) ? h.z : -h.z));
                    const glm::vec4 wc4 = world * glm::vec4(corner, 1.0f);
                    const glm::vec3 wc(wc4 / wc4.w);
                    wmin = glm::min(wmin, wc);
                    wmax = glm::max(wmax, wc);
                }
                *out_min = wmin;
                *out_max = wmax;
            };
        auto view = wc->registry.view<const cairns::SkinRef,
                                       const cairns::WorldTransform>();

        constexpr uint32_t kBucketCap = cairns::kMaxSkinnedMeshes;
        uint32_t* mesh_actor_count =
            s.arena.AllocateArray<uint32_t>(kBucketCap);
        std::memset(mesh_actor_count, 0,
                    sizeof(uint32_t) * kBucketCap);

        uint32_t total_actors = 0;
        uint32_t dropped_actors = 0;
        entt::entity* kept_entities =
            s.arena.AllocateArray<entt::entity>(kAnimActorsCap);
        for (auto e : view) {
            const cairns::SkinRef& sr = view.get<const cairns::SkinRef>(e);
            auto* sh = skins_.GetHot(sr.id);
            if (!sh) {
                continue;
            }
            if (sh->mesh.index >= kBucketCap) {
                continue;
            }
            const cairns::Mesh::Hot* mhot_c = meshes_.GetHot(sh->mesh);
            if (mhot_c) {
                const cairns::WorldTransform& wt =
                    view.get<const cairns::WorldTransform>(e);
                glm::vec3 wmn, wmx;
                actor_world_aabb(mhot_c, wt.world, &wmn, &wmx);
                if (aabb_outside(wmn, wmx)) {
                    continue;
                }
            }
            if (total_actors >= kAnimActorsCap) {
                ++dropped_actors;
                continue;
            }
            kept_entities[total_actors] = e;
            ++mesh_actor_count[sh->mesh.index];
            ++total_actors;
        }
        if (dropped_actors > 0) {
            // Loud every-frame report via CAIRNS_PRINT_ERR so Android
            // logcat surfaces it at ERROR level (the silent latch-once
            // hid that a large fraction of actors were dropping to bind
            // pose). Animated + dropped quoted so the shortfall is obvious.
            CAIRNS_PRINT_ERR(
                "[ANIM-CAP] BuildSkinFrame slot=%u: hit kAnimActorsCap=%u; "
                "animated=%u dropped=%u (raise cap or lower hero count)\n",
                slot, kAnimActorsCap, total_actors, dropped_actors);
            anim_actors_cap_warned_ = true;
        }
        if (total_actors == 0) {
            return;
        }

        uint32_t* bucket_remap =
            s.arena.AllocateArray<uint32_t>(kBucketCap);
        std::memset(bucket_remap, 0xFF,
                    sizeof(uint32_t) * kBucketCap);

        cairns::SkinBatchGpu* batches =
            s.arena.AllocateArray<cairns::SkinBatchGpu>(kBucketCap);
        uint32_t* bucket_inst_cursor =
            s.arena.AllocateArray<uint32_t>(kBucketCap);
        std::memset(bucket_inst_cursor, 0,
                    sizeof(uint32_t) * kBucketCap);

        uint32_t bucket_count = 0;
        uint32_t meta_running = 0;
        uint32_t palette_running = 0;
        for (uint32_t k = 0; k < total_actors; ++k) {
            const entt::entity e = kept_entities[k];
            const cairns::SkinRef& sr = view.get<const cairns::SkinRef>(e);
            auto* sh = skins_.GetHot(sr.id);
            if (!sh) {
                continue;
            }
            if (bucket_remap[sh->mesh.index] != UINT32_MAX) {
                continue;
            }
            const cairns::Mesh::Hot* mhot = meshes_.GetHot(sh->mesh);
            if (!mhot) {
                continue;
            }
            const uint32_t inst = mesh_actor_count[sh->mesh.index];
            const uint32_t joint_count = sh->joint_count;
            const uint32_t vert_count = mhot->vert_count;
            if (inst == 0 || joint_count == 0 || vert_count == 0) {
                continue;
            }
            bucket_remap[sh->mesh.index] = bucket_count;
            cairns::SkinBatchGpu& b = batches[bucket_count];
            b.mesh_set = rhi::Handle<rhi::BindGroup>{};
            b.mesh = sh->mesh;
            b.first_meta = meta_running;
            b.first_palette_mat4 = palette_running;
            b.instance_count = inst;
            b.joint_count = joint_count;
            b.vertex_count = vert_count;
            b.workgroups = (vert_count + 63u) / 64u;
            meta_running += inst;
            palette_running += inst * joint_count;
            ++bucket_count;
        }
        if (bucket_count == 0) {
            return;
        }

        glm::uvec2* instance_meta =
            s.arena.AllocateArray<glm::uvec2>(meta_running);
        cairns::GpuActorRecord* actor_records =
            s.arena.AllocateArray<cairns::GpuActorRecord>(meta_running);

        // #222 Phase 0.3: wide-base anim clock. (sim_frame_ * kFixedDt) +
        // scale/offset in double, fmod by double(clip duration), narrow to
        // float. The kernel's own wrap (anim_eval.comp.glsl) still runs on
        // the narrowed value as a no-op safety. float sim time loses
        // sub-frame precision after about 17 minutes; double holds it
        // beyond any plausible camera-app session.
        const double anim_t_d =
            static_cast<double>(sim_frame_) * cairns::kFixedDt;
        for (uint32_t k = 0; k < total_actors; ++k) {
            const entt::entity e = kept_entities[k];
            const cairns::SkinRef& sr = view.get<const cairns::SkinRef>(e);
            auto* sh = skins_.GetHot(sr.id);
            if (!sh) {
                continue;
            }
            if (sh->gpu_prefab_header_idx == UINT32_MAX) {
                continue;
            }
            const uint32_t bi = bucket_remap[sh->mesh.index];
            if (bi == UINT32_MAX) {
                continue;
            }
            cairns::SkinBatchGpu& b = batches[bi];
            const uint32_t cursor = bucket_inst_cursor[bi];
            if (cursor >= b.instance_count) {
                continue;
            }
            const uint32_t actor_idx = b.first_meta + cursor;
            const uint32_t palette_slot_base =
                b.first_palette_mat4 + cursor * b.joint_count;

            instance_meta[actor_idx] =
                glm::uvec2(cursor * b.joint_count, sh->slice_offset);

            // #222 Phase H.5: duration cached on Hot at skin-create; no
            // per-actor prefabs_.GetCold this frame.
            const double dur = static_cast<double>(sh->gpu_clip_duration);
            const double scaled =
                anim_t_d * static_cast<double>(sh->time_scale) +
                static_cast<double>(sh->time_offset);
            const double wrapped = scaled - dur * std::floor(scaled / dur);

            cairns::GpuActorRecord& rec = actor_records[actor_idx];
            rec.scene_idx = sh->gpu_prefab_header_idx;
            rec.world_scratch_base = actor_idx * kAnimMaxNodes;
            rec.palette_out_base = palette_slot_base;
            rec.time = static_cast<float>(wrapped);

            ++bucket_inst_cursor[bi];
        }

        s.pkt.instance_meta =
            std::span<const glm::uvec2>(instance_meta, meta_running);
        s.pkt.actor_records =
            std::span<const cairns::GpuActorRecord>(actor_records, meta_running);
        s.pkt.skin_batches =
            std::span<const cairns::SkinBatchGpu>(batches, bucket_count);
    }

    void initAnimEvalKernel() {
        const std::string shader_dir = cairns::GetBasePathSafe();
        rhi::ComputePipelineDesc desc{};
        desc.logical_shader = "anim_eval";
        desc.shader_dir = shader_dir.c_str();
        desc.debug_name = "anim_eval";
        desc.layout = rhi::ComputePipelineLayout::kAnimEval;
        anim_eval_kernel_ = rhi_.pipelines.CreateComputePipeline(
            rhi_.resources, rhi_.frames, desc);
        if (anim_eval_kernel_.IsNull()) {
            CAIRNS_PRINT("initAnimEvalKernel: load failed -- gpu palette eval disabled\n");
        }
    }

    // #221 Phase 5b: flatten every loaded scene's animation tables into
    // shared kDefault SSBOs and stamp per-scene base offsets into the
    // scene_headers SSBO. Called ONCE after all scenes are loaded. Bumps
    // anim_eval_tables_uploaded_ = true on success.
    // #228 H4b vk fix: (re)create dyn_anim_eval_ DynamicBuffers set using
    // the current anim buffer handles. Idempotent and safe to call before
    // anim_eval_tables_uploaded_ flips true (early-returns when buffers
    // don't exist yet). vk needs this set; metal ignores dyn_set_0 in
    // DispatchAnimEval.
    bool recreateAnimDynBindings() {
        if (!anim_eval_tables_uploaded_) {
            return true;
        }
        const rhi::Handle<rhi::Buffer> ae_ssbo[12] = {
            scene_headers_buf_, ae_parent_buf_, ae_topo_buf_,
            ae_bind_pose_buf_, ae_channels_buf_, ae_samplers_buf_,
            ae_times_buf_, ae_values_buf_, ae_joint_nodes_buf_,
            ae_inverse_binds_buf_, world_scratch_buf_, palette_out_buf_,
        };
        cairns::rhi::DynamicBinding ae_b[13]{};
        for (uint32_t i = 0; i < 13; ++i) {
            ae_b[i].stages = cairns::rhi::kStageCompute;
        }
        ae_b[0].slot = 0;
        ae_b[0].kind = cairns::rhi::BufferKind::kUniform;
        ae_b[0].max_range = 16384u;
        ae_b[0].has_dynamic_offset = true;
        for (uint32_t i = 0; i < 12; ++i) {
            ae_b[1 + i].slot = 1 + i;
            ae_b[1 + i].kind = cairns::rhi::BufferKind::kStorage;
            ae_b[1 + i].max_range = 0;  // VK_WHOLE_SIZE
            ae_b[1 + i].has_dynamic_offset = false;
            ae_b[1 + i].backing = ae_ssbo[i];
        }
        cairns::rhi::DynamicBuffersDesc ae_d{};
        ae_d.debug_name = "dyn_anim_eval";
        ae_d.bindings =
            std::span<const cairns::rhi::DynamicBinding>(ae_b, 13);
        // #228 F1 user: enqueue the old set for kFIF-frame fenced deletion
        // instead of WaitIdle+Destroy. The new set is created+used
        // immediately; the old one persists in-flight one more frame and
        // then gets Released when the slot's bucket drains. No GPU drain.
        if (!dyn_anim_eval_.IsNull()) {
            rhi_.resources.DeferFree(dyn_anim_eval_);
        }
        dyn_anim_eval_ =
            rhi_.resources.CreateDynamicBuffers(rhi_.alloc, rhi_.frames, ae_d);
        if (dyn_anim_eval_.IsNull()) {
            CAIRNS_PRINT("recreateAnimDynBindings: dyn_anim_eval create failed\n");
            return false;
        }
        return true;
    }

    // #224 L9 wedge-at-scale fix: dyn_skin_group_b_ binding 1 (palettes)
    // is gated on anim_eval_tables_uploaded_ at GreaterInit. L9's empty
    // boot leaves it false, so binding 1 falls back to the kDynamic master
    // (a per-frame 64 KB ring) instead of palette_out_buf_ (16 MB). The
    // skin compute then reads palette transforms out of the wrong buffer
    // -- at ~9 actors the dyn master happens to contain enough zeros to
    // pass, but at 100+ actors every joint read is garbage and the mesh
    // wedges into giant tendrils. Recreate the set after first upload so
    // binding 1 captures palette_out_buf_. Mirrors GreaterInit's gb[] tab.
    bool recreateSkinGroupB() {
        if (skin_kernel_.IsNull() || skin_output_pool_buffer_.IsNull()) {
            return true;
        }
        cairns::rhi::DynamicBinding gb[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            gb[i].stages = cairns::rhi::kStageCompute;
        }
        gb[0].slot = 0;
        gb[0].kind = cairns::rhi::BufferKind::kUniform;
        gb[0].max_range = 64u;
        gb[0].has_dynamic_offset = true;
        gb[1].slot = 1;
        gb[1].kind = cairns::rhi::BufferKind::kStorage;
        gb[1].max_range = 1u << 20;
        gb[1].has_dynamic_offset = true;
        if (anim_eval_tables_uploaded_) {
            gb[1].backing = palette_out_buf_;
        }
        gb[2].slot = 2;
        gb[2].kind = cairns::rhi::BufferKind::kStorage;
        gb[2].max_range = 16384u;
        gb[2].has_dynamic_offset = true;
        gb[3].slot = 3;
        gb[3].kind = cairns::rhi::BufferKind::kStorage;
        gb[3].max_range = 0;  // VK_WHOLE_SIZE
        gb[3].has_dynamic_offset = false;
        gb[3].backing = skin_output_pool_buffer_;
        cairns::rhi::DynamicBuffersDesc gd{};
        gd.debug_name = "dyn_skin_group_b";
        gd.bindings = std::span<const cairns::rhi::DynamicBinding>(gb, 4);
        if (!dyn_skin_group_b_.IsNull()) {
            rhi_.resources.DeferFree(dyn_skin_group_b_);
        }
        dyn_skin_group_b_ =
            rhi_.resources.CreateDynamicBuffers(rhi_.alloc, rhi_.frames, gd);
        if (dyn_skin_group_b_.IsNull()) {
            CAIRNS_PRINT("recreateSkinGroupB: dyn_skin_group_b create failed\n");
            return false;
        }
        return true;
    }

    void uploadAnimTablesGpu() {
        if (anim_eval_kernel_.IsNull()) {
            return;
        }
        const auto& prefab_ids = prefab_ids_;
        if (prefab_ids.empty()) {
            return;
        }
        // #228 H4b: Aaltonen delta path. Walk only the trailing window
        // [anim_uploaded_prefab_count_..end) and upload the new data at
        // an OFFSET into each buffer past what was uploaded previously.
        // The steady-state cost is O(batch). Buffer growth is rare (only
        // when a new batch's totals exceed the existing buffer capacity);
        // when it happens, fall back to a full rebuild that re-uploads
        // every prefab from scratch.
        bool full_rebuild = (anim_uploaded_prefab_count_ == 0) ||
                            (anim_uploaded_prefab_count_ > prefab_ids.size());
        // Storage that survives retry (filled once per attempt).
        std::vector<cairns::GpuSceneHeader> headers;
        std::vector<int32_t> parent_flat;
        std::vector<int32_t> topo_flat;
        std::vector<cairns::GpuTRS> bind_pose_flat;
        std::vector<cairns::GpuChannel> channels_flat;
        std::vector<cairns::GpuSampler> samplers_flat;
        std::vector<float> times_flat;
        std::vector<glm::vec4> values_flat;
        std::vector<int32_t> joint_nodes_flat;
        std::vector<glm::mat4> inverse_binds_flat;
        AnimCursors base{};
        AnimCursors target{};
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (full_rebuild) {
                anim_uploaded_prefab_count_ = 0;
                anim_cur_ = {};
            }
            base = anim_cur_;
            headers.clear();
            parent_flat.clear();
            topo_flat.clear();
            bind_pose_flat.clear();
            channels_flat.clear();
            samplers_flat.clear();
            times_flat.clear();
            values_flat.clear();
            joint_nodes_flat.clear();
            inverse_binds_flat.clear();
            const uint32_t start = anim_uploaded_prefab_count_;
            headers.reserve(prefab_ids.size() - start);
            for (uint32_t i = start; i < prefab_ids.size(); ++i) {
                cairns::PrefabId sid = prefab_ids[i];
                cairns::Prefab::Hot* hot = prefabs_.GetHot(sid);
                cairns::Prefab::Cold* cold = prefabs_.GetCold(sid);
                if (!hot || !cold) {
                    continue;
                }
                if (cold->skins.empty() || cold->clips.empty() ||
                    cold->nodes.empty()) {
                    hot->gpu_prefab_header_idx = UINT32_MAX;
                    continue;
                }
                cairns::GpuSceneHeader sh{};
                sh.node_count = static_cast<uint32_t>(cold->nodes.size());
                sh.joint_count =
                    static_cast<uint32_t>(cold->gpu_joint_nodes.size());
                sh.channel_count =
                    static_cast<uint32_t>(cold->gpu_channels.size());
                sh.sampler_count =
                    static_cast<uint32_t>(cold->gpu_samplers.size());
                // offsets absolute into the GPU buffer (base + delta-so-far).
                sh.parent_off = base.parent +
                                static_cast<uint32_t>(parent_flat.size());
                sh.topo_off = base.topo +
                              static_cast<uint32_t>(topo_flat.size());
                sh.bind_pose_off =
                    base.bind_pose +
                    static_cast<uint32_t>(bind_pose_flat.size());
                sh.channel_off = base.channels +
                                 static_cast<uint32_t>(channels_flat.size());
                sh.sampler_off = base.samplers +
                                 static_cast<uint32_t>(samplers_flat.size());
                sh.times_off = base.times +
                               static_cast<uint32_t>(times_flat.size());
                sh.values_off = base.values +
                                static_cast<uint32_t>(values_flat.size());
                sh.joint_nodes_off =
                    base.joint_nodes +
                    static_cast<uint32_t>(joint_nodes_flat.size());
                sh.inverse_binds_off =
                    base.inverse_binds +
                    static_cast<uint32_t>(inverse_binds_flat.size());
                sh.mesh_node = cold->gpu_mesh_node;
                sh.duration = cold->gpu_clip_duration;
                const uint32_t local_times_base = sh.times_off;
                const uint32_t local_values_base = sh.values_off;
                parent_flat.insert(parent_flat.end(),
                                    cold->gpu_parent.begin(),
                                    cold->gpu_parent.end());
                topo_flat.insert(topo_flat.end(), cold->gpu_topo.begin(),
                                  cold->gpu_topo.end());
                bind_pose_flat.insert(bind_pose_flat.end(),
                                        cold->gpu_bind_pose.begin(),
                                        cold->gpu_bind_pose.end());
                channels_flat.insert(channels_flat.end(),
                                      cold->gpu_channels.begin(),
                                      cold->gpu_channels.end());
                for (cairns::GpuSampler gs : cold->gpu_samplers) {
                    gs.times_off += local_times_base;
                    gs.values_off += local_values_base;
                    samplers_flat.push_back(gs);
                }
                times_flat.insert(times_flat.end(), cold->gpu_times.begin(),
                                   cold->gpu_times.end());
                values_flat.insert(values_flat.end(),
                                    cold->gpu_values.begin(),
                                    cold->gpu_values.end());
                joint_nodes_flat.insert(joint_nodes_flat.end(),
                                         cold->gpu_joint_nodes.begin(),
                                         cold->gpu_joint_nodes.end());
                inverse_binds_flat.insert(inverse_binds_flat.end(),
                                           cold->gpu_inverse_binds.begin(),
                                           cold->gpu_inverse_binds.end());
                hot->gpu_prefab_header_idx =
                    base.headers + static_cast<uint32_t>(headers.size());
                headers.push_back(sh);
            }
            if (headers.empty()) {
                // No new prefabs had anim data. Cursors unchanged; bump
                // uploaded count so we don't re-walk these on the next
                // call.
                anim_uploaded_prefab_count_ =
                    static_cast<uint32_t>(prefab_ids.size());
                return;
            }
            target.headers =
                base.headers + static_cast<uint32_t>(headers.size());
            target.parent =
                base.parent + static_cast<uint32_t>(parent_flat.size());
            target.topo =
                base.topo + static_cast<uint32_t>(topo_flat.size());
            target.bind_pose =
                base.bind_pose + static_cast<uint32_t>(bind_pose_flat.size());
            target.channels =
                base.channels + static_cast<uint32_t>(channels_flat.size());
            target.samplers =
                base.samplers + static_cast<uint32_t>(samplers_flat.size());
            target.times =
                base.times + static_cast<uint32_t>(times_flat.size());
            target.values =
                base.values + static_cast<uint32_t>(values_flat.size());
            target.joint_nodes =
                base.joint_nodes +
                static_cast<uint32_t>(joint_nodes_flat.size());
            target.inverse_binds =
                base.inverse_binds +
                static_cast<uint32_t>(inverse_binds_flat.size());
            // For delta attempts: does every buffer already fit the new
            // total? If not, retry as full rebuild (delta-only writes
            // can't span a destroyed-and-recreated buffer).
            auto fits = [&](const rhi::Handle<rhi::Buffer>& b,
                            uint32_t target_entries,
                            size_t entry_size) -> bool {
                if (b.IsNull()) {
                    return false;
                }
                const uint32_t have = rhi_.resources.GetBufferByteSize(b);
                return have >= target_entries * entry_size;
            };
            if (!full_rebuild && (
                    !fits(scene_headers_buf_, target.headers,
                          sizeof(cairns::GpuSceneHeader)) ||
                    !fits(ae_parent_buf_, target.parent, sizeof(int32_t)) ||
                    !fits(ae_topo_buf_, target.topo, sizeof(int32_t)) ||
                    !fits(ae_bind_pose_buf_, target.bind_pose,
                          sizeof(cairns::GpuTRS)) ||
                    !fits(ae_channels_buf_, target.channels,
                          sizeof(cairns::GpuChannel)) ||
                    !fits(ae_samplers_buf_, target.samplers,
                          sizeof(cairns::GpuSampler)) ||
                    !fits(ae_times_buf_, target.times, sizeof(float)) ||
                    !fits(ae_values_buf_, target.values, sizeof(glm::vec4)) ||
                    !fits(ae_joint_nodes_buf_, target.joint_nodes,
                          sizeof(int32_t)) ||
                    !fits(ae_inverse_binds_buf_, target.inverse_binds,
                          sizeof(glm::mat4)))) {
                full_rebuild = true;
                continue;
            }
            break;
        }
        // Upload helper: writes `bytes` of `data` into `out` at byte
        // offset `byte_off`. Allocates / grows `out` so it can hold at
        // least `total_bytes`. Recycles the existing handle when its
        // capacity is sufficient (H4a leak fix preserved).
        auto upload_at = [&](const void* data, size_t bytes,
                             size_t byte_off, size_t total_bytes,
                             rhi::Handle<rhi::Buffer>& out) -> bool {
            if (total_bytes == 0) {
                total_bytes = 16;
            }
            const uint32_t need = static_cast<uint32_t>(total_bytes);
            uint32_t have = 0;
            if (!out.IsNull()) {
                have = rhi_.resources.GetBufferByteSize(out);
            }
            if (out.IsNull() || have < need) {
                if (!out.IsNull()) {
                    rhi_.device.WaitIdle();
                    rhi_.resources.Destroy(rhi_.alloc, out);
                }
                // #228 H4b: growth pad so subsequent appends don't immediately
                // re-trigger growth. 4x current need (Aaltonen reserve-
                // and-grow); clamp small allocations up to 4 KB. Covers
                // typical hero-size variance so steady-state delta fires
                // for most appends.
                rhi::BufferDesc bd{};
                uint32_t alloc_size = need * 4;
                if (alloc_size < 4096) {
                    alloc_size = 4096;
                }
                bd.byte_size = alloc_size;
                bd.usage = rhi::kUsageStorage;
                bd.memory = rhi::Memory::kDefault;
                out = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
                if (out.IsNull()) {
                    return false;
                }
                // Handle changed -- vk's dyn_anim_eval_ descriptor set
                // captures buffer handles at creation time, so it now
                // points at a freed handle. Mark for recreation.
                anim_dyn_dirty_ = true;
            }
            if (data && bytes > 0) {
                rhi_.resources.UploadBuffer(
                    rhi_.alloc, out,
                    static_cast<uint32_t>(byte_off),
                    std::span<const uint8_t>(
                        static_cast<const uint8_t*>(data), bytes));
            }
            return true;
        };
        if (!upload_at(headers.data(),
                       headers.size() * sizeof(cairns::GpuSceneHeader),
                       base.headers * sizeof(cairns::GpuSceneHeader),
                       target.headers * sizeof(cairns::GpuSceneHeader),
                       scene_headers_buf_) ||
            !upload_at(parent_flat.data(),
                       parent_flat.size() * sizeof(int32_t),
                       base.parent * sizeof(int32_t),
                       target.parent * sizeof(int32_t),
                       ae_parent_buf_) ||
            !upload_at(topo_flat.data(),
                       topo_flat.size() * sizeof(int32_t),
                       base.topo * sizeof(int32_t),
                       target.topo * sizeof(int32_t),
                       ae_topo_buf_) ||
            !upload_at(bind_pose_flat.data(),
                       bind_pose_flat.size() * sizeof(cairns::GpuTRS),
                       base.bind_pose * sizeof(cairns::GpuTRS),
                       target.bind_pose * sizeof(cairns::GpuTRS),
                       ae_bind_pose_buf_) ||
            !upload_at(channels_flat.empty() ? nullptr : channels_flat.data(),
                       channels_flat.size() * sizeof(cairns::GpuChannel),
                       base.channels * sizeof(cairns::GpuChannel),
                       target.channels * sizeof(cairns::GpuChannel),
                       ae_channels_buf_) ||
            !upload_at(samplers_flat.empty() ? nullptr : samplers_flat.data(),
                       samplers_flat.size() * sizeof(cairns::GpuSampler),
                       base.samplers * sizeof(cairns::GpuSampler),
                       target.samplers * sizeof(cairns::GpuSampler),
                       ae_samplers_buf_) ||
            !upload_at(times_flat.empty() ? nullptr : times_flat.data(),
                       times_flat.size() * sizeof(float),
                       base.times * sizeof(float),
                       target.times * sizeof(float),
                       ae_times_buf_) ||
            !upload_at(values_flat.empty() ? nullptr : values_flat.data(),
                       values_flat.size() * sizeof(glm::vec4),
                       base.values * sizeof(glm::vec4),
                       target.values * sizeof(glm::vec4),
                       ae_values_buf_) ||
            !upload_at(joint_nodes_flat.data(),
                       joint_nodes_flat.size() * sizeof(int32_t),
                       base.joint_nodes * sizeof(int32_t),
                       target.joint_nodes * sizeof(int32_t),
                       ae_joint_nodes_buf_) ||
            !upload_at(inverse_binds_flat.data(),
                       inverse_binds_flat.size() * sizeof(glm::mat4),
                       base.inverse_binds * sizeof(glm::mat4),
                       target.inverse_binds * sizeof(glm::mat4),
                       ae_inverse_binds_buf_)) {
            return;
        }
        anim_cur_ = target;
        anim_uploaded_prefab_count_ =
            static_cast<uint32_t>(prefab_ids.size());
        anim_eval_tables_uploaded_ = true;
        // #228 H4b vk fix: if any buffer was destroyed-and-recreated this
        // call, the dyn_anim_eval_ descriptor set holds stale handles --
        // recreate it. Also recreates on first-ever upload because the
        // GreaterInit gate left it Null post-L9.
        if (anim_dyn_dirty_) {
            recreateAnimDynBindings();
            recreateSkinGroupB();
            anim_dyn_dirty_ = false;
        }
        CAIRNS_PRINT("uploadAnimTablesGpu: %s | +%zu scenes -> %u total | "
                     "parent +%zu/%u topo +%zu/%u bind_pose +%zu/%u "
                     "channels +%zu/%u samplers +%zu/%u "
                     "times +%zu/%u values +%zu/%u "
                     "joint_nodes +%zu/%u inverse_binds +%zu/%u\n",
                     full_rebuild ? "FULL" : "DELTA",
                     headers.size(), target.headers,
                     parent_flat.size(), target.parent,
                     topo_flat.size(), target.topo,
                     bind_pose_flat.size(), target.bind_pose,
                     channels_flat.size(), target.channels,
                     samplers_flat.size(), target.samplers,
                     times_flat.size(), target.times,
                     values_flat.size(), target.values,
                     joint_nodes_flat.size(), target.joint_nodes,
                     inverse_binds_flat.size(), target.inverse_binds);
    }

    // #221 Phase 4: best-effort load of skin compute kernel. Failing the
    // load (missing skin.comp.spv / skin.metal) leaves skin_kernel_ Null;
    // Phase 5's dispatch checks IsNull() and degenerates to "no skinning
    // this frame", preserving the static path bit-for-bit.
    void initSkinKernel() {
        const std::string shader_dir = cairns::GetBasePathSafe();
        rhi::ComputePipelineDesc desc{};
        desc.logical_shader = "skin";
        desc.shader_dir = shader_dir.c_str();
        desc.debug_name = "skin_compute";
        desc.layout = rhi::ComputePipelineLayout::kSkin;
        skin_kernel_ = rhi_.pipelines.CreateComputePipeline(rhi_.resources,
                                                              rhi_.frames, desc);
        if (skin_kernel_.IsNull()) {
            CAIRNS_PRINT("initSkinKernel: skin kernel load failed (skin.comp.spv / skin.metal missing?) -- skinning disabled\n");
        }
    }

    bool initParticles() {
        const std::string shader_dir = cairns::GetBasePathSafe();
        // #222 Phase D.4: SSBOs created first so the parity DynamicBuffers
        // can write their bindings 1+2 at create time. Particle kernel
        // then references the parity layout (set 0) instead of the
        // legacy frames.plat.compute_layout_.
        if (!initParticleSsbos()) {
            return false;
        }
        {  // #222 Phase D.4: 2 parity DynamicBuffers, one per (src,dst) order.
           // Binding 0 = UBO_DYN dt over kDynamic master (per-dispatch dyn off).
           // Bindings 1+2 = SSBO over particle_ssbo_[A]/[B] no-dyn.
            for (uint32_t p = 0; p < 2; ++p) {
                cairns::rhi::DynamicBinding pb[3]{};
                for (uint32_t i = 0; i < 3; ++i) {
                    pb[i].stages = cairns::rhi::kStageCompute;
                }
                pb[0].slot = 0;
                pb[0].kind = cairns::rhi::BufferKind::kUniform;
                pb[0].max_range = sizeof(float);
                pb[0].has_dynamic_offset = true;
                pb[1].slot = 1;
                pb[1].kind = cairns::rhi::BufferKind::kStorage;
                pb[1].max_range = 0;  // VK_WHOLE_SIZE
                pb[1].has_dynamic_offset = false;
                pb[1].backing = particle_ssbo_[p];          // src
                pb[2].slot = 2;
                pb[2].kind = cairns::rhi::BufferKind::kStorage;
                pb[2].max_range = 0;
                pb[2].has_dynamic_offset = false;
                pb[2].backing = particle_ssbo_[p ^ 1];      // dst
                cairns::rhi::DynamicBuffersDesc pd{};
                pd.debug_name = (p == 0) ? "dyn_particle_parity_0"
                                          : "dyn_particle_parity_1";
                pd.bindings =
                    std::span<const cairns::rhi::DynamicBinding>(pb, 3);
                dyn_particle_parity_[p] =
                    rhi_.resources.CreateDynamicBuffers(rhi_.alloc,
                                                          rhi_.frames, pd);
                if (dyn_particle_parity_[p].IsNull()) {
                    CAIRNS_PRINT("initParticles: dyn_particle_parity create failed\n");
                    return false;
                }
            }
        }
        {  // particle compute kernel via rhi
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "particle_compute";
            desc.layout = rhi::ComputePipelineLayout::kParticle;
            // #222 Phase D.4: pipeline layout reads from parity[0]'s
            // DynamicBuffers Hot layout (UBO_DYN @0 + 2 SSBO). Both
            // parity sets share the same layout shape.
            desc.dyn_set_0 = dyn_particle_parity_[0];
            particle_kernel_ = rhi_.pipelines.CreateComputePipeline(rhi_.resources, rhi_.frames, desc);
            if (particle_kernel_.IsNull()) {
                return false;
            }
        }
        {  // particle render pipeline via rhi
            const rhi::VertexInputAttribute attrs[2] = {
                {0, 0, rhi::Format::kRg32F,
                 static_cast<uint32_t>(offsetof(Particle, position))},
                {1, 0, rhi::Format::kRgba32F,
                 static_cast<uint32_t>(offsetof(Particle, color))},
            };
            const rhi::VertexBufferLayout layout{
                0, static_cast<uint32_t>(sizeof(Particle))};
            rhi::GraphicsPipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(attrs, 2);
            desc.vertex_buffers =
                std::span<const rhi::VertexBufferLayout>(&layout, 1);
            desc.topology = rhi::PrimitiveTopology::kPointList;
            desc.cull = rhi::CullMode::kBack;
            desc.front_face = rhi::FrontFace::kCounterClockwise;
            desc.depth_test = true;
            desc.depth_write = true;
            desc.depth_compare = rhi::CompareOp::kLess;
            desc.blend.enable = true;
            desc.blend.src_color = rhi::BlendFactor::kSrcAlpha;
            desc.blend.dst_color = rhi::BlendFactor::kOneMinusSrcAlpha;
            desc.blend.src_alpha = rhi::BlendFactor::kOne;
            desc.blend.dst_alpha = rhi::BlendFactor::kZero;
            desc.color_format = rhi::Format::kBgra8Unorm;
            desc.depth_format = rhi::Format::kD32F;
            desc.sample_count = sampleCount;
            desc.push_constant_bytes = 0;
            desc.debug_name = "particle_render";
            desc.swap_chain = &swapchain_;
            particle_render_shader_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, desc);
            if (particle_render_shader_.IsNull()) {
                return false;
            }
            // Offscreen variant for the render-graph forward pass. #206 the
            // forward pass is MRT (BGRA color + R32U id); pipeline declares
            // both attachments so the offscreen-target-cache renderpass
            // matches the pipeline's compat renderpass.
            rhi::GraphicsPipelineDesc opd = desc;
            opd.sample_count = 1;
            opd.swap_chain = nullptr;
            opd.color_formats[0] = rhi::Format::kBgra8Unorm;
            opd.color_formats[1] = rhi::Format::kR32Uint;
            opd.color_count = 2;
            // #242: particle frag writes only outColor (location 0); the
            // R32U id attachment exists for renderpass-compat but gets
            // colorWriteMask=0 so we don't undef-stomp it.
            opd.frag_color_output_count = 1;
            opd.debug_name = "particle_render_offscreen";
            particle_render_offscreen_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, opd);
            if (particle_render_offscreen_.IsNull()) {
                return false;
            }
            // #222 Phase A.1 fix: id-less variant for the no-id forward
            // pass. Same shaders; the frag's location-1 write to the id
            // attachment becomes a no-op write to a discarded location.
            rhi::GraphicsPipelineDesc npd = opd;
            npd.color_formats[1] = rhi::Format::kBgra8Unorm;  // unused
            npd.color_count = 1;
            npd.debug_name = "particle_render_offscreen_noid";
            particle_render_offscreen_noid_ =
                rhi_.pipelines.CreateGraphicsPipeline(
                    rhi_.resources, rhi_.frames, npd);
            if (particle_render_offscreen_noid_.IsNull()) {
                return false;
            }
        }

        {
            // imgui pipeline (swapchain MSAA, alpha blend, no depth).
            const std::string shader_dir = cairns::GetBasePathSafe();
            const rhi::VertexInputAttribute ia[3] = {
                {0, 0, rhi::Format::kRg32F, offsetof(ImDrawVert, pos)},
                {1, 0, rhi::Format::kRg32F, offsetof(ImDrawVert, uv)},
                {2, 0, rhi::Format::kRgba8Unorm, offsetof(ImDrawVert, col)},
            };
            const rhi::VertexBufferLayout il{
                0, static_cast<uint32_t>(sizeof(ImDrawVert))};
            rhi::GraphicsPipelineDesc id{};
            id.logical_shader = "imgui";
            id.shader_dir = shader_dir.c_str();
            id.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(ia, 3);
            id.vertex_buffers = std::span<const rhi::VertexBufferLayout>(&il, 1);
            id.topology = rhi::PrimitiveTopology::kTriangleList;
            id.cull = rhi::CullMode::kNone;
            id.depth_test = false;
            id.depth_write = false;
            id.blend.enable = true;
            id.blend.src_color = rhi::BlendFactor::kSrcAlpha;
            id.blend.dst_color = rhi::BlendFactor::kOneMinusSrcAlpha;
            id.blend.src_alpha = rhi::BlendFactor::kOne;
            id.blend.dst_alpha = rhi::BlendFactor::kOneMinusSrcAlpha;
            const bool surfaceless_imgui = !final_target_.IsNull();
            id.color_format = rhi::Format::kBgra8Unorm;
            id.depth_format = surfaceless_imgui ? rhi::Format::kUndefined
                                                : rhi::Format::kD32F;
            id.sample_count = surfaceless_imgui ? 1u : sampleCount;
            id.push_constant_bytes = 16;
            id.debug_name = "imgui";
            id.swap_chain = surfaceless_imgui ? nullptr : &swapchain_;
            imgui_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources,
                                                          rhi_.frames, id);
            if (imgui_.IsNull()) {
                return false;
            }

            // imgui font atlas -> Texture. Scale font + style by surface
            // width so HiDPI / mobile displays don't render a postage-stamp
            // overlay. 1280px is the desktop reference width.
            ImGuiIO& io = ImGui::GetIO();
            const float kRefWidth = 900.0f;
            const float raw_scale =
                static_cast<float>(FrameWidth()) / kRefWidth;
#if CAIRNS_ANDROID
            const float kScaleMax = 2.5f;
#elif CAIRNS_APPLE && TARGET_OS_IPHONE
            const float kScaleMax = 1.0f;
#else
            const float kScaleMax = 1.5f;
#endif
            const float dpi_scale = std::clamp(raw_scale, 1.0f, kScaleMax);
            ImFontConfig fc;
            fc.SizePixels = 13.0f * dpi_scale;
            io.Fonts->Clear();
            io.Fonts->AddFontDefault(&fc);
            ImGui::GetStyle().ScaleAllSizes(dpi_scale);
            unsigned char* pixels = nullptr;
            int fw = 0;
            int fh = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
            rhi::TextureDesc ftd{};
            ftd.dimensions = {fw, fh, 1};
            ftd.format = rhi::Format::kRgba8Unorm;
            ftd.mip_levels = 1;
            ftd.array_layers = 1;
            ftd.usage = rhi::kTexUsageSampled | rhi::kTexUsageTransferDst;
            ftd.memory = rhi::Memory::kDefault;
            ftd.initial_data = std::span<const uint8_t>(
                pixels, static_cast<size_t>(fw) * fh * 4);
            imgui_font_ = rhi_.resources.CreateTexture(rhi_.alloc, ftd);
            rhi::SamplerDesc sd{};
            sd.min_filter = rhi::Filter::kLinear;
            sd.mag_filter = rhi::Filter::kLinear;
            sd.mip_filter = rhi::Filter::kLinear;
            sd.address_mode = rhi::AddressMode::kClampToEdge;
            imgui_sampler_ = rhi_.resources.CreateSampler(sd);
        }

        return !particle_ssbo_[0].IsNull() && !particle_ssbo_[1].IsNull();
    }

    // #222 Phase D.4: particle SSBOs split out of initParticles so the
    // parity DynamicBuffers can grab their handles before the kernel
    // pipeline is built (pipeline layout is sourced from parity[0]).
    bool initParticleSsbos() {
        // A.1: portable mt19937 + hand-rolled [0,1) mapping. std::rand is
        // implementation-defined (libc++ vs NDK vs glibc all differ), and
        // its consumption order depended on heap layout, which is what was
        // flaking macOS triangle hashes. ParticleRng (cairns::ParticleRng,
        // src/render/particle_emitter.hpp) is the spec-test path -- same
        // engine, same (NextU32()>>8) * (1/16777216) mapping. Byte-stable
        // across runs and across libc++ flavors.
        // Override via Engine::SetRandomSeed before GreaterInit if you need
        // a different starting state (e.g. the rng.seed NDJSON op).
        cairns::ParticleRng rng(random_seed_);
        std::vector<Particle> particles(kParticleCount);
        for (uint32_t i = 0; i < kParticleCount; ++i) {
            const float r = rng.NextUnit();
            const float theta = r * 2.0f * static_cast<float>(std::numbers::pi);
            const float radius = rng.NextUnit();
            particles[i].position[0] = radius * std::cos(theta);
            particles[i].position[1] = radius * std::sin(theta);
            const float vx = (rng.NextUnit() - 0.5f) * 0.5f;
            const float vy = (rng.NextUnit() - 0.5f) * 0.5f;
            particles[i].velocity[0] = vx;
            particles[i].velocity[1] = vy;
            const float t = static_cast<float>(i) / static_cast<float>(kParticleCount);
            particles[i].color[0] = t;
            particles[i].color[1] = 1.0f - t;
            particles[i].color[2] = 0.5f;
            particles[i].color[3] = 1.0f;
        }

        const std::span<const uint8_t> init_data(
            reinterpret_cast<const uint8_t*>(particles.data()),
            particles.size() * sizeof(Particle));
        rhi::BufferDesc bd;
        bd.byte_size = static_cast<uint32_t>(particles.size() * sizeof(Particle));
        bd.usage = rhi::kUsageStorage | rhi::kUsageVertex;
        bd.memory = rhi::Memory::kDefault;
        bd.initial_data = init_data;
        particle_ssbo_[0] = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
        bd.initial_data = init_data;
        particle_ssbo_[1] = rhi_.resources.CreateBuffer(rhi_.alloc, bd);
        return !particle_ssbo_[0].IsNull() && !particle_ssbo_[1].IsNull();
    }

    bool deinit() {
        if (render_thread_) {
            render_thread_->Shutdown();
            render_thread_.reset();
        }
        swapchain_.Deinit();
        rhi_.pipelines.Deinit(rhi_.resources);
        rhi_.frames.Deinit();
        // #222 Phase F.1/F.3: sibling subsystem teardown after frames.
        rhi_.gpu_profiler.Deinit();
        rhi_.offscreen_targets.Deinit();
        rhi_.resources.Deinit();
        rhi_.alloc.Deinit();
        rhi_.device.Deinit();
        return true;
    }
    
private:
    uint32_t frame_ = 0;

    // #220 Step 3: prefabs_ is now a generational pool. prefab_ids_ is the
    // order-stable parallel list of SceneIds; consumers that want
    // index-by-position semantics (LoadPrefabsGpu's span, entity
    // assignment's `i % prefab_ids_.size()`) iterate this. The Prefab::Hot
    // / Prefab::Cold records live in the pool, not the vector.
    cairns::ResourceManager<cairns::Prefab> prefabs_;
    std::vector<cairns::PrefabId> prefab_ids_;
    // #222 Phase #267: parallel to prefab_ids_; index N maps to the GLB
    // path that produced prefab_ids_[N]. Read by the [PICK] log line.
    std::vector<std::filesystem::path> glb_paths_;
    // #269: parallel to prefab_ids_; AssetId registered for each scene.
    // InstantiatePrefab consumes this to stamp AssetRef on the new entity.
    std::vector<cairns::AssetId> per_prefab_asset_;
    // #222 Phase H.6: built once at scene-load + uploadAnimTablesGpu;
    // every frame's resident_textures span points at this vector
    // instead of being arena-allocated + filled per frame.
    std::vector<rhi::Handle<rhi::Texture>> resident_textures_;
    // #222 Phase H.4 partial: the skin-attr SSBO is the SAME handle on every
    // skinned mesh from one LoadPrefabsGpu call -- it does not belong on
    // Mesh::Hot.
    // #224 L1: was a single handle; now a per-batch list. Each
    // LoadPrefabBatch call pushes ONE handle (the batch's shared skin
    // attrs buffer) and stamps the batch index on every Mesh::Hot it
    // creates. The H.4 dedup still holds *within* a batch (every
    // skinned mesh in the batch reads the same handle) -- only across
    // batches do they differ. Per-mesh resolution:
    //   ResolvedSharedSkin(mhot) == per_batch_shared_skin_[mhot.batch_id]
    // E (allocator-survey demo): per_batch_shared_skin_ wears the
    // print_allocator so CAIRNS_ALLOC_TRACE=1 builds emit a tagged
    // [ALLOC] line on every grow. Lifecycle: per-batch (push on every
    // LoadPrefabBatch with a skinned mesh; cleared on UnloadAllPrefabs).
    struct kTagPerBatchSharedSkin {
        static constexpr const char* name() {
            return "Engine::per_batch_shared_skin_";
        }
    };
    std::vector<rhi::Handle<rhi::Buffer>,
                cairns::print_allocator<rhi::Handle<rhi::Buffer>,
                                          kTagPerBatchSharedSkin>>
        per_batch_shared_skin_;
    // #224 L3: the instrument. Populated each LoadPrefabBatch call.
    // #224 L8: editor-chrome toggle (selection outline pass gate).
    // Default on; remixer/canvas flips to false to suppress meta-UI
    // before captures. Stylized highlight (materials) untouched.
    bool editor_chrome_enabled_ = true;
    cairns::LoadTrace        last_load_trace_{};
    cairns::LoaderCounters   loader_counters_{};
    cairns::ValidationReport last_validation_report_{};
    // #224 L6: APPEND-only debug snapshot stashed between two NDJSON
    // op calls (cairns.debug.snapshotPrefabHandles ->
    // cairns.debug.assertAppendOnly). Empty until first snapshot call.
    std::vector<PrefabHandleSnapshot> last_handle_snapshot_;
    std::vector<int32_t> root_nodes_stack_cache_;

    // #220 Step 1: handle-pilled pool. Bind group lives on Hot;
    // texture+sampler on Cold. Stale-slot reads fail at GetHot/GetCold
    // instead of silently aliasing a recycled bind group, which the
    // prior parallel material_bind_groups_ vector could not detect.
    cairns::ResourceManager<cairns::Material> materials_;
    // material_bind_groups_ DELETED -- set2 now lives in Hot.

    // #220 Step 2: handle-pilled mesh pool. Was nested inside each
    // Scene as std::vector<Mesh>; lifted out so multiple scenes can
    // reference the same loaded GLB through AssetRegistry. Scene now
    // holds std::vector<MeshId>.
    cairns::ResourceManager<cairns::Mesh> meshes_;

    // #221 Skinning Phase 3: handle-pilled SkinnedAttachment pool +
    // persistent GPU output pool. skin_output_pool_ is a RangePool over
    // skin_output_pool_buffer_ (256 MB private heap) measured in vec4
    // vertex units (Alloc(n) -> slice for n verts; bind offset =
    // slice.offset * sizeof(vec4)). skins_ wraps generation counters
    // around the per-actor SkinnedAttachment::Hot/Cold records.
    cairns::ResourceManager<cairns::SkinnedAttachment> skins_;
    rhi::Handle<rhi::Buffer> skin_output_pool_buffer_;
    cairns::RangePool skin_output_pool_;

    // #221 Phase 5b: GPU palette evaluation buffers + kernel. Scene-table
    // SSBOs are shared across all scenes with per-scene base offsets stored
    // in SceneHeader entries. World scratch + palette output are
    // actors_cap-sized (1024 * 256 mat4 = 16 MB each).
    rhi::Handle<rhi::Kernel> anim_eval_kernel_;
    rhi::Handle<rhi::Buffer> scene_headers_buf_;
    rhi::Handle<rhi::Buffer> ae_parent_buf_;
    rhi::Handle<rhi::Buffer> ae_topo_buf_;
    rhi::Handle<rhi::Buffer> ae_bind_pose_buf_;
    rhi::Handle<rhi::Buffer> ae_channels_buf_;
    rhi::Handle<rhi::Buffer> ae_samplers_buf_;
    rhi::Handle<rhi::Buffer> ae_times_buf_;
    rhi::Handle<rhi::Buffer> ae_values_buf_;
    rhi::Handle<rhi::Buffer> ae_joint_nodes_buf_;
    rhi::Handle<rhi::Buffer> ae_inverse_binds_buf_;
    rhi::Handle<rhi::Buffer> world_scratch_buf_;
    rhi::Handle<rhi::Buffer> palette_out_buf_;
    bool anim_eval_tables_uploaded_ = false;
    // #228 H4b: anim-table cursor state for the Aaltonen delta path.
    // count == number of entries currently uploaded into each buffer.
    // uploadAnimTablesGpu compares prefab_ids_.size() against
    // anim_uploaded_prefab_count_ and uploads ONLY the trailing delta
    // (new prefabs) when each buffer has spare capacity. Buffer growth
    // forces a full rebuild for ALL buffers in one retry pass.
    struct AnimCursors {
        uint32_t headers = 0;
        uint32_t parent = 0;
        uint32_t topo = 0;
        uint32_t bind_pose = 0;
        uint32_t channels = 0;
        uint32_t samplers = 0;
        uint32_t times = 0;
        uint32_t values = 0;
        uint32_t joint_nodes = 0;
        uint32_t inverse_binds = 0;
    };
    AnimCursors anim_cur_{};
    uint32_t anim_uploaded_prefab_count_ = 0;
    // #228 H4b vk fix: when uploadAnimTablesGpu Destroys+CreateBuffer's
    // any anim buffer (first allocation or growth), the existing
    // dyn_anim_eval_ descriptor set still references the freed handle.
    // anim_dyn_dirty_ tells the next uploadAnimTablesGpu tail to
    // recreate the descriptor set with the current handles. Also true
    // before the first upload so we create the set on first use (post-L9
    // GreaterInit no longer creates it because anim_eval_tables_uploaded_
    // is false at boot).
    bool anim_dyn_dirty_ = true;
    // #222 Phase 0.2: latched once-per-process warning when skinned-actor
    // count exceeded kAnimActorsCap in any frame.
    bool anim_actors_cap_warned_ = false;

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

    // EnTT scene-layer path. scenes_ pre-reserved at startup
    // (kMaxScenes Acquire+Release cycle) to keep Scene::Cold* pointer
    // stable across real Acquire later.
    static constexpr uint32_t kMaxScenes = 8;
    cairns::AssetRegistry assets_;
    cairns::ResourceManager<cairns::Scene> scenes_;
    std::vector<cairns::RenderProxyArrays> scene_proxies_;
    cairns::SceneId active_scene_;
    cairns::SceneId secondary_scene_;  // P6 multi-scene coexistence test
    cairns::SceneId primary_scene_;    // index-0 scene; stable as active_ moves

    // #220 Step 4: handle-pilled Viewport pool. viewports_ owns Hot+Cold;
    // viewport_ids_[0..active_viewport_count_) carry the slot ordering
    // (preserves the [0..N) layout/indexing semantics the rest of the
    // engine uses to address PerSlot::pending_globals[], id_target_[],
    // etc.). FlyController moved into Viewport::Cold (was fly_ parallel
    // array; reason "fly is parallel so Viewport struct can grow" is moot
    // once Viewport is in a generational pool).
    //
    // active_viewport_ is now the ViewportId of the focused viewport.
    // active_viewport_index_ caches its position in viewport_ids_ so the
    // PerSlot per-viewport arrays can still be indexed by int. Both fields
    // are updated together via setActiveViewport().
    //
    // #194: compile-time cap on simultaneous viewports. active_viewport_count_
    // (runtime) tells the engine how many slots are LIVE. Default = 1 (full-
    // frame viewport 0). Grow via cairns.viewport.open / shrink via
    // cairns.viewport.close. Layout rects on Viewport::Hot::layout_rect
    // (NDC 0..1 over the swap pane) describe where each live viewport tiles.
    static constexpr int kNumViewports = 4;
    cairns::ResourceManager<cairns::Viewport> viewports_;
    std::array<cairns::ViewportId, kNumViewports> viewport_ids_{};
    cairns::ViewportId active_viewport_;
    int active_viewport_index_ = 0;
    int active_viewport_count_ = 1;  // #194 runtime-live count, default 1
    bool cam_pose_override_ = false;

    // #220 Step 4 vpN wire-name layer. Engine-assigned monotonic counter
    // ("vp0", "vp1", ...); names never reused for the lifetime of the
    // engine process. Sorted-vector mapping (small N, binary search). The
    // RPC layer (scene_ops.cpp) is the only consumer; everywhere internal
    // uses ViewportId.
    struct ViewportName {
        uint32_t counter = 0;
        cairns::ViewportId id;
    };
    std::array<ViewportName, kNumViewports> viewport_names_{};
    uint8_t viewport_names_count_ = 0;
    uint32_t next_viewport_name_ = 0;

    // #210 per-slot CPU arena capacity. #221 Phase 3 raise to 16 MiB to
    // cover the per-frame palette/InstanceMeta/SkinMeshBatch arrays the
    // skin pipeline parks here (Phase 5 will print HighWater() and we'll
    // resize from data). Stored on PerSlot (the slot IS the lock).
    // Initialized in initCpuAllocators, Reset()'d at slot Acquire (render
    // thread already drained).
    static constexpr size_t kArenaBytesPerSlot = 16u * 1024u * 1024u;

    rhi::Rhi rhi_;
    // #228 H3: mesh_master_handle_ deleted. Was set in GreaterInit to
    // prefab_ids_[0]'s first mesh's posHandle, copied into
    // MeshDrawList::resident_buffers, never read by the recorder.
    // Per-frame render graph. Reused via Reset() across frames (vector storage
    // for passes/textures is preserved). Constructed lazily on first RecordFrame
    // because Resources& / Allocator& must already be initialized.
    std::unique_ptr<rhi::RenderGraph> graph_;

    cairns::rhi::SwapChain swapchain_;
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
    // through DynamicBuffers. Created post-scene-load when palette_out_buf_
    // + ae_*_buf_ + skin_output_pool_buffer_ exist. Backing buffer is set
    // per-binding (palette_out_buf_ for palettes; skin_output_pool for output
    // pool; ae_*_buf_ for scene tables). kDynamic-master fallback covers
    // the per-dispatch dynamic-offset bindings (params/inst_meta/records).
    rhi::Handle<rhi::DynamicBuffers> dyn_skin_group_b_;
    rhi::Handle<rhi::DynamicBuffers> dyn_anim_eval_;
    // #222 Phase D.4: particle parity DynamicBuffers. Index 0 binds
    // particle_ssbo_[0] -> binding 1 and particle_ssbo_[1] -> binding 2
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
    // particles
    static constexpr uint32_t kParticleCount = 512;
    rhi::Handle<rhi::Kernel> particle_kernel_;
    // #221 Phase 4: skin pre-skin compute kernel. Best-effort load -- if
    // shader assets aren't present (skin.comp.spv / skin.metal) the handle
    // stays Null and Phase 5's BuildSkinFrame / DispatchSkinBatches skip.
    rhi::Handle<rhi::Kernel> skin_kernel_;
    rhi::Handle<rhi::Shader> particle_render_shader_;
    rhi::Handle<rhi::Shader> particle_render_offscreen_;
    // #222 Phase A.1 fix: id-less variant for the no-id forward pass.
    rhi::Handle<rhi::Shader> particle_render_offscreen_noid_;
    rhi::Handle<rhi::Buffer> particle_ssbo_[2];
    std::unique_ptr<cairns::RenderThread> render_thread_;
    // Render thread writes back particle_parity_out under parity_m_ so the
    // game thread can chain frame N+1's parity_in from frame N's parity_out
    // without sharing a mutable counter. In steady state Acquire(slot N+1)
    // already happens-after Release of slot N, so the cv wait is a no-op.
    std::mutex parity_m_;
    std::condition_variable parity_cv_;
    uint32_t latest_parity_out_ = 0;
    uint64_t latest_parity_frame_ = 0;

    // Present handoff. Render thread runs Frames::EndSubmit and stores
    // (fc, target) on the slot under present_m_; main thread waits on the
    // SAME slot it just submitted to (slot index from frame_), reads the
    // stored values, and runs Frames::Present. One frame of present lag,
    // not kFramesInFlight. Driver vendors don't test render-thread present
    // (MoltenVK reaches into CALayer) so the contract is the same on every
    // backend.
    std::mutex present_m_;
    std::condition_variable present_cv_;
    [[maybe_unused]] int32_t prev_present_slot_ = -1;
    std::deque<int32_t> present_queue_;
    bool dump_emitted_ = false;
    uint32_t dump_emit_frame_ = 0;

    // Headless / surfaceless mode (cairns_serve): swap pass writes into this
    // offscreen target instead of the swapchain drawable. Allocated when
    // cfg.surfaceless == true in GreaterInit; null otherwise. P1C uses a
    // minimal clear-only render path; P2+ wires the full scene path through it.
    rhi::Handle<rhi::Texture> final_target_ = rhi::Handle<rhi::Texture>::Null;
    uint32_t final_target_w_ = 0;
    uint32_t final_target_h_ = 0;

    // #207 persistent per-viewport R32U id targets. Replaces the previous
    // transient id_off graph texture so end-of-frame pick readback (a
    // vkCmdCopyImageToBuffer / metal blit on a 1x1 region) can read the
    // last-rendered id without racing the transient pool's reuse. Sized
    // (vp_w, vp_h); reallocated lazily by EnsureIdTargets when dims drift.
    std::array<rhi::Handle<rhi::Texture>, kNumViewports> id_target_{};
    uint32_t id_target_w_ = 0;
    uint32_t id_target_h_ = 0;

    // #207 highlights texture: R32U 65x1 packed as [count, id0, id1, ...].
    // Sampled by outline.frag to filter the edge-detect to the current
    // highlight set. Recreated on highlights_rev_ change (rare -- clicks).
    rhi::Handle<rhi::Texture> highlights_tex_ = rhi::Handle<rhi::Texture>::Null;
    uint32_t highlights_tex_rev_ = 0;
    static constexpr uint32_t kMaxHighlights = 64;

    // P4 selection / highlight / pick. Selection + highlight are
    // document-side state; rev counters let the protocol's
    // cairns.selection.changed event know when to emit. Pick state holds
    // the most recent unresolved (viewport, x, y) click intent until the
    // GPU ID buffer + readback path lands.
    std::vector<cairns::SelectionTarget> selection_;
    std::vector<cairns::SelectionTarget> highlights_;
    uint32_t selection_rev_ = 0;
    uint32_t highlights_rev_ = 0;
    bool pick_pending_ = false;
    int pick_viewport_ = 0;
    uint32_t pick_x_ = 0;
    uint32_t pick_y_ = 0;
    bool pick_resolved_ = false;
    PickResult last_pick_result_{};

    // P3 resize lifecycle. SDL fires WINDOW_PIXEL_SIZE_CHANGED on the event
    // thread; we record intent + dims and settle on the next draw() call so
    // GPU teardown happens with no in-flight frames. last_seen_swap_w_/h_
    // also catches drift from Vulkan WSI auto-recreating the swapchain on
    // OUT_OF_DATE without our resize intent path firing.
    bool resize_pending_ = false;
    uint32_t resize_pending_w_ = 0;
    uint32_t resize_pending_h_ = 0;
    uint32_t last_seen_swap_w_ = 0;
    uint32_t last_seen_swap_h_ = 0;

    // Seed used by initParticles. Default 42 preserves the existing golden;
    // the rng.seed NDJSON op writes through SetRandomSeed before init.
    uint32_t random_seed_ = 42;
    // Fiedler fixed-timestep accumulator state. Game-thread only -- never
    // touched by the render thread. clock_ is WallClock in live mode,
    // FixedClock under CAIRNS_DUMP.
    std::unique_ptr<cairns::FrameClock> clock_;
    double accumulator_ = 0.0;
    uint64_t sim_frame_ = 0;
    float sim_angle_deg_ = 0.0f;
    float render_angle_deg_ = 0.0f;
    uint32_t sim_steps_this_frame_ = 0;
    bool golden_ = false;
    // Subset of golden_: only true when dump_path != "" (the CLI byte-gate
    // path). Tests use use_fixed_clock => golden_=true, dump_and_exit_=false.
    bool dump_and_exit_ = false;
    // Optional override for HUD numbers (HudStats::Mock used by the imgui
    // overlay golden so the captured screen is byte-stable). When present,
    // the HUD draw path reads from this instead of HudFromTimer. Set via
    // SetInjectedHudStats from the golden test harness.
    std::optional<cairns::HudStats> injected_hud_stats_;
    // A.2: particle_sim + particle_draw gate. Mirrors
    // EngineConfig::particles_enabled at GreaterInit; runtime-mutable via
    // Engine::EnableParticles(bool). Tests default false; CLI shells default
    // true via shell/env_config.cpp.
    bool particles_enabled_ = false;
    // Set via SetNestedGraphMode (JS render.nestedGraph op). Currently
    // informational only -- the engine's render graph composes the right
    // shape (multiple forward passes, one per active viewport) regardless.
    // Will gate the resolved-depth path once the ReadBackBuffer salvage
    // is wired.
    bool nested_graph_mode_ = false;
    // A.7: stamped at end of BuildMeshOpaqueDraws every frame.
    FrameStats last_frame_stats_{};
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

