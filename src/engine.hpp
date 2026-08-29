#pragma once

#include "util/define.hpp"

#include <cmath>
#include <cstdlib>
#include <string_view>
#include <filesystem>
#include <thread>
#include <fstream>
#include <iostream>
#include <numbers>
#include <variant>

#include <stb_image_write.h>

#include "gfx_api.hpp"
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
#include "util/log.hpp"
#include "scene/scene_world.hpp"
#include "render/render_extract.hpp"
#include "render/render_scene.hpp"
#include "render/render_graph.hpp"
#include "rhi/rhi.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/command_recorder.hpp"
#include <memory>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "util/cpu_arena.hpp"
#include "util/frame_clock.hpp"

namespace cairns {

inline static constexpr uint32_t kFrameSlabBytes = 4u * 1024u * 1024u;  // per-frame transient (A)
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
    
    Engine() :
    scenes_(cairns::Allocator<cairns::Scene>(hot_arena_)),
    root_nodes_stack_cache_(cairns::Allocator<int32_t>(hot_arena_)),
    drawListSorted_(cairns::Allocator<std::pair<cairns::DrawKey,uint32_t>>(hot_arena_)),
    drawList_(cairns::Allocator<cairns::Draw>(hot_arena_))
    {
        
    }
    
    bool initSwapChain(SDL_Window* window) {
        if ( !rhi_.device.InitSwapChain(swapchain_, window)) {
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
        // Allocator B: general chunk allocator backs hot_arena_ + provides slab for A.
        hot_arena_.Init();
        // Allocator A: per-frame transient ring. Slab borrowed from B. kFrameSlabBytes
        // is a starting size -- read frame_arena_.HighWaterAny() in Timer reports to tune.
        const uint32_t slab_bytes = rhi::kFramesInFlight * kFrameSlabBytes;
        frame_arena_slab_ = static_cast<uint8_t*>(
            hot_arena_.Allocate(slab_bytes, 16));
        if (frame_arena_slab_ == nullptr) {
            return false;
        }
        frame_arena_.Init<rhi::kFramesInFlight>(frame_arena_slab_, kFrameSlabBytes);
        return true;
    }
    
    bool initResourceManagers() {
        using namespace cairns;
        using namespace cairns::rhi;
        // 4 because we're only pretending to be a real UGC engine at this point
        return true;
    }
    
    bool GreaterInit(SDL_Window* window) {
        // Golden capture (headless byte-gate) auto-engages a FixedClock so the sim
        // is deterministic; live runs use a real WallClock. srand pinned for
        // reproducible particle spawn.
        golden_ = std::getenv("CAIRNS_DUMP") != nullptr;
        if (golden_) {
            clock_.reset(new cairns::FixedClock(kFixedDt));
        } else {
            clock_.reset(new cairns::WallClock());
        }
        srand(42);

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
        if (!rhi_.device.Init(window)) {
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
        if ( !initSwapChain(window)) {
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
                        printf("file missing %s\n", cairns::kDebugGlbs[glb_idx]);
                        continue;
                    }
                    glb_paths.push_back(filepath);
                }
            }

            const int kHeroSlices = 66;
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
        if (!scenes_.empty()) {
            world_.scenes = scenes_.data();
            world_.scene_count = scenes_.size();
            world_.ClearEntities();
            world_.entities.Reserve(debugSceneXforms_.size());
            world_.live_entities.reserve(debugSceneXforms_.size());
            for (size_t i = 0; i < debugSceneXforms_.size(); ++i) {
                cairns::SceneEntity::Hot e{};
                e.transform = debugSceneXforms_[i];
                e.scene_index = static_cast<uint32_t>(i % scenes_.size());
                world_.AddEntity(e);
            }
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

        if (std::getenv("CAIRNS_RG_TOY")) {
            cairns::rhi::RenderGraphToyTest(rhi_.resources, rhi_.alloc);
        }

        return true;
    }

    bool BuildMeshOpaqueDraws() {
        drawList_.clear();
        drawListSorted_.clear();
        
        const glm::mat4 rot_matrix = glm::rotate(
            glm::mat4(1.0f), glm::radians(render_angle_deg_), glm::vec3(0, 1.0, 0));
        
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
        
        { // set up render pass globals
            cairns::Timer t_build("set up render pass globals", 3);
            // set up camera
            const float screen_width = swapchain_.Width();
            const float screen_height = swapchain_.Height();
            glm::mat4 view_proj = proj_matrix * view_matrix;
            cairns::rhi::RenderPassGlobals render_pass_globals {
                .view_proj = view_proj,
                .inv_view_proj = glm::inverse(view_proj),
                .camera_pos = glm::vec4(camera_pos, 1.0f /*exposure */),
                .camera_dir = glm::vec4(camera_dir, near_z),
                .screen_params = glm::vec4(screen_width, screen_height, 1.0f / screen_width, 1.0f / screen_height)
            };
            void* gptr = rhi_.alloc.BumpAllocate(
                sizeof(cairns::rhi::RenderPassGlobals), rhi_.alloc.UboAlign(),
                rhi::Memory::kDynamic, &globals_offset_);
            assert(gptr && "bump alloc failed: render pass globals");
            memcpy(gptr, &render_pass_globals, sizeof(render_pass_globals));
            if (frame_ <= 6) {
                fprintf(stderr,
                        "[FLAKE] frame=%u w=%u h=%u aspect=%.9f vp00=%.9f vp11=%.9f "
                        "vp22=%.9f vp32=%.9f goff=%u parity=%u\n",
                        frame_, swapchain_.Width(), swapchain_.Height(), aspect_ratio,
                        view_proj[0][0], view_proj[1][1], view_proj[2][2], view_proj[3][2],
                        globals_offset_, particle_parity_);
            }
        }

        cairns::Timer t_build("build opaque draw list", 4);
        world_.root_transform = rot_matrix;
        {
            cairns::Timer t_flatten("scene flatten", 5);
            cairns::Extract(world_, proxies_);
        }
        for (const cairns::MeshProxy& mp : proxies_.meshes.data) {
            const BufHandle pos = mp.pos;
            [[maybe_unused]] const BufHandle attr = mp.attr;
            const BufHandle index = mp.index;
            const glm::mat4& world_mat = mp.world_matrix;
            const uint32_t index_base_off = rhi_.resources.BufferBaseOffset(rhi_.alloc, index);
            for (uint32_t p = 0; p < mp.primitive_count; ++p) {
                const cairns::PrimitiveProxy& prim = proxies_.primitives[mp.first_primitive + p];
                const MatId mat_id = prim.material_id;
                const cairns::rhi::MaterialGpu material_gpu {};
                uint32_t material_offset = 0;
                void* mptr = rhi_.alloc.BumpAllocate(
                    sizeof(cairns::rhi::MaterialGpu), rhi_.alloc.UboAlign(),
                    rhi::Memory::kDynamic, &material_offset);
                assert(mptr && "bump alloc failed: material");
                memcpy(mptr, &material_gpu, sizeof(material_gpu));

                const cairns::rhi::DrawTmp draw_tmp {
                    .model_matrix = world_mat,
                };
                uint32_t drawtmp_offset = 0;
                void* tptr = rhi_.alloc.BumpAllocate(
                    sizeof(cairns::rhi::DrawTmp), rhi_.alloc.UboAlign(),
                    rhi::Memory::kDynamic, &drawtmp_offset);
                assert(tptr && "bump alloc failed: draw tmp");
                memcpy(tptr, &draw_tmp, sizeof(draw_tmp));

                cairns::Draw draw{};
                draw.bind_groups[1] = material_bind_groups_[mat_id];
                draw.index_buffer = index;
                draw.index_offset = index_base_off + (prim.first_index * sizeof(uint32_t));
                draw.vertex_offset = prim.vertex_offset;
                draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot] = pos;
                draw.vertex_buffers[cairns::Draw::kVertexBufferAttrSlot] = attr;
                draw.instance_offset = 0;
                draw.instance_count = 1;
                draw.dynamic_buffer_offsets[0] = material_offset;
                draw.dynamic_buffer_offsets[1] = drawtmp_offset;
                assert(prim.index_count % 3 == 0);
                draw.triangle_count = prim.index_count / 3;

                const glm::vec4 view_pos = view_matrix * world_mat[3];
                const float view_depth = -view_pos.z;
                const float d01 = glm::clamp(
                    (view_depth - near_z) / (far_z - near_z), 0.0f, 1.0f);
                const uint32_t depth_q =
                    static_cast<uint32_t>(d01 * float((1u << 24) - 1));
                drawListSorted_.emplace_back(
                    cairns::BuildDrawKey(mat_id & 0x3FFFFFFFu, depth_q,
                                         kMockTranslucency, kMockViewport,
                                         kMockViewportLayer, kMockFullscreenLayer),
                    drawList_.size());
                drawList_.push_back(draw);
            }
        }

        return true;
    }

    // Bump a RenderPassGlobals for one camera; return its dynamic offset.
    uint32_t UploadGlobals(const glm::mat4& view_proj, const glm::vec3& cam_pos,
                           float near_z, float w, float h) {
        cairns::rhi::RenderPassGlobals g{
            .view_proj = view_proj,
            .inv_view_proj = glm::inverse(view_proj),
            .camera_pos = glm::vec4(cam_pos, 1.0f),
            .camera_dir = glm::vec4(0.0f, 0.0f, -1.0f, near_z),
            .screen_params = glm::vec4(w, h, 1.0f / w, 1.0f / h)};
        uint32_t off = 0;
        void* p = rhi_.alloc.BumpAllocate(sizeof(g), rhi_.alloc.UboAlign(),
                                          rhi::Memory::kDynamic, &off);
        assert(p && "bump alloc failed: parallel globals");
        memcpy(p, &g, sizeof(g));
        return off;
    }

    // Build a draw list for a subset of world entities, depth-sorted for the
    // given camera. Mirrors BuildMeshOpaqueDraws over a filtered SceneWorld.
    void BuildSubsetDraws(std::span<const uint32_t> entity_indices,
                          const glm::mat4& view_matrix, const glm::mat4& root,
                          std::vector<cairns::Draw>& out_draws,
                          std::vector<std::pair<cairns::DrawKey, uint32_t>>& out_sorted) {
        out_draws.clear();
        out_sorted.clear();
        // Build a handle filter from the live indices (D's path: handles, not raw copies).
        std::vector<rhi::Handle<cairns::SceneEntity>> handles;
        handles.reserve(entity_indices.size());
        for (uint32_t ei : entity_indices) {
            if (ei < world_.live_entities.size()) {
                handles.push_back(world_.live_entities[ei]);
            }
        }
        cairns::Extract(world_, subset_proxies_, handles, &root);
        const float near_z = 0.1f;
        const float far_z = 100.0f;
        for (const cairns::MeshProxy& mp : subset_proxies_.meshes.data) {
            const BufHandle pos = mp.pos;
            const BufHandle attr = mp.attr;
            const BufHandle index = mp.index;
            const glm::mat4& world_mat = mp.world_matrix;
            const uint32_t index_base_off = rhi_.resources.BufferBaseOffset(rhi_.alloc, index);
            for (uint32_t p = 0; p < mp.primitive_count; ++p) {
                const cairns::PrimitiveProxy& prim = subset_proxies_.primitives[mp.first_primitive + p];
                const MatId mat_id = prim.material_id;
                const cairns::rhi::MaterialGpu material_gpu{};
                uint32_t material_offset = 0;
                void* mptr = rhi_.alloc.BumpAllocate(
                    sizeof(cairns::rhi::MaterialGpu), rhi_.alloc.UboAlign(),
                    rhi::Memory::kDynamic, &material_offset);
                assert(mptr && "bump alloc failed: subset material");
                memcpy(mptr, &material_gpu, sizeof(material_gpu));

                const cairns::rhi::DrawTmp draw_tmp{.model_matrix = world_mat};
                uint32_t drawtmp_offset = 0;
                void* tptr = rhi_.alloc.BumpAllocate(
                    sizeof(cairns::rhi::DrawTmp), rhi_.alloc.UboAlign(),
                    rhi::Memory::kDynamic, &drawtmp_offset);
                assert(tptr && "bump alloc failed: subset draw tmp");
                memcpy(tptr, &draw_tmp, sizeof(draw_tmp));

                cairns::Draw draw{};
                draw.bind_groups[1] = material_bind_groups_[mat_id];
                draw.index_buffer = index;
                draw.index_offset = index_base_off + (prim.first_index * sizeof(uint32_t));
                draw.vertex_offset = prim.vertex_offset;
                draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot] = pos;
                draw.vertex_buffers[cairns::Draw::kVertexBufferAttrSlot] = attr;
                draw.instance_offset = 0;
                draw.instance_count = 1;
                draw.dynamic_buffer_offsets[0] = material_offset;
                draw.dynamic_buffer_offsets[1] = drawtmp_offset;
                draw.triangle_count = prim.index_count / 3;

                const glm::vec4 view_pos = view_matrix * world_mat[3];
                const float view_depth = -view_pos.z;
                const float d01 = glm::clamp((view_depth - near_z) / (far_z - near_z),
                                             0.0f, 1.0f);
                const uint32_t depth_q = static_cast<uint32_t>(d01 * float((1u << 24) - 1));
                out_sorted.emplace_back(
                    cairns::BuildDrawKey(mat_id & 0x3FFFFFFFu, depth_q, kMockTranslucency,
                                         kMockViewport, kMockViewportLayer,
                                         kMockFullscreenLayer),
                    out_draws.size());
                out_draws.push_back(draw);
            }
        }
        std::sort(out_sorted.begin(), out_sorted.end());
    }

    // CAIRNS_RG_PARALLEL test: 3 cameras render disjoint GLB subsets into 3 frame
    // targets via independent graph branches (blur / depth / plain), composited
    // big-FT3 + PIP insets, UI on top. No threading -- the graph topo-sorts them.
    bool drawParallel(rhi::FrameContext& fc, bool draw_imgui) {
        const float fb_w = static_cast<float>(swapchain_.Width());
        const float fb_h = static_cast<float>(swapchain_.Height());
        const uint32_t w = swapchain_.Width();
        const uint32_t h = swapchain_.Height();
        const float aspect = fb_w / fb_h;
        const float near_z = 0.1f;
        const float far_z = 100.0f;
        const glm::vec3 eye(0, 0, 0);
        const glm::vec3 up(0, 1, 0);

        const glm::mat4 root =
            glm::rotate(glm::mat4(1.0f), glm::radians(render_angle_deg_), glm::vec3(0, 1, 0));

        // Models are placed as (entity.transform * root) -> they spin in place at
        // the fixed grid position, so the camera target is the entity translation.
        auto entity_center = [&](uint32_t i) -> glm::vec3 {
            const cairns::SceneEntity::Hot* hot =
                world_.entities.GetHot(world_.live_entities[i]);
            return glm::vec3(hot->transform[3]);
        };
        const glm::vec3 t0 = world_.live_entities.size() > 0 ? entity_center(0)
                                                             : glm::vec3(0, 0, -3);
        const glm::vec3 t1 = world_.live_entities.size() > 1 ? entity_center(1)
                                                             : glm::vec3(0, 0, -3);

        const glm::mat4 proj_wide = glm::perspectiveRH_ZO(glm::radians(90.0f), aspect, near_z, far_z);
        const glm::mat4 proj_narrow = glm::perspectiveRH_ZO(glm::radians(22.0f), aspect, near_z, far_z);
        const glm::mat4 view_c = glm::lookAtRH(eye, eye + glm::vec3(0, 0, -1), up);
        const glm::mat4 view_a = glm::lookAtRH(eye, t0, up);
        const glm::mat4 view_b = glm::lookAtRH(eye, t1, up);
        const glm::mat4 vp_a = proj_narrow * view_a;
        const glm::mat4 vp_b = proj_narrow * view_b;
        const glm::mat4 vp_c = proj_wide * view_c;
        const uint32_t off_a = UploadGlobals(vp_a, eye, near_z, fb_w, fb_h);
        const uint32_t off_b = UploadGlobals(vp_b, eye, near_z, fb_w, fb_h);
        const uint32_t off_c = UploadGlobals(vp_c, eye, near_z, fb_w, fb_h);

        // Smoke wire-up for allocator A: route this transient through the per-frame ring.
        std::vector<uint32_t, cairns::BumpStdAllocator<uint32_t>> e_all{
            cairns::BumpStdAllocator<uint32_t>(frame_arena_.Current())};
        e_all.reserve(world_.live_entities.size());
        for (uint32_t i = 0; i < world_.live_entities.size(); ++i) {
            e_all.push_back(i);
        }
        const uint32_t e_glb1[] = {0};
        const uint32_t e_glb2[] = {1};
        BuildSubsetDraws(e_glb1, view_a, root, glb1_draws_, glb1_sorted_);
        BuildSubsetDraws(e_glb2, view_b, root, glb2_draws_, glb2_sorted_);
        BuildSubsetDraws(e_all, view_c, root, all5_draws_, all5_sorted_);

        auto make_ml = [&](std::vector<cairns::Draw>& d,
                           std::vector<std::pair<cairns::DrawKey, uint32_t>>& s,
                           ShaderHandle pipe, uint32_t goff) {
            rhi::MeshDrawList ml{};
            ml.draws = std::span<const cairns::Draw>(d.data(), d.size());
            ml.sorted_draws =
                std::span<const std::pair<cairns::DrawKey, uint32_t>>(s.data(), s.size());
            ml.pipeline = pipe;
            ml.globals_offset = goff;
            ml.resident_textures = std::span<const rhi::Handle<rhi::Texture>>(
                resident_textures_.data(), resident_textures_.size());
            ml.resident_buffers =
                std::span<const rhi::Handle<rhi::Buffer>>(&mesh_master_handle_, 1);
            return ml;
        };

        const float clear[4] = {41.0f / 255.0f, 42.0f / 255.0f, 48.0f / 255.0f, 1.0f};
        auto color_desc = [&]() {
            rhi::GraphTextureDesc d{};
            d.width = w;
            d.height = h;
            d.format = rhi::Format::kBgra8Unorm;
            d.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled;
            return d;
        };
        auto depth_desc = [&](bool sampled) {
            rhi::GraphTextureDesc d{};
            d.width = w;
            d.height = h;
            d.format = rhi::Format::kD32F;
            d.usage = sampled ? (rhi::kTexUsageDepthTarget | rhi::kTexUsageSampled)
                              : rhi::kTexUsageDepthTarget;
            return d;
        };

        graph_.Reset();
        rhi::GraphTexture texA;
        rhi::GraphTexture texA_depth;
        rhi::GraphTexture ft1;
        rhi::GraphTexture depthB;
        rhi::GraphTexture ft2;
        rhi::GraphTexture ft3;
        rhi::GraphTexture ft3_depth;
        rhi::GraphTexture swap_tex;

        // Branch 1: cam A -> glb1 color (texA) -> blur -> FT1.
        graph_.AddPass(
            "geo_glb1", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                texA = b.CreateColorTarget(color_desc());
                texA_depth = b.CreateDepthTarget(depth_desc(false));
                b.AddColorOutput("texA", texA, rhi::LoadOp::kClear, clear);
                b.AddDepthOutput("texA_depth", texA_depth, rhi::LoadOp::kClear, 1.0f);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                cmd.DrawMeshes(rhi_.resources, rhi_.alloc,
                               make_ml(glb1_draws_, glb1_sorted_, unlit_offscreen_, off_a));
            });
        graph_.AddPass(
            "blur", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                ft1 = b.CreateColorTarget(color_desc());
                b.AddColorOutput("ft1", ft1, rhi::LoadOp::kClear, clear);
                b.AddAttachmentInput(texA);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources& res) {
                const rhi::Handle<rhi::Texture> t = res.Resolve(texA);
                cmd.DrawFullscreen(rhi_.resources, blur_, &t, 1, composite_sampler_);
            });

        // Branch 2: cam B -> glb2 depth-only (depthB) -> depthviz -> FT2.
        graph_.AddPass(
            "geo_glb2", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                depthB = b.CreateDepthTarget(depth_desc(true));
                b.AddDepthOutput("depthB", depthB, rhi::LoadOp::kClear, 1.0f);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                cmd.DrawMeshes(rhi_.resources, rhi_.alloc,
                               make_ml(glb2_draws_, glb2_sorted_, depth_only_, off_b));
            });
        graph_.AddPass(
            "depthviz", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                ft2 = b.CreateColorTarget(color_desc());
                b.AddColorOutput("ft2", ft2, rhi::LoadOp::kClear, clear);
                b.AddAttachmentInput(depthB);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources& res) {
                const rhi::Handle<rhi::Texture> t = res.Resolve(depthB);
                cmd.DrawFullscreen(rhi_.resources, depthviz_, &t, 1, composite_sampler_);
            });

        // Branch 3: cam C -> all 5 glbs -> FT3.
        graph_.AddPass(
            "geo_5glb", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                ft3 = b.CreateColorTarget(color_desc());
                ft3_depth = b.CreateDepthTarget(depth_desc(false));
                b.AddColorOutput("ft3", ft3, rhi::LoadOp::kClear, clear);
                b.AddDepthOutput("ft3_depth", ft3_depth, rhi::LoadOp::kClear, 1.0f);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                cmd.DrawMeshes(rhi_.resources, rhi_.alloc,
                               make_ml(all5_draws_, all5_sorted_, unlit_offscreen_, off_c));
            });

        // Converge: composite FT3 (full) + FT1/FT2 (PIP) + UI -> swapchain.
        graph_.AddPass(
            "composite3", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                rhi::GraphTextureDesc td{};
                td.width = w;
                td.height = h;
                swap_tex = b.ImportTexture(rhi::Handle<rhi::Texture>::Null, td);
                b.AddColorOutput("swapchain", swap_tex, rhi::LoadOp::kClear, clear);
                b.AddAttachmentInput(ft3);
                b.AddAttachmentInput(ft1);
                b.AddAttachmentInput(ft2);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources& res) {
                const rhi::Handle<rhi::Texture> texs[3] = {
                    res.Resolve(ft3), res.Resolve(ft1), res.Resolve(ft2)};
                cmd.DrawFullscreen(rhi_.resources, composite3_, texs, 3, composite_sampler_);
                if (draw_imgui) {
                    cmd.DrawImGui(rhi_.resources, rhi_.alloc, imgui_, imgui_font_,
                                  composite_sampler_, ImGui::GetDrawData());
                }
            });
        graph_.SetOutput(swap_tex);
        return graph_.Bake() && graph_.Execute(fc, swapchain_);
    }

    bool draw() {
        frame_++;
        const uint64_t cpu_now_ns = cairns::timestamp_ns();
        if (cpu_last_frame_ns_ != 0) {
            cpu_ms_last_ = static_cast<float>(cpu_now_ns - cpu_last_frame_ns_) / 1.0e6f;
            cpu_ms_history_[cpu_ms_head_] = cpu_ms_last_;
            cpu_ms_head_ = (cpu_ms_head_ + 1) % kCpuMsHistory;
        }
        cpu_last_frame_ns_ = cpu_now_ns;

        // Fixed-timestep accumulator (Fiedler). The sim advances in kFixedDt steps;
        // wall time only decides how many steps run this frame. alpha drives cheap
        // render-side interpolation of the rotation (no GPU interp for particles).
        accumulator_ += clock_->Tick();
        sim_steps_this_frame_ = 0;
        while (accumulator_ >= kFixedDt && sim_steps_this_frame_ < kMaxStepsPerFrame) {
            sim_angle_deg_ += kRotDegPerSec * kFixedDt;
            ++sim_frame_;
            ++sim_steps_this_frame_;
            accumulator_ -= kFixedDt;
        }
        if (sim_steps_this_frame_ == kMaxStepsPerFrame && accumulator_ >= kFixedDt) {
            accumulator_ = 0.0f;
        }
        const float alpha = accumulator_ / kFixedDt;
        render_angle_deg_ = sim_angle_deg_ + alpha * kRotDegPerSec * kFixedDt;

        if (golden_ && sim_frame_ == kGoldenDumpFrame) {
            rhi_.frames.SetDumpPath(std::getenv("CAIRNS_DUMP"));
            fprintf(stderr, "[GOLDEN] dump at sim_frame=%u\n", sim_frame_);
        }
        if (golden_ && sim_frame_ > kGoldenDumpFrame) {
            std::exit(0);
        }

        cairns::Timer t_frame("frame", 0);

        rhi::FrameContext fc = rhi_.frames.Begin(rhi_.resources, rhi_.alloc);
        frame_arena_.BeginFrame(frame_);

        cairns::Timer t_build("build_draws", 1);
        if ( !BuildMeshOpaqueDraws()) {
            return false;
        }
        t_build.End();
        {
            std::sort(drawListSorted_.begin(), drawListSorted_.end());
        }
        resident_textures_.clear();
        for (auto& s : scenes_) {
            for (const auto th : s.textureHandles) {
                resident_textures_.push_back(th);
            }
        }

        cairns::Timer t_record("record", 2);

        rhi::MeshDrawList ml{};
        ml.draws = std::span<const cairns::Draw>(drawList_.data(), drawList_.size());
        ml.sorted_draws = std::span<const std::pair<cairns::DrawKey, uint32_t>>(
            drawListSorted_.data(), drawListSorted_.size());
        ml.pipeline = unlit_;
        ml.globals_offset = globals_offset_;
        ml.resident_textures = std::span<const rhi::Handle<rhi::Texture>>(
            resident_textures_.data(), resident_textures_.size());
        ml.resident_buffers =
            std::span<const rhi::Handle<rhi::Buffer>>(&mesh_master_handle_, 1);

        const float clear[4] = {41.0f / 255.0f, 42.0f / 255.0f, 48.0f / 255.0f, 1.0f};
        const uint32_t fb_w = swapchain_.Width();
        const uint32_t fb_h = swapchain_.Height();

        // Overlay is non-deterministic (fps text changes per frame) -> skip it in
        // golden capture so the byte-gate dump stays reproducible.
        const bool parallel = std::getenv("CAIRNS_RG_PARALLEL") != nullptr;
        const bool draw_imgui = !golden_;
        if (draw_imgui) {
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            // Parallel test has insets in the top corners -> park the overlay low.
            if (parallel) {
                const float disp_h = ImGui::GetIO().DisplaySize.y;
                ImGui::SetNextWindowPos(ImVec2(20.0f, disp_h - 150.0f), ImGuiCond_Always);
            } else {
                ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_FirstUseEver);
            }
            ImGui::Begin("cairns", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            float ms_max = 1.0f;
            float ms_avg = 0.0f;
            for (int i = 0; i < kCpuMsHistory; ++i) {
                ms_max = cpu_ms_history_[i] > ms_max ? cpu_ms_history_[i] : ms_max;
                ms_avg += cpu_ms_history_[i];
            }
            ms_avg /= static_cast<float>(kCpuMsHistory);
            const float fps_avg = ms_avg > 0.0f ? 1000.0f / ms_avg : 0.0f;
            ImGui::Text("CPU %.2f ms   |   %.0f FPS  (avg/120f)", ms_avg, fps_avg);
            ImGui::Text("peak %.2f ms", ms_max);
            ImGui::Text("draws %zu", drawList_.size());
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%.2f ms", ms_avg);
            ImGui::PlotLines("##cpuhist", cpu_ms_history_, kCpuMsHistory, cpu_ms_head_,
                             overlay, 0.0f, ms_max * 1.15f, ImVec2(300.0f, 110.0f));
            ImGui::End();
            ImGui::Render();
        }

        if (parallel) {
            if (!drawParallel(fc, draw_imgui)) {
                return false;
            }
            t_record.End();
            rhi_.frames.End(swapchain_, fc);
            t_frame.End();
            return true;
        }

        cairns::Timer t_rg_build("render graph build", 6);
        graph_.Reset();
        rhi::GraphTexture depth_tex;
        rhi::GraphTexture color_tex;
        rhi::GraphTexture fwd_depth;
        rhi::GraphTexture swap_tex;
        rhi::GraphBuffer sim_ssbo;
        // Particle sim: advance sim_steps_this_frame_ fixed-dt steps (0 at high
        // refresh, 1 in golden). Ping-pong the SSBOs; leave particle_parity_ on the
        // freshest. ReadBuffer in composite orders this before the point draw.
        graph_.AddPass(
            "particle_sim", rhi::PassType::kCompute,
            [&](rhi::PassBuilder& b) {
                rhi::GraphBufferDesc bd{};
                bd.usage = rhi::kUsageStorage;
                sim_ssbo = b.ImportBuffer(particle_ssbo_[0], bd);
                b.WriteBuffer(sim_ssbo);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                uint32_t dt_off = 0;
                float* dt_ptr = static_cast<float*>(rhi_.alloc.BumpAllocate(
                    sizeof(float), rhi_.alloc.UboAlign(), rhi::Memory::kDynamic, &dt_off));
                assert(dt_ptr && "bump alloc failed: particle dt");
                *dt_ptr = kFixedDt;
                const rhi::Handle<rhi::Buffer> dt_master =
                    rhi_.alloc.BumpMasterBuffer(rhi::Memory::kDynamic);
                uint32_t cur = particle_parity_;
                for (uint32_t s = 0; s < sim_steps_this_frame_; ++s) {
                    const rhi::BoundBuffer cbufs[3] = {
                        {0, dt_master, dt_off},
                        {1, particle_ssbo_[cur], 0},
                        {2, particle_ssbo_[1 - cur], 0},
                    };
                    rhi::ComputeDispatch cd{};
                    cd.kernel = particle_kernel_;
                    cd.buffers = std::span<const rhi::BoundBuffer>(cbufs, 3);
                    cd.groups_x = kParticleCount / 256;
                    cd.local_x = 256;
                    cmd.Dispatch(rhi_.resources, rhi_.alloc, cd);
                    cur ^= 1;
                }
                particle_parity_ = cur;
            });
        graph_.AddPass(
            "depth_prepass", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                rhi::GraphTextureDesc dd{};
                dd.width = fb_w;
                dd.height = fb_h;
                dd.format = rhi::Format::kD32F;
                dd.usage = rhi::kTexUsageDepthTarget | rhi::kTexUsageSampled;
                depth_tex = b.CreateDepthTarget(dd);
                b.AddDepthOutput("depth", depth_tex, rhi::LoadOp::kClear, 1.0f);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                rhi::MeshDrawList dl = ml;
                dl.pipeline = depth_only_;
                cmd.DrawMeshes(rhi_.resources, rhi_.alloc, dl);
            });
        graph_.AddPass(
            "forward", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                rhi::GraphTextureDesc cd{};
                cd.width = fb_w;
                cd.height = fb_h;
                cd.format = rhi::Format::kBgra8Unorm;
                cd.usage = rhi::kTexUsageColorTarget | rhi::kTexUsageSampled;
                color_tex = b.CreateColorTarget(cd);
                rhi::GraphTextureDesc dd{};
                dd.width = fb_w;
                dd.height = fb_h;
                dd.format = rhi::Format::kD32F;
                dd.usage = rhi::kTexUsageDepthTarget;
                fwd_depth = b.CreateDepthTarget(dd);
                b.AddColorOutput("color", color_tex, rhi::LoadOp::kClear, clear);
                b.AddDepthOutput("fwd_depth", fwd_depth, rhi::LoadOp::kClear, 1.0f);
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources&) {
                rhi::MeshDrawList dl = ml;
                dl.pipeline = unlit_offscreen_;
                cmd.DrawMeshes(rhi_.resources, rhi_.alloc, dl);
            });
        graph_.AddPass(
            "composite", rhi::PassType::kGraphics,
            [&](rhi::PassBuilder& b) {
                rhi::GraphTextureDesc td{};
                td.width = fb_w;
                td.height = fb_h;
                swap_tex = b.ImportTexture(rhi::Handle<rhi::Texture>::Null, td);
                b.AddColorOutput("swapchain", swap_tex, rhi::LoadOp::kClear, clear);
                b.AddAttachmentInput(color_tex);
                b.AddAttachmentInput(depth_tex);
                b.ReadBuffer(sim_ssbo);  // order particle_sim before the point draw
            },
            [&](rhi::CommandRecorder& cmd, const rhi::PassResources& res) {
                const rhi::Handle<rhi::Texture> texs[2] = {res.Resolve(color_tex),
                                                           res.Resolve(depth_tex)};
                cmd.DrawFullscreen(rhi_.resources, composite_, texs, 2,
                                   composite_sampler_);
                rhi::PointDraw pd{};
                pd.pipeline = particle_render_shader_;
                pd.vertex_buffer = particle_ssbo_[particle_parity_];
                pd.vertex_offset = 0;
                pd.vertex_count = kParticleCount;
                cmd.DrawPoints(rhi_.resources, rhi_.alloc, pd);
                if (draw_imgui) {
                    cmd.DrawImGui(rhi_.resources, rhi_.alloc, imgui_, imgui_font_,
                                  composite_sampler_, ImGui::GetDrawData());
                }
            });
        graph_.SetOutput(swap_tex);
        if (!graph_.Bake()) {
            return false;
        }
        t_rg_build.End();
        cairns::Timer t_rg_exec("render graph execute", 7);
        if (!graph_.Execute(fc, swapchain_)) {
            return false;
        }
        t_rg_exec.End();
        t_record.End();
        rhi_.frames.End(swapchain_, fc);
        t_frame.End();
        if (frame_ % 120 == 0) {
            printf("draws: %zu\n", drawList_.size());
            cairns::Timer::PrintReport();
            cairns::Timer::Reset();
        }
        return true;
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

            // depth-only prepass: offscreen, single-sample, no color attachment.
            rhi::GraphicsPipelineDesc dpd = desc;
            dpd.logical_shader = "depth_only";
            dpd.color_format = rhi::Format::kUndefined;
            dpd.depth_format = rhi::Format::kD32F;
            dpd.depth_test = true;
            dpd.depth_write = true;
            dpd.depth_compare = rhi::CompareOp::kLess;
            dpd.sample_count = 1;
            dpd.debug_name = "depth_only";
            dpd.swap_chain = nullptr;
            depth_only_ =
                rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, dpd);

            // forward color: offscreen, single-sample (composited later).
            rhi::GraphicsPipelineDesc ofd = desc;
            ofd.logical_shader = "unlit";
            ofd.color_format = rhi::Format::kBgra8Unorm;
            ofd.depth_format = rhi::Format::kD32F;
            ofd.sample_count = 1;
            ofd.debug_name = "unlit_offscreen";
            ofd.swap_chain = nullptr;
            unlit_offscreen_ =
                rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, ofd);

            // composite: fullscreen tri to the swapchain (MSAA), samples color+depth.
            rhi::GraphicsPipelineDesc cpd{};
            cpd.logical_shader = "composite";
            cpd.shader_dir = shader_dir.c_str();
            cpd.topology = rhi::PrimitiveTopology::kTriangleList;
            cpd.cull = rhi::CullMode::kNone;
            cpd.depth_test = false;
            cpd.depth_write = false;
            cpd.color_format = rhi::Format::kBgra8Unorm;
            cpd.depth_format = rhi::Format::kD32F;
            cpd.sample_count = sampleCount;
            cpd.debug_name = "composite";
            cpd.swap_chain = &swapchain_;
            composite_ =
                rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, cpd);

            // Fullscreen post pipelines for the parallel test: blur + depthviz are
            // offscreen single-sample; composite3 targets the swapchain (MSAA).
            rhi::GraphicsPipelineDesc fd{};
            fd.shader_dir = shader_dir.c_str();
            fd.topology = rhi::PrimitiveTopology::kTriangleList;
            fd.cull = rhi::CullMode::kNone;
            fd.depth_test = false;
            fd.depth_write = false;
            fd.color_format = rhi::Format::kBgra8Unorm;
            fd.depth_format = rhi::Format::kUndefined;
            fd.sample_count = 1;
            fd.swap_chain = nullptr;
            fd.logical_shader = "blur";
            fd.debug_name = "blur";
            blur_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, fd);
            fd.logical_shader = "depthviz";
            fd.debug_name = "depthviz";
            depthviz_ =
                rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, fd);

            rhi::GraphicsPipelineDesc c3d{};
            c3d.logical_shader = "composite3";
            c3d.shader_dir = shader_dir.c_str();
            c3d.topology = rhi::PrimitiveTopology::kTriangleList;
            c3d.cull = rhi::CullMode::kNone;
            c3d.depth_test = false;
            c3d.depth_write = false;
            c3d.color_format = rhi::Format::kBgra8Unorm;
            c3d.depth_format = rhi::Format::kD32F;
            c3d.sample_count = sampleCount;
            c3d.debug_name = "composite3";
            c3d.swap_chain = &swapchain_;
            composite3_ =
                rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.frames, c3d);

            if (depth_only_.IsNull() || unlit_offscreen_.IsNull() ||
                composite_.IsNull() || blur_.IsNull() || depthviz_.IsNull() ||
                composite3_.IsNull()) {
                std::exit(0);
            }

            rhi::SamplerDesc sd{};
            sd.min_filter = rhi::Filter::kLinear;
            sd.mag_filter = rhi::Filter::kLinear;
            sd.mip_filter = rhi::Filter::kLinear;
            sd.address_mode = rhi::AddressMode::kClampToEdge;
            composite_sampler_ = rhi_.resources.CreateSampler(sd);

            // imgui pipeline: swapchain (MSAA), alpha blend, no depth.
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
                std::exit(0);
            }

            // imgui font atlas -> sampled texture (ImGui context created in main).
            ImGuiIO& io = ImGui::GetIO();
            unsigned char* pixels = nullptr;
            int fw = 0;
            int fh = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
            rhi::TextureDesc ftd{};
            ftd.dimensions = {fw, fh, 1};
            ftd.format = rhi::Format::kRgba8Unorm;
            ftd.usage = rhi::kTexUsageSampled;
            ftd.initial_data = std::span<const uint8_t>(
                pixels, static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4);
            ftd.debug_name = "imgui_font";
            imgui_font_ = rhi_.resources.CreateTexture(rhi_.alloc, ftd);
            io.Fonts->SetTexID(static_cast<ImTextureID>(1));
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
        }

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
        swapchain_.Deinit();
        rhi_.pipelines.Deinit(rhi_.resources);
        rhi_.frames.Deinit();
        rhi_.resources.Deinit();
        rhi_.alloc.Deinit();
        rhi_.device.Deinit();
        // Return the FrameArena slab to B before its destructor runs.
        if (frame_arena_slab_ != nullptr) {
            hot_arena_.Free(frame_arena_slab_);
            frame_arena_slab_ = nullptr;
        }
        return true;
    }
    
private:
    // todo @iamies
    // make an engine dtor and delete this
    cairns::Arena hot_arena_;
    // Per-frame transient ring (A). Slab borrowed from hot_arena_ (B).
    uint8_t* frame_arena_slab_ = nullptr;
    cairns::FrameArena frame_arena_;
    
    ////////// DO NOT MOVE ARENA BELOW THIS LINE. because c++.
    
    uint32_t frame_ = 0;

    std::vector<cairns::Scene, cairns::Allocator<cairns::Scene>> scenes_;
    std::vector<int32_t, cairns::Allocator<int32_t>> root_nodes_stack_cache_;

    std::vector<glm::mat4> debugSceneXforms_;

    std::vector<cairns::LoadedMaterial> materials_;
    // set-2 per-material bind groups, dense by MatId (NO hash). Built once at load.
    std::vector<rhi::Handle<rhi::BindGroup>> material_bind_groups_;

    std::vector<std::pair<cairns::DrawKey,uint32_t>,cairns::Allocator<std::pair<cairns::DrawKey,uint32_t>>> drawListSorted_;
    std::vector<cairns::Draw,cairns::Allocator<cairns::Draw>> drawList_;
    std::vector<rhi::Handle<rhi::Texture>> resident_textures_;

    cairns::SceneWorld world_;
    cairns::RenderProxyArrays proxies_;
    // CAIRNS_RG_PARALLEL: scratch + per-branch subset draw lists.
    cairns::RenderProxyArrays subset_proxies_;
    std::vector<cairns::Draw> glb1_draws_;
    std::vector<cairns::Draw> glb2_draws_;
    std::vector<cairns::Draw> all5_draws_;
    std::vector<std::pair<cairns::DrawKey, uint32_t>> glb1_sorted_;
    std::vector<std::pair<cairns::DrawKey, uint32_t>> glb2_sorted_;
    std::vector<std::pair<cairns::DrawKey, uint32_t>> all5_sorted_;

    rhi::Rhi rhi_;
    rhi::Handle<rhi::Buffer> mesh_master_handle_ = rhi::Handle<rhi::Buffer>::Null;
    rhi::RenderGraph graph_{rhi_.resources, rhi_.alloc};

    cairns::rhi::SwapChain swapchain_;
    // shaders
    ShaderHandle unlit_ = ShaderHandle::Null;
    ShaderHandle depth_only_ = ShaderHandle::Null;
    ShaderHandle unlit_offscreen_ = ShaderHandle::Null;
    ShaderHandle composite_ = ShaderHandle::Null;
    ShaderHandle blur_ = ShaderHandle::Null;
    ShaderHandle depthviz_ = ShaderHandle::Null;
    ShaderHandle composite3_ = ShaderHandle::Null;
    ShaderHandle imgui_ = ShaderHandle::Null;
    rhi::Handle<rhi::Sampler> composite_sampler_ = rhi::Handle<rhi::Sampler>::Null;
    rhi::Handle<rhi::Texture> imgui_font_ = rhi::Handle<rhi::Texture>::Null;
    // particles
    static constexpr uint32_t kParticleCount = 512;
    rhi::Handle<rhi::Kernel> particle_kernel_;
    rhi::Handle<rhi::Shader> particle_render_shader_;
    rhi::Handle<rhi::Buffer> particle_ssbo_[2];
    uint32_t particle_parity_ = 0;
    uint32_t globals_offset_ = 0;
    // cpu frame-time history (wall-clock between draw() calls) for the imgui graph
    static constexpr int kCpuMsHistory = 120;
    float cpu_ms_history_[kCpuMsHistory] = {};
    int cpu_ms_head_ = 0;
    float cpu_ms_last_ = 0.0f;
    uint64_t cpu_last_frame_ns_ = 0;
    // Fixed-timestep sim clock (FixedClock under CAIRNS_DUMP, else WallClock).
    static constexpr float kFixedDt = 1.0f / 60.0f;
    static constexpr int kMaxStepsPerFrame = 5;
    static constexpr float kRotDegPerSec = 22.5f;  // old 45deg/2s
    static constexpr uint32_t kGoldenDumpFrame = 60;
    std::unique_ptr<cairns::FrameClock> clock_;
    bool golden_ = false;
    float accumulator_ = 0.0f;
    uint32_t sim_frame_ = 0;
    uint32_t sim_steps_this_frame_ = 0;
    float sim_angle_deg_ = 0.0f;
    float render_angle_deg_ = 0.0f;
    // render pass
    static constexpr size_t sampleCount = 4;
};

} // namespace cairns

