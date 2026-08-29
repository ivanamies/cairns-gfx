#pragma once

#include "util/define.hpp"

#include <cmath>
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <thread>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numbers>
#include <variant>

#include <condition_variable>
#include <mutex>

#include <stb_image_write.h>

#include "gfx_api.hpp"
#include "rhi/init_config.hpp"
#include "rhi/swap_chain.hpp"
#include "gpu_scene_registry.hpp"
#include "util/misc.hpp"
#include "util/std_allocator.hpp"
#include "util/render_pass_globals.hpp"
#include "util/offset_allocator.hpp"
#include "util/gltf_loader.hpp"
#include "util/debug_asset.hpp"
#include "util/draw.hpp"
#include "util/draw_key.hpp"
#include "util/material_gpu.hpp"
#include "util/scene_gpu.hpp"
#include "util/timer.hpp"
#include "util/imgui_snapshot.hpp"
#include "util/frame_clock.hpp"
#include "util/log.hpp"
#include "scene/asset_registry.hpp"
#include "scene/components.hpp"
#include "scene/world.hpp"
#include "render/frame_packet.hpp"
#include "render/render_extract.hpp"
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

inline static constexpr uint32_t kHotArenaMemorySize = 1 << 29;
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
    using MatId = uint32_t;
    using SamplerHandle = rhi::Handle<rhi::Sampler>;
    using BindGroupId = uint32_t;

    static constexpr uint32_t kFramesInFlight = 2;

    // Per-slot storage. drawList / drawListSorted / proxies / resident_textures
    // / draw_world_matrices live here so the game thread can fill slot S while
    // the render thread reads slot ~S. Capacity grows on demand; .clear()/
    // .resize() preserve buffers across frame reuse. pending_globals etc. are
    // staged by Build and consumed by EncodeDraws.
    struct PerSlot {
        cairns::RenderProxyArrays proxies;
        std::vector<cairns::Draw, cairns::Allocator<cairns::Draw>> drawList;
        std::vector<std::pair<cairns::DrawKey, uint32_t>,
                    cairns::Allocator<std::pair<cairns::DrawKey, uint32_t>>>
            drawListSorted;
        std::vector<rhi::Handle<rhi::Texture>> resident_textures;
        std::vector<glm::mat4> draw_world_matrices;
        cairns::rhi::RenderPassGlobals pending_globals{};
        glm::mat4 pending_view_matrix{1.0f};
        float pending_near_z = 0.1f;
        float pending_far_z = 100.0f;
        uint32_t globals_offset = 0;
        uint32_t dt_off = 0;
        cairns::ImDrawDataSnapshot imgui_snapshot;
        cairns::FramePacket pkt{};

        explicit PerSlot(cairns::Arena& a)
            : drawList(cairns::Allocator<cairns::Draw>(a)),
              drawListSorted(
                  cairns::Allocator<std::pair<cairns::DrawKey, uint32_t>>(a)) {}
    };

    Engine() :
    hot_arena_mem_(malloc(kHotArenaMemorySize)),
    hot_arena_(hot_arena_mem_, kHotArenaMemorySize),
    scenes_(cairns::Allocator<cairns::Scene>(hot_arena_)),
    root_nodes_stack_cache_(cairns::Allocator<int32_t>(hot_arena_))
    {
        slots_.reserve(kFramesInFlight);
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            slots_.emplace_back(hot_arena_);
        }
    }
    
    bool initSwapChain(const rhi::InitConfig& cfg) {
        if ( !rhi_.device.InitSwapChain(swapchain_, cfg)) {
            return false;
        }

        return true;
    }
    
    bool requestResizeFrameBuffer(uint32_t width, uint32_t height) {
        (void)width;
        (void)height;
        return true;
    }

    bool RequestViewportDump(const std::filesystem::path& path) {
        rhi_.frames.SetDumpPath(path);
        return true;
    }
    
    bool initCpuAllocators() {
        return true;
    }
    
    bool initResourceManagers() {
        using namespace cairns;
        using namespace cairns::rhi;
        // 4 because we're only pretending to be a real UGC engine at this point
        return true;
    }
    
    bool GreaterInit(const rhi::InitConfig& cfg) {
        // Clock selection: CAIRNS_DUMP => FixedClock (golden); else WallClock.
        golden_ = (std::getenv("CAIRNS_DUMP") != nullptr);
        tiny_quad_test_ = (std::getenv("CAIRNS_TINY_QUAD") != nullptr);
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
        if (!rhi_.alloc.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: alloc.Init failed\n");
            return false;
        }
        if (!rhi_.resources.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: resources.Init failed\n");
            return false;
        }
        if (!rhi_.frames.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: frames.Init failed\n");
            return false;
        }
        if (!rhi_.pipelines.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: pipelines.Init failed\n");
            return false;
        }
        if ( !initSwapChain(cfg)) {
            CAIRNS_PRINT("GreaterInit: initSwapChain failed\n");
            return false;
        }
        { // init debug assets
            std::vector<std::filesystem::path> glb_paths;
            if (const char* env_glb = std::getenv("CAIRNS_GLB")) {
                std::string spec(env_glb);
                size_t start = 0;
                while (start <= spec.size()) {
                    size_t comma = spec.find(',', start);
                    std::string tok = spec.substr(
                        start, comma == std::string::npos ? std::string::npos
                                                          : comma - start);
                    if (!tok.empty()) {
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
                    if (comma == std::string::npos) {
                        break;
                    }
                    start = comma + 1;
                }
            } else {
                for (size_t glb_idx = cairns::kDebugGlbsToParseStart;
                     glb_idx < cairns::kDebugGlbsToParseStart + cairns::kDebugGlbsToParse;
                     ++glb_idx) {
                    std::filesystem::path filepath;
                    if (!cairns::GetStaticResourceFilepath(cairns::kDebugGlbs[glb_idx],
                                                           filepath)) {
                        fprintf(stderr, "file missing %s\n", cairns::kDebugGlbs[glb_idx]);
                        continue;
                    }
                    glb_paths.push_back(filepath);
                }
            }

            const int kHeroSlices = 33;
            const int loaded_heroes = static_cast<int>(glb_paths.size());
            const int instance_count =
                std::getenv("CAIRNS_N") ? std::atoi(std::getenv("CAIRNS_N"))
                                        : loaded_heroes * kHeroSlices;
            const int grid_n = std::max(
                1, static_cast<int>(std::ceil(std::sqrt(
                       static_cast<float>(instance_count)))));
            const float spacing = 4.0f / static_cast<float>(grid_n);
            const float scale =
                std::getenv("CAIRNS_SCALE")
                    ? static_cast<float>(std::atof(std::getenv("CAIRNS_SCALE")))
                    : 0.013f / static_cast<float>(grid_n);
            const float start = -spacing * static_cast<float>(grid_n - 1) * 0.5f;
            debugSceneXforms_ = cairns::GenerateDebugGridTransforms(
                glm::vec3(start, start, -3), grid_n, spacing, spacing, 1.0f, scale,
                instance_count);

            for (const std::filesystem::path& filepath : glb_paths) {
                scenes_.push_back(cairns::Scene(hot_arena_));
                cairns::Scene& scene = scenes_.back();
                if (!cairns::LoadSceneFromGltf(filepath, scene)) {
                    return false;
                }
                cairns::PrepareSceneResources(scene, rhi_.resources, rhi_.alloc, materials_);
            }

            if (!cairns::rhi::LoadScenesGpu(
                    std::span<cairns::Scene>(scenes_.data(), scenes_.size()),
                    rhi_.resources, rhi_.alloc)) {
                return false;
            }

            for (cairns::Scene& scene : scenes_) {
                scene.CleanupTmps();
            }
        }
        if (!scenes_.empty() && !scenes_[0].meshes.empty()) {
            mesh_master_handle_ = scenes_[0].meshes[0].posHandle;
        }

        // EnTT scene-layer path. Register each loaded Scene with the
        // AssetRegistry, then create one entity per debug-grid xform in
        // the active world's registry. SceneEntity / SceneWorld are
        // gone -- the entt::registry IS the source of truth.
        if (!scenes_.empty()) {
            // Pre-allocate hot/cold cells up to kMaxWorlds so Acquire
            // doesn't trigger a vector growth that would move
            // World::Cold and invalidate any cached pointers. The
            // unique_ptr<entt::registry> inside Cold is the second
            // safety layer.
            for (uint32_t w = 0; w < kMaxWorlds; ++w) {
                cairns::WorldId tmp = worlds_.Acquire();
                worlds_.Release(tmp);
            }

            // Shared GPU buffer handles -- all GLBs alias the same
            // packed buffer-set (see scene_gpu.hpp).
            const auto pos_handle = scenes_[0].meshes[0].posHandle;
            const auto attr_handle = scenes_[0].meshes[0].attrHandle;
            const auto idx_handle = scenes_[0].meshes[0].indexHandle;

            std::vector<cairns::AssetId> per_scene_asset;
            per_scene_asset.reserve(scenes_.size());
            for (size_t s_idx = 0; s_idx < scenes_.size(); ++s_idx) {
                per_scene_asset.push_back(
                    assets_.RegisterExistingScene(
                        static_cast<uint32_t>(s_idx), &scenes_[s_idx],
                        pos_handle, attr_handle, idx_handle));
            }

            active_world_ = worlds_.Acquire();
            cairns::World::Hot* wh = worlds_.GetHot(active_world_);
            cairns::World::Cold* wc = worlds_.GetCold(active_world_);
            if (wh && wc) {
                // Reused-slot trap: fresh re-init in case this slot was
                // recycled. unique_ptr<registry> + dirty get reset.
                *wc = cairns::World::Cold{};
                wh->proxy_slot = 0;
                wh->dirty = true;

                auto& reg = wc->registry;
                for (size_t i = 0; i < debugSceneXforms_.size(); ++i) {
                    const uint32_t scene_idx =
                        static_cast<uint32_t>(i % scenes_.size());
                    const entt::entity e = reg.create();
                    cairns::WorldTransform wt;
                    wt.world = debugSceneXforms_[i];
                    reg.emplace<cairns::WorldTransform>(e, wt);
                    cairns::AssetRef ar;
                    ar.asset = per_scene_asset[scene_idx];
                    reg.emplace<cairns::AssetRef>(e, ar);
                    cairns::Renderable rdr;
                    rdr.layer_mask = 0xFFFFFFFFu;
                    rdr.flags = cairns::kProxyVisible;
                    reg.emplace<cairns::Renderable>(e, rdr);
                }
            }
            world_proxies_.resize(1);  // active_world_ uses slot 0

            // P6: open a SECOND world to flush single-world assumptions
            // in the type system + pool plumbing. Populated with half
            // the debug-grid for visible distinctness if anyone wires a
            // second view to it. NOT rendered yet -- the active path
            // still draws only active_world_. P6's isolation gate is
            // satisfied by "world 0 pixels unchanged when world 1
            // exists." Full side-by-side rendering (per-view targets +
            // tiled composite + composite_pip variant) is the next
            // commit on top of this seam.
            secondary_world_ = worlds_.Acquire();
            if (auto* wh2 = worlds_.GetHot(secondary_world_)) {
                if (auto* wc2 = worlds_.GetCold(secondary_world_)) {
                    *wc2 = cairns::World::Cold{};
                    wh2->proxy_slot = 1;
                    wh2->dirty = true;
                    auto& reg2 = wc2->registry;
                    const size_t half = debugSceneXforms_.size() / 2;
                    for (size_t i = 0; i < half; ++i) {
                        const uint32_t scene_idx = static_cast<uint32_t>(
                            i % scenes_.size());
                        const entt::entity e = reg2.create();
                        cairns::WorldTransform wt;
                        wt.world = debugSceneXforms_[i];
                        reg2.emplace<cairns::WorldTransform>(e, wt);
                        cairns::AssetRef ar;
                        ar.asset = per_scene_asset[scene_idx];
                        reg2.emplace<cairns::AssetRef>(e, ar);
                        cairns::Renderable rdr;
                        rdr.layer_mask = 0xFFFFFFFFu;
                        rdr.flags = cairns::kProxyVisible;
                        reg2.emplace<cairns::Renderable>(e, rdr);
                    }
                }
            }
            world_proxies_.resize(2);  // secondary_world_ uses slot 1
        }
        if ( !initRenderPipeline() ) {
            CAIRNS_PRINT("GreaterInit: initRenderPipeline failed\n");
            return false;
        }
        if ( !rhi_.frames.InitTargets(rhi_.resources, rhi_.alloc, swapchain_) ) {
            CAIRNS_PRINT("GreaterInit: frames.InitTargets failed\n");
            return false;
        }
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

        // CAMERA MUST ALWAYS REMAIN AT (0, 0, 0)
        const glm::vec3 camera_pos(0, 0, 0);
        const glm::vec3 camera_dir(0, 0, -1);
        const glm::vec3 world_up(0, 1, 0);

        const glm::mat4 view_matrix = glm::lookAtRH(camera_pos, camera_pos + camera_dir, world_up);

        const float aspect_ratio = (1.0f * swapchain_.Width()) / swapchain_.Height();
        const float fov = 90 * (std::numbers::pi / 180.0f);
        const float near_z = 0.1f;
        const float far_z = 100.0f;

        const glm::mat4 proj_matrix = glm::perspectiveRH_ZO(fov, aspect_ratio, near_z, far_z);
        const glm::mat4 view_proj = proj_matrix * view_matrix;

        const float screen_width = swapchain_.Width();
        const float screen_height = swapchain_.Height();
        s.pending_globals = cairns::rhi::RenderPassGlobals {
            .view_proj = view_proj,
            .inv_view_proj = glm::inverse(view_proj),
            .camera_pos = glm::vec4(camera_pos, 1.0f /*exposure */),
            .camera_dir = glm::vec4(camera_dir, near_z),
            .screen_params = glm::vec4(screen_width, screen_height, 1.0f / screen_width, 1.0f / screen_height)
        };
        s.pending_view_matrix = view_matrix;
        s.pending_near_z = near_z;
        s.pending_far_z = far_z;

        // Set the active world's root_transform, run TRS hierarchy
        // propagation (no-op when no entity carries a Transform; the
        // current scene-load emplaces WorldTransform directly), then
        // extract. Extract composes node.globalTransform * (world *
        // root_transform).
        {
            cairns::World::Hot* wh = worlds_.GetHot(active_world_);
            cairns::World::Cold* wc = worlds_.GetCold(active_world_);
            if (wh && wc) {
                wh->root_transform = rot_matrix;
                cairns::PropagateTransforms(*wc, glm::mat4(1.0f));
                cairns::ExtractFromWorld(*wc, wh->root_transform, assets_,
                                         s.proxies);
            }
        }

        // Counting pass -> total_draws.
        uint32_t total_draws = 0;
        for (const cairns::MeshProxy& mp : s.proxies.meshes.data) {
            total_draws += mp.primitive_count;
        }
        s.drawList.resize(total_draws);
        s.drawListSorted.resize(total_draws);
        s.draw_world_matrices.resize(total_draws);

        // Fill pass -- stable_idx assigned by prefix sum over the proxy walk
        // (deterministic of input order, independent of execution order so a
        // future parallel_for is a drop-in).
        uint32_t stable_idx = 0;
        for (const cairns::MeshProxy& mp : s.proxies.meshes.data) {
            const BufHandle pos = mp.pos;
            [[maybe_unused]] const BufHandle attr = mp.attr;
            const BufHandle index = mp.index;
            const glm::mat4& world_mat = mp.world_matrix;
            const uint32_t index_base_off = rhi_.resources.BufferBaseOffset(rhi_.alloc, index);
            for (uint32_t p = 0; p < mp.primitive_count; ++p) {
                const cairns::PrimitiveProxy& prim = s.proxies.primitives[mp.first_primitive + p];
                const MatId mat_id = prim.material_id;

                cairns::Draw draw{};
                draw.bind_groups[1] = material_bind_groups_[mat_id];
                draw.index_buffer = index;
                draw.index_offset = index_base_off + (prim.first_index * sizeof(uint32_t));
                draw.vertex_offset = prim.vertex_offset;
                draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot] = pos;
                draw.vertex_buffers[cairns::Draw::kVertexBufferAttrSlot] = attr;
                draw.instance_offset = 0;
                draw.instance_count = 1;
                draw.dynamic_buffer_offsets[0] = UINT32_MAX;  // material   - filled by EncodeDraws
                draw.dynamic_buffer_offsets[1] = UINT32_MAX;  // draw_tmp   - filled by EncodeDraws
                assert(prim.index_count % 3 == 0);
                draw.triangle_count =
                    tiny_quad_test_ ? 2 : prim.index_count / 3;

                const glm::vec4 view_pos = view_matrix * world_mat[3];
                const float view_depth = -view_pos.z;
                const float d01 = glm::clamp(
                    (view_depth - near_z) / (far_z - near_z), 0.0f, 1.0f);
                const uint32_t depth_q =
                    static_cast<uint32_t>(d01 * float((1u << 24) - 1));
                s.drawListSorted[stable_idx] = std::make_pair(
                    cairns::BuildDrawKey(mat_id & 0x3FFFFFFFu, depth_q,
                                         kMockTranslucency, kMockViewport,
                                         kMockViewportLayer, kMockFullscreenLayer),
                    stable_idx);
                s.drawList[stable_idx] = draw;
                s.draw_world_matrices[stable_idx] = world_mat;
                ++stable_idx;
            }
        }
        assert(stable_idx == total_draws);

        return true;
    }

    // Render-side bump of all per-frame UBOs. Order matters: globals first,
    // per-draw (material, draw_tmp) in stable_idx order, fixed_dt last.
    // Writes s.globals_offset, s.drawList[*].dynamic_buffer_offsets[0..1],
    // s.dt_off. Compute kernel sees pkt.fixed_dt (constant sim dt), not wall.
    void EncodeDraws(const FramePacket& pkt) {
        PerSlot& s = slots_[pkt.slot];
        // 1. globals UBO.
        void* gptr = rhi_.alloc.BumpAllocate(
            sizeof(cairns::rhi::RenderPassGlobals), rhi_.alloc.UboAlign(),
            rhi::Memory::kDynamic, &s.globals_offset);
        assert(gptr && "bump alloc failed: render pass globals");
        memcpy(gptr, &s.pending_globals, sizeof(s.pending_globals));
        if (frame_ <= 6) {
            const glm::mat4& vp = s.pending_globals.view_proj;
            const float aspect_ratio = (1.0f * swapchain_.Width()) / swapchain_.Height();
            size_t entity_count = 0;
            if (auto* wc = worlds_.GetCold(active_world_)) {
                entity_count = wc->registry.storage<entt::entity>().size();
            }
            fprintf(stderr,
                    "[FLAKE] frame=%u w=%u h=%u aspect=%.9f vp00=%.9f vp11=%.9f "
                    "vp22=%.9f vp32=%.9f goff=%u par_in=%u par_out=%u "
                    "entities=%zu meshes=%zu prims=%zu\n",
                    frame_, swapchain_.Width(), swapchain_.Height(), aspect_ratio,
                    vp[0][0], vp[1][1], vp[2][2], vp[3][2],
                    s.globals_offset, pkt.particle_parity_in, pkt.particle_parity_out,
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

            const cairns::rhi::DrawTmp draw_tmp { .model_matrix = s.draw_world_matrices[i] };
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

    bool draw() {
        frame_++;
        const uint32_t slot = (frame_ - 1) % kFramesInFlight;

        // Acquire BEFORE touching slot storage -- this is the backpressure
        // gate, blocks if the render thread is still holding slot S.
        render_thread_->Acquire(slot);

        PerSlot& s = slots_[slot];

        const uint64_t cpu_now_ns = cairns::timestamp_ns();
        if (cpu_last_frame_ns_ != 0) {
            cpu_ms_last_ = static_cast<float>(cpu_now_ns - cpu_last_frame_ns_) / 1.0e6f;
            cpu_ms_history_[cpu_ms_head_] = cpu_ms_last_;
            cpu_ms_head_ = (cpu_ms_head_ + 1) % kCpuMsHistory;
        }
        cpu_last_frame_ns_ = cpu_now_ns;
        s.pkt.request_dump = false;
        s.pkt.dump_path.clear();
        if (golden_ && !dump_emitted_ && sim_frame_ >= cairns::kGoldenDumpFrame) {
            const char* dump = std::getenv("CAIRNS_DUMP");
            s.pkt.request_dump = true;
            s.pkt.dump_path = dump ? dump : "/tmp/cairns_dump.png";
            dump_emitted_ = true;
            dump_emit_frame_ = frame_;
        }
        if (dump_emitted_ && frame_ >= dump_emit_frame_ + 2) {
            std::exit(0);  // headless byte-gate: dump frame flushed, now quit
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
        std::sort(s.drawListSorted.begin(), s.drawListSorted.end());

        s.resident_textures.clear();
        for (auto& scene : scenes_) {
            for (const auto th : scene.textureHandles) {
                s.resident_textures.push_back(th);
            }
        }

        // Fill packet header (the view into per-slot storage).
        s.pkt.frame_idx = frame_;
        s.pkt.slot = slot;
        s.pkt.view = s.pending_view_matrix;
        s.pkt.proj = glm::mat4(1.0f);  // not used downstream; view_proj baked into pending_globals
        s.pkt.near_z = s.pending_near_z;
        s.pkt.far_z = s.pending_far_z;
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
            s.resident_textures.data(), s.resident_textures.size());

        const bool draw_imgui = !golden_;
        if (draw_imgui) {
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.85f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
            ImGui::Begin("cairns", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            const float fps = cpu_ms_last_ > 0.0f ? 1000.0f / cpu_ms_last_ : 0.0f;
            float ms_max = 1.0f;
            float ms_avg = 0.0f;
            for (int i = 0; i < kCpuMsHistory; ++i) {
                ms_max = cpu_ms_history_[i] > ms_max ? cpu_ms_history_[i] : ms_max;
                ms_avg += cpu_ms_history_[i];
            }
            ms_avg /= static_cast<float>(kCpuMsHistory);
            ImGui::Text("CPU %.2f ms   |   %.0f FPS", cpu_ms_last_, fps);
            ImGui::Text("avg %.2f ms   |   peak %.2f ms", ms_avg, ms_max);
            auto slot_avg_ms = [](int s) -> float {
                const uint64_t n = cairns::Timer::accum_itrs_[s];
                if (n == 0) {
                    return 0.0f;
                }
                return static_cast<float>(
                    cairns::Timer::accum_times_[s] / static_cast<double>(n) /
                    1000.0);
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
                if (cairns::Timer::accum_itrs_[s] == 0) {
                    continue;
                }
                const char* nm = cairns::Timer::slot_names_[s]
                                     ? cairns::Timer::slot_names_[s]
                                     : "?";
                if (std::strcmp(nm, "set up render pass globals") == 0 ||
                    std::strcmp(nm, "build opaque draw list") == 0 ||
                    std::strcmp(nm, "particle_sim") == 0 ||
                    std::strcmp(nm, "forward") == 0) {
                    continue;
                }
                ImGui::Text("%-12s %5.2f ms", nm, slot_avg_ms(s));
            }
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%.2f ms", cpu_ms_last_);
            ImGui::PlotLines("##cpuhist", cpu_ms_history_, kCpuMsHistory,
                             cpu_ms_head_, overlay, 0.0f, ms_max * 1.15f,
                             ImVec2(300.0f, 110.0f));
            ImGui::End();
            ImGui::PopStyleColor(4);
            ImGui::Render();
            cairns::SnapshotImDrawData(ImGui::GetDrawData(), s.imgui_snapshot);
            s.pkt.imgui_snapshot = &s.imgui_snapshot.data;
        } else {
            s.imgui_snapshot.Clear();
            s.pkt.imgui_snapshot = nullptr;
        }

        render_thread_->Submit(slot, &s.pkt);

        // Under CAIRNS_DUMP, collapse to depth-1 pipelining: wait for the
        // render thread to fully complete this frame before the next iteration
        // queues another. Keeps frame 5's dump output byte-identical regardless
        // of threading (Drain forces same parity sequence as single-threaded).
        if (std::getenv("CAIRNS_DUMP")) {
            render_thread_->Drain();
        }

        t_frame.End();
        if (frame_ % 120 == 0) {
            const size_t loaded = scenes_.size();
            size_t entities = 0;
            if (auto* wc = worlds_.GetCold(active_world_)) {
                entities = wc->registry.storage<entt::entity>().size();
            }
            const size_t slices = loaded > 0 ? entities / loaded : 0;
            CAIRNS_PRINT("============\n");
            CAIRNS_PRINT("draws %zu | %zu GLBs x %zu slices = %zu entities | resolution %u x %u\n",
                         s.drawList.size(), loaded, slices, entities,
                         swapchain_.Width(), swapchain_.Height());
            cairns::Timer::PrintReport();
            cairns::Timer::Reset();
        }
        return true;
    }

    // Render-thread entry point (post commit 6). Today called synchronously
    // from draw(). Owns: rhi_.frames.Begin/End, the bump-ring EncodeDraws,
    // the compute + render-pass encode. Reads pkt + slots_[pkt.slot].
    void RecordFrame(FramePacket& pkt) {
        [[maybe_unused]] cairns::TaskGuard task_guard;

        PerSlot& s = slots_[pkt.slot];

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
            rhi_.frames.SetDumpPath(pkt.dump_path);
        }

        rhi::FrameContext fc = rhi_.frames.Begin(rhi_.resources, rhi_.alloc, swapchain_);

        cairns::Timer t_record("record", 2);
        EncodeDraws(pkt);

        if (!graph_) {
            graph_ = std::make_unique<rhi::RenderGraph>(rhi_.resources, rhi_.alloc);
        }
        graph_->Reset();

        rhi::MeshDrawList ml{};
        ml.draws = pkt.draws;
        ml.sorted_draws = pkt.sorted;
        ml.pipeline = unlit_offscreen_;
        ml.globals_offset = s.globals_offset;
        ml.resident_textures = pkt.resident_textures;
        ml.resident_buffers =
            std::span<const rhi::Handle<rhi::Buffer>>(&mesh_master_handle_, 1);

        rhi::PointDraw pd{};
        pd.pipeline = particle_render_offscreen_;
        pd.vertex_buffer = particle_ssbo_[pkt.particle_parity_out];
        pd.vertex_offset = 0;
        pd.vertex_count = kParticleCount;

        const float clear[4] = {41.0f / 255.0f, 42.0f / 255.0f, 48.0f / 255.0f, 1.0f};
        const uint32_t fb_w = swapchain_.Width();
        const uint32_t fb_h = swapchain_.Height();

        // pass 1: particle_sim kCompute. import the writer ssbo so prune keeps
        // it (external side effect -- game thread reads particle_parity_out).
        rhi::GraphBuffer sim_out;
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
                    const uint32_t step_dst = step_src ^ 1u;
                    rhi::BoundBuffer cbufs[3] = {
                        {0, rhi_.alloc.BumpMasterBuffer(rhi::Memory::kDynamic),
                         s.dt_off},
                        {1, particle_ssbo_[step_src], 0},
                        {2, particle_ssbo_[step_dst], 0},
                    };
                    cd.buffers = std::span<const rhi::BoundBuffer>(cbufs, 3);
                    cd.step_index = k;
                    cmd.Dispatch(rhi_.resources, rhi_.alloc, cd);
                }
            });

        // pass 2: forward kGraphics. offscreen color + depth, single-sample.
        rhi::GraphTexture color_off;
        rhi::GraphTexture depth_off;
        graph_->AddPass(
            "forward", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                rhi::GraphTextureDesc cd{};
                cd.width = fb_w;
                cd.height = fb_h;
                cd.format = rhi::Format::kBgra8Unorm;
                cd.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled;
                color_off = b.CreateColorTarget(cd);
                rhi::GraphTextureDesc dd{};
                dd.width = fb_w;
                dd.height = fb_h;
                dd.format = rhi::Format::kD32F;
                dd.usage = rhi::kTexUsageDepthTarget | rhi::kTexUsageSampled;
                depth_off = b.CreateDepthTarget(dd);
                b.AddColorOutput("color", color_off, rhi::LoadOp::kClear, clear);
                b.AddDepthOutput("fwd_depth", depth_off, rhi::LoadOp::kClear, 1.0f);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                cmd.DrawMeshes(rhi_.resources, rhi_.alloc, ml);
                cmd.DrawPoints(rhi_.resources, rhi_.alloc, pd);
            });

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
                swap_tex = b.ImportTexture(rhi::Handle<rhi::Texture>::Null, td);
                b.AddColorOutput("swapchain", swap_tex, rhi::LoadOp::kClear, clear);
                b.AddAttachmentInput(color_off);
                b.AddAttachmentInput(depth_off);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources& res) {
                const rhi::Handle<rhi::Texture> color = res.Resolve(color_off);
                const rhi::Handle<rhi::Texture> depth = res.Resolve(depth_off);
                // composite: full-screen forward color.
                cmd.DrawFullscreen(rhi_.resources, composite_pip_,
                                   std::span<const rhi::Handle<rhi::Texture>>(&color, 1),
                                   composite_sampler_);
                // bottom-right 25% PIP, depthviz silhouette.
                const float x = 0.75f * static_cast<float>(fb_w);
                const float y = 0.75f * static_cast<float>(fb_h);
                const float w = 0.25f * static_cast<float>(fb_w);
                const float h = 0.25f * static_cast<float>(fb_h);
                cmd.SetViewport(x, y, w, h);
                cmd.SetScissor(static_cast<int32_t>(x), static_cast<int32_t>(y),
                               static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                cmd.DrawFullscreen(rhi_.resources, depthviz_,
                                   std::span<const rhi::Handle<rhi::Texture>>(&depth, 1),
                                   composite_sampler_);
                // restore full extent before the ui draw.
                cmd.SetViewport(0.0f, 0.0f, static_cast<float>(fb_w),
                                static_cast<float>(fb_h));
                cmd.SetScissor(0, 0, fb_w, fb_h);
                // ui (in-encoder phase).
                if (pkt.imgui_snapshot) {
                    cmd.DrawImGui(rhi_.resources, rhi_.alloc, imgui_, imgui_font_,
                                  imgui_sampler_, pkt.imgui_snapshot);
                }
            });

        graph_->SetOutput(swap_tex);
        if (!graph_->Bake() || !graph_->Execute(fc, swapchain_)) {
            t_record.End();
            rhi_.frames.End(swapchain_, fc);
            return;
        }
        if (frame_ <= 6) {
            const rhi::Handle<rhi::Texture> coff = graph_->ResolveTexture(color_off);
            const rhi::Handle<rhi::Texture> doff = graph_->ResolveTexture(depth_off);
            fprintf(stderr,
                    "[FLAKE-R] frame=%u slot=%u img=%u color_off=%u/%u depth_off=%u/%u "
                    "steps=%u\n",
                    frame_, pkt.slot, fc.swapchain_image_index,
                    coff.index, coff.generation, doff.index, doff.generation,
                    pkt.sim_steps_this_frame);
        }
        t_record.End();
        rhi_.frames.End(swapchain_, fc);
    }
    
    bool initRenderPipeline() {
        {
            // set-2 per-material bind groups (portable path). Dense, indexed by
            // MatId. Texture+sampler arrive via this group, not the global table.
            material_bind_groups_.assign(materials_.size(),
                                         rhi::Handle<rhi::BindGroup>::Null);
            for (size_t m = 0; m < materials_.size(); ++m) {
                const rhi::TextureBinding tb{0, materials_[m].color};
                const rhi::SamplerBinding sb{0, materials_[m].sampler};
                rhi::BindGroupDesc bgd{};
                bgd.textures = std::span<const rhi::TextureBinding>(&tb, 1);
                bgd.samplers = std::span<const rhi::SamplerBinding>(&sb, 1);
                material_bind_groups_[m] = rhi_.resources.CreateBindGroup(bgd);
            }
        }

        {  // unlit graphics pipeline via rhi
            const char* base = SDL_GetBasePath();
            const std::string shader_dir = base ? base : "";
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
            desc.debug_name = "unlit";
            desc.swap_chain = &swapchain_;
            unlit_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, desc);
            if (unlit_.IsNull()) {
                std::exit(0);
            }

            // Offscreen variant: single-sample, no swapchain compat. Same shaders
            // + vertex layout as unlit; targets a render-graph color_off+depth_off.
            rhi::GraphicsPipelineDesc ofd = desc;
            ofd.logical_shader = "unlit_offscreen";
            ofd.sample_count = 1;
            ofd.swap_chain = nullptr;
            ofd.debug_name = "unlit_offscreen";
            unlit_offscreen_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, ofd);
            if (unlit_offscreen_.IsNull()) {
                std::exit(0);
            }

            // composite_pip: full-screen tri, samples 1 color tex, writes MSAA swap.
            rhi::GraphicsPipelineDesc cpd{};
            cpd.logical_shader = "composite_pip";
            cpd.shader_dir = shader_dir.c_str();
            cpd.topology = rhi::PrimitiveTopology::kTriangleList;
            cpd.cull = rhi::CullMode::kNone;
            cpd.depth_test = false;
            cpd.depth_write = false;
            cpd.color_format = rhi::Format::kBgra8Unorm;
            cpd.depth_format = rhi::Format::kD32F;
            cpd.sample_count = sampleCount;
            cpd.debug_name = "composite_pip";
            cpd.swap_chain = &swapchain_;
            composite_pip_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, cpd);

            // depthviz: same shape, samples 1 depth tex, blue silhouette.
            rhi::GraphicsPipelineDesc dvd = cpd;
            dvd.logical_shader = "depthviz";
            dvd.debug_name = "depthviz";
            depthviz_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, dvd);

            if (composite_pip_.IsNull() || depthviz_.IsNull()) {
                std::exit(0);
            }

            rhi::SamplerDesc sd{};
            sd.min_filter = rhi::Filter::kLinear;
            sd.mag_filter = rhi::Filter::kLinear;
            sd.mip_filter = rhi::Filter::kLinear;
            sd.address_mode = rhi::AddressMode::kClampToEdge;
            composite_sampler_ = rhi_.resources.CreateSampler(sd);
        }

        return true;
    }

    struct Particle {
        float position[2];
        float velocity[2];
        float color[4];
    };

    bool initParticles() {
        const char* base = SDL_GetBasePath();
        const std::string shader_dir = base ? base : "";
        {  // particle compute kernel via rhi
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "particle_compute";
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
            // Offscreen variant for the render-graph forward pass.
            rhi::GraphicsPipelineDesc opd = desc;
            opd.sample_count = 1;
            opd.swap_chain = nullptr;
            opd.debug_name = "particle_render_offscreen";
            particle_render_offscreen_ = rhi_.pipelines.CreateGraphicsPipeline(
                rhi_.resources, rhi_.frames, opd);
            if (particle_render_offscreen_.IsNull()) {
                return false;
            }
        }

        {
            // imgui pipeline (swapchain MSAA, alpha blend, no depth).
            const char* base = SDL_GetBasePath();
            const std::string shader_dir = base ? base : "";
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
            id.color_format = rhi::Format::kBgra8Unorm;
            id.depth_format = rhi::Format::kD32F;
            id.sample_count = sampleCount;
            id.push_constant_bytes = 16;
            id.debug_name = "imgui";
            id.swap_chain = &swapchain_;
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
                static_cast<float>(swapchain_.Width()) / kRefWidth;
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

        // Deterministic particle seed -- matches the golden capture path.
        std::srand(42);
        std::vector<Particle> particles(kParticleCount);
        for (uint32_t i = 0; i < kParticleCount; ++i) {
            const float r = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
            const float theta = r * 2.0f * static_cast<float>(std::numbers::pi);
            const float radius = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
            particles[i].position[0] = radius * std::cos(theta);
            particles[i].position[1] = radius * std::sin(theta);
            const float vx = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 0.5f;
            const float vy = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 0.5f;
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
        rhi_.resources.Deinit();
        rhi_.alloc.Deinit();
        rhi_.device.Deinit();
        return true;
    }
    
private:
    // todo @iamies
    // make an engine dtor and delete this
    void* hot_arena_mem_;
    cairns::Arena hot_arena_;
    
    ////////// DO NOT MOVE ARENA BELOW THIS LINE. because c++.
    
    uint32_t frame_ = 0;

    std::vector<cairns::Scene, cairns::Allocator<cairns::Scene>> scenes_;
    std::vector<int32_t, cairns::Allocator<int32_t>> root_nodes_stack_cache_;

    std::vector<glm::mat4> debugSceneXforms_;

    std::vector<cairns::LoadedMaterial> materials_;
    // set-2 per-material bind groups, dense by MatId (NO hash). Built once at load.
    std::vector<rhi::Handle<rhi::BindGroup>> material_bind_groups_;

    // Per-slot frame buffers (drawList / drawListSorted / proxies /
    // resident_textures / draw_world_matrices / pending_globals / globals_offset
    // / dt_off / FramePacket). See PerSlot above.
    std::vector<PerSlot> slots_;

    // EnTT scene-layer path. worlds_ pre-reserved at startup
    // (kMaxWorlds Acquire+Release cycle) to keep World::Cold* pointer
    // stable across real Acquire later.
    static constexpr uint32_t kMaxWorlds = 8;
    cairns::AssetRegistry assets_;
    cairns::ResourceManager<cairns::World> worlds_;
    std::vector<cairns::RenderProxyArrays> world_proxies_;
    cairns::WorldId active_world_;
    cairns::WorldId secondary_world_;  // P6 multi-world coexistence test

    rhi::Rhi rhi_;
    rhi::Handle<rhi::Buffer> mesh_master_handle_ = rhi::Handle<rhi::Buffer>::Null;
    // Per-frame render graph. Reused via Reset() across frames (vector storage
    // for passes/textures is preserved). Constructed lazily on first RecordFrame
    // because Resources& / Allocator& must already be initialized.
    std::unique_ptr<rhi::RenderGraph> graph_;

    cairns::rhi::SwapChain swapchain_;
    // shaders
    ShaderHandle unlit_ = ShaderHandle::Null;
    ShaderHandle unlit_offscreen_ = ShaderHandle::Null;
    ShaderHandle composite_pip_ = ShaderHandle::Null;
    ShaderHandle depthviz_ = ShaderHandle::Null;
    rhi::Handle<rhi::Sampler> composite_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // imgui
    rhi::Handle<rhi::Shader> imgui_ = rhi::Handle<rhi::Shader>::Null;
    rhi::Handle<rhi::Texture> imgui_font_ = rhi::Handle<rhi::Texture>::Null;
    rhi::Handle<rhi::Sampler> imgui_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    // particles
    static constexpr uint32_t kParticleCount = 512;
    rhi::Handle<rhi::Kernel> particle_kernel_;
    rhi::Handle<rhi::Shader> particle_render_shader_;
    rhi::Handle<rhi::Shader> particle_render_offscreen_;
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
    bool dump_emitted_ = false;
    uint32_t dump_emit_frame_ = 0;
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
    // Diagnostic: pin every draw to 2 triangles. Draw count + submission
    // identical, geometry throughput ~700x smaller. Isolates draw-submission
    // overhead vs geometry-throughput in the forward pass cost.
    bool tiny_quad_test_ = false;
    // cpu frame-time history (wall-clock between draw() calls) for the imgui graph
    static constexpr int kCpuMsHistory = 128;
    float cpu_ms_history_[kCpuMsHistory] = {};
    int cpu_ms_head_ = 0;
    float cpu_ms_last_ = 0.0f;
    uint64_t cpu_last_frame_ns_ = 0;
    // render pass
    static constexpr size_t sampleCount = 4;
};

} // namespace cairns

