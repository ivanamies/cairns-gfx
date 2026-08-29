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
#include "rhi/rhi.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/command_recorder.hpp"

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
    
    Engine() :
    hot_arena_mem_(malloc(kHotArenaMemorySize)),
    hot_arena_(hot_arena_mem_, kHotArenaMemorySize),
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
        return true;
    }
    
    bool initResourceManagers() {
        using namespace cairns;
        using namespace cairns::rhi;
        // 4 because we're only pretending to be a real UGC engine at this point
        return true;
    }
    
    bool GreaterInit(SDL_Window* window) {
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
        if (!rhi_.bindless.Init(rhi_.device)) {
            CAIRNS_PRINT("GreaterInit: bindless.Init failed\n");
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

                if (!cairns::rhi::LoadSceneGpu(scene, rhi_.resources, rhi_.alloc)) {
                    return false;
                }

                scene.CleanupTmps();
            }
        }
        if (!scenes_.empty() && !scenes_[0].meshes.empty()) {
            mesh_master_handle_ = scenes_[0].meshes[0].posHandle;
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

        return true;
    }

    bool BuildMeshOpaqueDraws() {
        drawList_.clear();
        drawListSorted_.clear();
        
        const char* freeze_rot = std::getenv("CAIRNS_FREEZE_ROT");
        const float angle_degs = freeze_rot
                                     ? static_cast<float>(std::atof(freeze_rot))
                                     : (SDL_GetTicks() / 1000.0 / 2.0 * 45);
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
        for ( size_t scene_xform_idx = 0; scene_xform_idx < debugSceneXforms_.size(); ++scene_xform_idx ) {
            size_t scene_idx = scene_xform_idx % scenes_.size();
            const glm::mat4& scene_xform = debugSceneXforms_[scene_xform_idx];
            cairns::Scene& scene = scenes_[scene_idx];
            root_nodes_stack_cache_.clear();
            for ( size_t j = 0; j < scene.rootNodes.size(); ++j ) {
                root_nodes_stack_cache_.push_back(scene.rootNodes[j]);
            }
            while(!root_nodes_stack_cache_.empty()) {
                int32_t nodeIdx = root_nodes_stack_cache_.back();
                root_nodes_stack_cache_.pop_back();
                const auto& node = scene.nodes[nodeIdx];
                if ( node.meshIndex < 0 ) {
                    for (int32_t c : node.children) {
                        root_nodes_stack_cache_.push_back(c);
                    }
                    continue;
                }

                const auto& mesh = scene.meshes[node.meshIndex];
                const BufHandle pos = mesh.posHandle;
                [[maybe_unused]] const BufHandle attr = mesh.attrHandle;
                const BufHandle index = mesh.indexHandle;
                
                for(const auto& prim : mesh.primitives) {
                    const uint32_t scene_mat_idx = prim.materialIndex;
                    const MatId mat_id = scene.materialIds[scene_mat_idx];
                    const rhi::Handle<rhi::Texture> tex_handle = materials_[mat_id].color;
                    const SamplerHandle sampler_handle = materials_[mat_id].sampler;
                    
                    const uint32_t gpu_tex_id = texture_id_map_[tex_handle.index];
                    const uint32_t gpu_sampler_id = sampler_id_map_[sampler_handle.index];
                    const uint32_t gpu_attr_idx = mesh_attr_id_map_[mesh.attrHandle.index];
                    
                    const cairns::rhi::MaterialGpu material_gpu {
                        .tex_color_id = gpu_tex_id,
                        .sampler_id = gpu_sampler_id,
                    };
                    uint32_t material_offset = 0;
                    void* mptr = rhi_.alloc.BumpAllocate(
                        sizeof(cairns::rhi::MaterialGpu), rhi_.alloc.UboAlign(),
                        rhi::Memory::kDynamic, &material_offset);
                    assert(mptr && "bump alloc failed: material");
                    memcpy(mptr, &material_gpu, sizeof(material_gpu));

                    // todo @iamies not a good idea
                    const glm::mat4 model_matrix = scene_xform * rot_matrix;
                    const glm::mat4 world_mat = node.globalTransform * model_matrix;
                    const cairns::rhi::DrawTmp draw_tmp {
                        .model_matrix = world_mat,
                        .mesh_id = gpu_attr_idx,
                        .tex_id = gpu_tex_id,
                        .sampler_id = gpu_sampler_id
                    };
                    uint32_t drawtmp_offset = 0;
                    void* tptr = rhi_.alloc.BumpAllocate(
                        sizeof(cairns::rhi::DrawTmp), rhi_.alloc.UboAlign(),
                        rhi::Memory::kDynamic, &drawtmp_offset);
                    assert(tptr && "bump alloc failed: draw tmp");
                    memcpy(tptr, &draw_tmp, sizeof(draw_tmp));

                    cairns::Draw draw{};
                    draw.index_buffer = index;
                    const uint32_t index_base_off = rhi_.resources.BufferBaseOffset(rhi_.alloc, index);
                    draw.index_offset = index_base_off + (prim.firstIndex * sizeof(uint32_t));
                    draw.vertex_offset = prim.vertexOffset;
                    draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot] = pos;
                    draw.instance_offset = 0;
                    draw.instance_count = 1;
                    draw.dynamic_buffer_offsets[0] = material_offset;
                    draw.dynamic_buffer_offsets[1] = drawtmp_offset;
                    assert(prim.indexCount % 3 == 0);
                    draw.triangle_count = prim.indexCount / 3;

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
                for(int32_t c : node.children) {
                    root_nodes_stack_cache_.push_back(c);
                }
            }
        }
        
        return true;
    }
    
    bool draw() {
        frame_++;
        if (frame_ == 5) {
            rhi_.frames.SetDumpPath("/tmp/cairns_dump.png");
        }

        cairns::Timer t_frame("frame", 0);

        rhi::FrameContext fc = rhi_.frames.Begin(rhi_.resources, rhi_.alloc, swapchain_);

        const uint64_t now_ticks = SDL_GetTicks();
        float delta_time = 0.016f;
#if !defined(CAIRNS_FREEZE_ROT) || !CAIRNS_FREEZE_ROT
        if (last_ticks_ > 0) {
            delta_time = static_cast<float>(now_ticks - last_ticks_) / 1000.0f;
        }
#endif
        last_ticks_ = now_ticks;

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
        uint32_t dt_off = 0;
        float* dt_ptr = static_cast<float*>(
            rhi_.alloc.BumpAllocate(sizeof(float), rhi_.alloc.UboAlign(),
                                    rhi::Memory::kDynamic, &dt_off));
        assert(dt_ptr && "bump alloc failed: delta time");
        *dt_ptr = delta_time;

        rhi::BoundBuffer cbufs[3] = {
            {0, rhi_.alloc.BumpMasterBuffer(rhi::Memory::kDynamic), dt_off},
            {1, particle_ssbo_[particle_parity_], 0},
            {2, particle_ssbo_[1 - particle_parity_], 0},
        };
        rhi::ComputeDispatch cd{};
        cd.kernel = particle_kernel_;
        cd.buffers = std::span<const rhi::BoundBuffer>(cbufs, 3);
        cd.groups_x = kParticleCount / 256;
        cd.local_x = 256;
        fc.cmd.Dispatch(rhi_.resources, rhi_.alloc, cd);

        rhi::ColorAttachment col[1]{};
        col[0].clear[0] = 41.0f / 255.0f;
        col[0].clear[1] = 42.0f / 255.0f;
        col[0].clear[2] = 48.0f / 255.0f;
        col[0].clear[3] = 1.0f;
        rhi::RenderPassDesc rp{};
        rp.color = std::span<const rhi::ColorAttachment>(col, 1);
        rp.depth.clear_depth = 1.0f;
        rp.width = swapchain_.Width();
        rp.height = swapchain_.Height();
        fc.cmd.BeginRenderPass(swapchain_, rp);

        rhi::MeshDrawList ml{};
        ml.draws = std::span<const cairns::Draw>(drawList_.data(), drawList_.size());
        ml.sorted_draws = std::span<const std::pair<cairns::DrawKey, uint32_t>>(
            drawListSorted_.data(), drawListSorted_.size());
        ml.pipeline = unlit_;
        ml.bindless = bindless_bg_;
        ml.globals_offset = globals_offset_;
        ml.resident_textures = std::span<const rhi::Handle<rhi::Texture>>(
            resident_textures_.data(), resident_textures_.size());
        ml.resident_buffers =
            std::span<const rhi::Handle<rhi::Buffer>>(&mesh_master_handle_, 1);
        fc.cmd.DrawMeshes(rhi_.resources, rhi_.alloc, ml);

        rhi::PointDraw pd{};
        pd.pipeline = particle_render_shader_;
        pd.vertex_buffer = particle_ssbo_[1 - particle_parity_];
        pd.vertex_offset = 0;
        pd.vertex_count = kParticleCount;
        fc.cmd.DrawPoints(rhi_.resources, rhi_.alloc, pd);

        fc.cmd.EndRenderPass();
        t_record.End();
        rhi_.frames.End(swapchain_, fc);
        particle_parity_ ^= 1;
        t_frame.End();
        if (frame_ % 120 == 0) {
            printf("draws: %zu\n", drawList_.size());
            cairns::Timer::PrintReport();
            cairns::Timer::Reset();
        }
        return true;
    }
    
    bool initRenderPipeline() {
        // Bindless registry FIRST: on Vulkan the unlit pipeline layout references
        // the bindless descriptor-set layout, which CreateBindlessRegistry creates.
        // (Metal pipelines don't reference it; order is byte-neutral there.)
        { // bindless resources set up via rhi
            using R = cairns::rhi::GpuSceneRegistry;
            rhi::BindlessRegistryDesc rdesc{};
            rdesc.max_textures = R::kMaxTextures;
            rdesc.max_attr_buffers = R::kMaxMeshes;
            rdesc.max_samplers = R::kMaxSamplers;
            rdesc.texture_slot = R::kTextureRegistrySlot;
            rdesc.attr_buffer_slot = R::kAttrBufferRegistrySlot;
            rdesc.sampler_slot = R::kSamplerRegistrySlot;
            rdesc.debug_name = "bindless";
            bindless_bg_ = rhi_.bindless.CreateRegistry(rhi_.resources, rhi_.alloc, rdesc);

            texture_id_map_.clear();
            mesh_attr_id_map_.clear();
            sampler_id_map_.clear();

            for (size_t i = 0; i < scenes_.size(); ++i) {
                cairns::Scene& scene = scenes_[i];
                for (size_t j = 0; j < scene.textureHandles.size(); ++j) {
                    auto h = scene.textureHandles[j];
                    if (h.index >= texture_id_map_.size()) {
                        texture_id_map_.resize(h.index + 1, 0);
                    }
                    rhi::Texture::Hot* hot = rhi_.resources.GetHot(h);
                    if (hot && hot->api_view) {
                        texture_id_map_[h.index] =
                            rhi_.bindless.AddTexture(rhi_.resources, bindless_bg_, h);
                    }
                }
                for (size_t j = 0; j < scene.meshes.size(); ++j) {
                    auto h = scene.meshes[j].attrHandle;
                    if (!h.IsNull()) {
                        if (h.index >= mesh_attr_id_map_.size()) {
                            mesh_attr_id_map_.resize(h.index + 1, 0);
                        }
                        mesh_attr_id_map_[h.index] =
                            rhi_.bindless.AddAttrBuffer(rhi_.resources, rhi_.alloc, bindless_bg_, h);
                    }
                }
                for (size_t j = 0; j < scene.samplerHandles.size(); ++j) {
                    auto h = scene.samplerHandles[j];
                    if (h.index >= sampler_id_map_.size()) {
                        sampler_id_map_.resize(h.index + 1, 0);
                    }
                    sampler_id_map_[h.index] =
                        rhi_.bindless.AddSampler(rhi_.resources, bindless_bg_, h);
                }
            }

            rhi_.bindless.Finalize(bindless_bg_);
        }

        {  // unlit graphics pipeline via rhi
            const char* base = SDL_GetBasePath();
            const std::string shader_dir = base ? base : "";
            const rhi::VertexInputAttribute pos_attr{
                0, cairns::kMeshPosBindSlot, rhi::Format::kRgba32F, 0};
            const rhi::VertexBufferLayout pos_layout{
                cairns::kMeshPosBindSlot, static_cast<uint32_t>(sizeof(glm::vec4))};
            rhi::GraphicsPipelineDesc desc{};
            desc.logical_shader = "unlit";
            desc.shader_dir = shader_dir.c_str();
            desc.vertex_attributes =
                std::span<const rhi::VertexInputAttribute>(&pos_attr, 1);
            desc.vertex_buffers =
                std::span<const rhi::VertexBufferLayout>(&pos_layout, 1);
            desc.topology = rhi::PrimitiveTopology::kTriangleList;
            desc.cull = rhi::CullMode::kBack;
            desc.front_face = rhi::FrontFace::kCounterClockwise;
            desc.depth_test = true;
            desc.depth_write = true;
            desc.depth_compare = rhi::CompareOp::kLess;
            desc.color_format = rhi::Format::kBgra8Unorm;
            desc.depth_format = rhi::Format::kD32F;
            desc.sample_count = sampleCount;
            desc.push_constant_bytes = sizeof(uint32_t);
            desc.debug_name = "unlit";
            desc.swap_chain = &swapchain_;
            unlit_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.bindless, rhi_.frames, desc);
            if (unlit_.IsNull()) {
                std::exit(0);
            }
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
            particle_render_shader_ = rhi_.pipelines.CreateGraphicsPipeline(rhi_.resources, rhi_.bindless, rhi_.frames, desc);
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
        rhi_.bindless.Deinit();
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

    std::vector<std::pair<cairns::DrawKey,uint32_t>,cairns::Allocator<std::pair<cairns::DrawKey,uint32_t>>> drawListSorted_;
    std::vector<cairns::Draw,cairns::Allocator<cairns::Draw>> drawList_;
    std::vector<rhi::Handle<rhi::Texture>> resident_textures_;
    
    rhi::Rhi rhi_;
    rhi::Handle<rhi::Buffer> mesh_master_handle_ = rhi::Handle<rhi::Buffer>::Null;
    rhi::Handle<rhi::BindGroup> bindless_bg_;
    std::vector<uint32_t> texture_id_map_;
    std::vector<uint32_t> mesh_attr_id_map_;
    std::vector<uint32_t> sampler_id_map_;

    cairns::rhi::SwapChain swapchain_;
    // shaders
    ShaderHandle unlit_ = ShaderHandle::Null;
    // particles
    static constexpr uint32_t kParticleCount = 512;
    rhi::Handle<rhi::Kernel> particle_kernel_;
    rhi::Handle<rhi::Shader> particle_render_shader_;
    rhi::Handle<rhi::Buffer> particle_ssbo_[2];
    uint32_t particle_parity_ = 0;
    uint32_t globals_offset_ = 0;
    uint64_t last_ticks_ = 0;
    // render pass
    static constexpr size_t sampleCount = 4;
};

} // namespace cairns

