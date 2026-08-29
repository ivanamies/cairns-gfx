#pragma once

#include "util/define.hpp"

#if CAIRNS_METAL

#include <cmath>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
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
#include "util/frame_transient_cache.hpp"
#include "util/timer.hpp"
#include "util/unique_ptr.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/command_recorder.hpp"

namespace cairns {

inline static constexpr uint32_t kHotArenaMemorySize = 1 << 29;
inline static constexpr uint32_t kUboAlign = 32;
inline static constexpr uint32_t kMeshPosBindSlot = 0;

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
    
    bool initDevice() {
        device_ = MTL::CreateSystemDefaultDevice();
        return device_ != nullptr;
    }
    
    bool initSwapChain(SDL_Window* window) {
        swapChain_ = std::make_unique<cairns::rhi::SwapChain>();
        
        if ( !rm_.InitSwapChain(*swapChain_, window)) {
            return false;
        }
        
        return true;
    }
    
    void resetFrameTmps() {
        dynBufs_.Reset();
        bindGroups_.Reset();
    }
    
    bool requestResizeFrameBuffer(uint32_t width, uint32_t height) {
        (void)width;
        (void)height;
        return true;
    }

    bool RequestViewportDump(const std::filesystem::path& path) {
        rm_.SetDumpPath(path);
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
            return false;
        }
        if ( !initResourceManagers() ) {
            return false;
        }
        if (!rm_.InitDevice(window)) {
            return false;
        }
        device_ = rm_.GetMtlDevice();
        metalCommandQueue = rm_.GetMtlQueue();
        if ( !initSwapChain(window)) {
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
                        return false;
                    }
                    glb_paths.push_back(filepath);
                }
            }

            const int instance_count =
                std::getenv("CAIRNS_N") ? std::atoi(std::getenv("CAIRNS_N"))
                                        : static_cast<int>(glb_paths.size());
            const float scale =
                std::getenv("CAIRNS_SCALE")
                    ? static_cast<float>(std::atof(std::getenv("CAIRNS_SCALE")))
                    : 0.005f;
            debugSceneXforms_ = cairns::GenerateDebugGridTransforms(
                glm::vec3(-1, -1, -3), 3, 1, 1, 1, scale, instance_count);

            for (const std::filesystem::path& filepath : glb_paths) {
                scenes_.push_back(cairns::Scene(hot_arena_));
                cairns::Scene& scene = scenes_.back();
                if (!cairns::LoadSceneFromGltf(filepath, scene)) {
                    return false;
                }
                cairns::PrepareSceneResources(scene, rm_, materials_);

                if (!cairns::rhi::LoadSceneGpu(scene, rm_)) {
                    return false;
                }

                scene.CleanupTmps();
            }
        }
        if (!scenes_.empty() && !scenes_[0].meshes.empty()) {
            mesh_master_handle_ = scenes_[0].meshes[0].posHandle;
        }
        if ( !initRenderPipeline() ) {
            return false;
        }
        if ( !rm_.InitFrameTargets(*swapChain_) ) {
            return false;
        }
        if ( !initParticles() ) {
            return false;
        }

        return true;
    }

    // tagiamies
    DynBufId getDynamicBuffers() {
        return dynBufs_.Acquire();
    }
    
    BindGroupId getBindGroup() {
        return bindGroups_.Acquire();
    }
    
    bool initCommandQueue() {
        metalCommandQueue = device_->newCommandQueue();
        return metalCommandQueue != nullptr;
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
        
        const float aspect_ratio = (1.0f * swapChain_->Width()) / swapChain_->Height();
        const float fov = 90 * (std::numbers::pi / 180.0f);
        const float near_z = 0.1f;
        const float far_z = 100.0f;
        
        const glm::mat4 proj_matrix = glm::perspectiveRH_ZO(fov, aspect_ratio, near_z, far_z);
        
        const ShaderHandle shader = unlit_;
        
        const BindGroupId bg_globals = getBindGroup();
        { // set up render pass globals
            // set up camera
            const float screen_width = swapChain_->Width();
            const float screen_height = swapChain_->Height();
            glm::mat4 view_proj = proj_matrix * view_matrix;
            cairns::rhi::RenderPassGlobals render_pass_globals {
                .view_proj = view_proj,
                .inv_view_proj = glm::inverse(view_proj),
                .camera_pos = glm::vec4(camera_pos, 1.0f /*exposure */),
                .camera_dir = glm::vec4(camera_dir, near_z),
                .screen_params = glm::vec4(screen_width, screen_height, 1.0f / screen_width, 1.0f / screen_height)
            };
            const uint32_t dyn_align =
                kUboAlign;
            void* gptr = rm_.BumpAllocate(
                sizeof(cairns::rhi::RenderPassGlobals), dyn_align,
                rhi::Memory::kDynamic);
            memcpy(gptr, &render_pass_globals, sizeof(render_pass_globals));
            globals_offset_ = rm_.BumpOffset(gptr);
        }

        //        cairns::Timer timer2("timer2", 2);
        for ( size_t scene_xform_idx = 0; scene_xform_idx < debugSceneXforms_.size(); ++scene_xform_idx ) {
            size_t scene_idx = scene_xform_idx % scenes_.size();
            const glm::mat4& scene_xform = debugSceneXforms_[scene_xform_idx];
            //            cairns::Timer timer3("timer3", 3);
            cairns::Scene& scene = scenes_[scene_idx];
            root_nodes_stack_cache_.clear();
            for ( size_t j = 0; j < scene.rootNodes.size(); ++j ) {
                root_nodes_stack_cache_.push_back(scene.rootNodes[j]);
            }
            while(!root_nodes_stack_cache_.empty()) {
                //                cairns::Timer timer3("timer4", 4);
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
                
                //                cairns::Timer timer5("timer5", 5);
                // warning @iamies alot of time is being lost between timers 2 and 3 and timers 5 and 6.
                // I am pretty sure this means cache misses.
                for(const auto& prim : mesh.primitives) {
                    //                    cairns::Timer timer6("timer6", 6);
                    const uint32_t scene_mat_idx = prim.materialIndex;
                    const MatId mat_id = scene.materialIds[scene_mat_idx];
                    const rhi::Handle<rhi::Texture> tex_handle = materials_[mat_id].color;
                    const SamplerHandle sampler_handle = materials_[mat_id].sampler;
                    
                    const uint32_t gpu_tex_id = tex_handle.index;
                    const uint32_t gpu_sampler_id = sampler_id_map_[sampler_handle.index];
                    const uint32_t gpu_attr_idx = mesh_attr_id_map_[mesh.attrHandle.index];
                    
                    //                    cairns::Timer timer7("timer7", 7);
                    const BindGroupId bg_material = getBindGroup();
                    auto& mat_obj = bindGroups_.At(bg_material);
                    { // material set up
                        const cairns::rhi::MaterialGpu material_gpu {
                            .tex_color_id = gpu_tex_id,
                            .sampler_id = gpu_sampler_id,
                        };
                        const uint32_t dyn_align =
                            kUboAlign;
                        void* mptr = rm_.BumpAllocate(
                            sizeof(cairns::rhi::MaterialGpu), dyn_align,
                            rhi::Memory::kDynamic);
                        memcpy(mptr, &material_gpu, sizeof(material_gpu));
                        mat_obj.material_offset = rm_.BumpOffset(mptr);
                        mat_obj.material = mat_id;
                    }
                    
                    //                    cairns::Timer timer8("timer8", 8);
                    const DynBufId tmp_handle = getDynamicBuffers();
                    auto& tmp_obj = dynBufs_.At(tmp_handle);
                    const glm::mat4 model_matrix = scene_xform * rot_matrix;
                    { // tmp draws set up
                        const cairns::rhi::DrawTmp draw_tmp {
                            .model_matrix = node.globalTransform * model_matrix,
                            .mesh_id = gpu_attr_idx,
                            .tex_id = gpu_tex_id,
                            .sampler_id = gpu_sampler_id
                        };
                        const uint32_t dyn_align =
                            kUboAlign;
                        void* tptr = rm_.BumpAllocate(
                            sizeof(cairns::rhi::DrawTmp), dyn_align,
                            rhi::Memory::kDynamic);
                        memcpy(tptr, &draw_tmp, sizeof(draw_tmp));
                        tmp_obj.offset = rm_.BumpOffset(tptr);
                    }
                    
                    //                    cairns::Timer timer9("timer9", 9);
                    cairns::Draw draw;
                    draw.shader = shader;
                    // todo @iamies
                    // should this be -1??
                    // did I mess up making vertex attributes bind slot 0?
                    draw.bind_groups[cairns::kRenderPassGlobalBindSlot-1] = bg_globals;
                    draw.bind_groups[cairns::kMaterialBindSlot-1] = bg_material;
                    draw.bind_groups[cairns::kShaderSpecificBindSlot-1] = cairns::kInvalidBindGroupId;
                    draw.dynamic_buffers = tmp_handle;
                    draw.index_buffer = index;
                    uint32_t index_base_off = 0;
                    rm_.GetMtlBuffer(index, &index_base_off);
                    draw.index_offset = index_base_off + (prim.firstIndex * sizeof(uint32_t));
                    draw.vertex_offset = prim.vertexOffset;
                    draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot] = pos;
                    draw.instance_offset = 0;
                    draw.instance_count = 1;
                    draw.dynamic_buffer_offsets[0] = mat_obj.material_offset;
                    draw.dynamic_buffer_offsets[1] = tmp_obj.offset;
                    assert(prim.indexCount % 3 == 0);
                    draw.triangle_count = prim.indexCount / 3;
                    
                    drawListSorted_.emplace_back(cairns::BuildDrawKey(draw),drawList_.size());
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
            rm_.SetDumpPath("/tmp/cairns_dump.png");
        }

        rhi::FrameContext fc = rm_.BeginFrame(*swapChain_);

        const uint64_t now_ticks = SDL_GetTicks();
        float delta_time = 0.016f;
#if !defined(CAIRNS_FREEZE_ROT) || !CAIRNS_FREEZE_ROT
        if (last_ticks_ > 0) {
            delta_time = static_cast<float>(now_ticks - last_ticks_) / 1000.0f;
        }
#endif
        last_ticks_ = now_ticks;

        if ( !BuildMeshOpaqueDraws()) {
            return false;
        }
        std::sort(drawListSorted_.begin(), drawListSorted_.end());
        sorted_draw_indices_.clear();
        for (const auto& [key, idx] : drawListSorted_) {
            sorted_draw_indices_.push_back(static_cast<uint32_t>(idx));
        }
        resident_textures_.clear();
        for (auto& s : scenes_) {
            for (const auto th : s.textureHandles) {
                resident_textures_.push_back(th);
            }
        }

        float* dt_ptr = static_cast<float*>(
            rm_.BumpAllocate(sizeof(float), rm_.UboAlign(), rhi::Memory::kDynamic));
        *dt_ptr = delta_time;
        const uint32_t dt_off = rm_.BumpOffset(dt_ptr);

        rhi::BoundBuffer cbufs[3] = {
            {0, rm_.BumpMasterBuffer(rhi::Memory::kDynamic), dt_off},
            {1, particle_ssbo_[particle_parity_], 0},
            {2, particle_ssbo_[1 - particle_parity_], 0},
        };
        rhi::ComputeDispatch cd{};
        cd.kernel = particle_kernel_;
        cd.buffers = rhi::Span<const rhi::BoundBuffer>(cbufs, 3);
        cd.groups_x = kParticleCount / 256;
        cd.local_x = 256;
        fc.cmd.Dispatch(cd);

        rhi::ColorAttachment col[1]{};
        col[0].clear[0] = 41.0f / 255.0f;
        col[0].clear[1] = 42.0f / 255.0f;
        col[0].clear[2] = 48.0f / 255.0f;
        col[0].clear[3] = 1.0f;
        rhi::RenderPassDesc rp{};
        rp.color = rhi::Span<const rhi::ColorAttachment>(col, 1);
        rp.depth.clear_depth = 1.0f;
        rp.width = swapChain_->Width();
        rp.height = swapChain_->Height();
        fc.cmd.BeginRenderPass(rp);

        rhi::MeshDrawList ml{};
        ml.draws = rhi::Span<const cairns::Draw>(drawList_.data(), drawList_.size());
        ml.sorted_indices =
            rhi::Span<const uint32_t>(sorted_draw_indices_.data(), sorted_draw_indices_.size());
        ml.pipeline = unlit_;
        ml.bindless = bindless_bg_handle_;
        ml.globals_offset = globals_offset_;
        ml.resident_textures = rhi::Span<const rhi::Handle<rhi::Texture>>(
            resident_textures_.data(), resident_textures_.size());
        ml.resident_buffers =
            rhi::Span<const rhi::Handle<rhi::Buffer>>(&mesh_master_handle_, 1);
        fc.cmd.DrawMeshes(ml);

        rhi::PointDraw pd{};
        pd.pipeline = particle_render_pso_;
        pd.vertex_buffer = particle_ssbo_[1 - particle_parity_];
        pd.vertex_offset = 0;
        pd.vertex_count = kParticleCount;
        fc.cmd.DrawPoints(pd);

        fc.cmd.EndRenderPass();
        rm_.EndFrame(fc);
        particle_parity_ ^= 1;
        return true;
    }
    
    bool initRenderPipeline() {
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
                rhi::Span<const rhi::VertexInputAttribute>(&pos_attr, 1);
            desc.vertex_buffers =
                rhi::Span<const rhi::VertexBufferLayout>(&pos_layout, 1);
            desc.color_format = rhi::Format::kBgra8Unorm;
            desc.depth_format = rhi::Format::kD32F;
            desc.sample_count = sampleCount;
            desc.debug_name = "unlit";
            unlit_ = rm_.CreateGraphicsPipeline(desc);
            if (unlit_.IsNull()) {
                std::exit(0);
            }
        }

        { // bindless resources set up via rhi
            using R = cairns::rhi::GpuSceneRegistry;
            rhi::BindlessRegistryDesc rdesc{};
            rdesc.max_textures = R::kMaxTextures;
            rdesc.max_attr_buffers = R::kMaxMeshes;
            rdesc.max_samplers = R::kMaxSamplers;
            rdesc.texture_slot = R::kTexturesSlotOffset;
            rdesc.attr_buffer_slot = R::kMeshesSlotOffset;
            rdesc.sampler_slot = R::kSamplersSlotOffset;
            rdesc.debug_name = "bindless";
            bindless_bg_handle_ = rm_.CreateBindlessRegistry(rdesc);

            mesh_attr_id_map_.clear();
            sampler_id_map_.clear();

            for (size_t i = 0; i < scenes_.size(); ++i) {
                cairns::Scene& scene = scenes_[i];
                for (size_t j = 0; j < scene.textureHandles.size(); ++j) {
                    auto h = scene.textureHandles[j];
                    MTL::Texture* tex = rm_.GetHot(h)->api_view;
                    if (tex) {
                        const uint32_t slot =
                            rm_.BindlessAddTexture(bindless_bg_handle_, h);
                        assert(h.index == slot);
                        (void)slot;
                    }
                }
                for (size_t j = 0; j < scene.meshes.size(); ++j) {
                    auto h = scene.meshes[j].attrHandle;
                    if (!h.IsNull()) {
                        mesh_attr_id_map_[h.index] =
                            rm_.BindlessAddAttrBuffer(bindless_bg_handle_, h);
                    }
                }
                for (size_t j = 0; j < scene.samplerHandles.size(); ++j) {
                    auto h = scene.samplerHandles[j];
                    sampler_id_map_[h.index] =
                        rm_.BindlessAddSampler(bindless_bg_handle_, h);
                }
            }

            rm_.BindlessFinalize(bindless_bg_handle_);
        }

        return true;
    }
    
    bool initParticles() {
        const char* base = SDL_GetBasePath();
        const std::string shader_dir = base ? base : "";
        {  // particle compute kernel via rhi
            rhi::ComputePipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.debug_name = "particle_compute";
            particle_kernel_ = rm_.CreateComputePipeline(desc);
            if (particle_kernel_.IsNull()) {
                return false;
            }
        }
        {  // particle render pipeline via rhi
            rhi::GraphicsPipelineDesc desc{};
            desc.logical_shader = "particle";
            desc.shader_dir = shader_dir.c_str();
            desc.topology = rhi::PrimitiveTopology::kPointList;
            desc.blend.enable = true;
            desc.blend.src_color = rhi::BlendFactor::kSrcAlpha;
            desc.blend.dst_color = rhi::BlendFactor::kOneMinusSrcAlpha;
            desc.blend.src_alpha = rhi::BlendFactor::kOne;
            desc.blend.dst_alpha = rhi::BlendFactor::kZero;
            desc.color_format = rhi::Format::kBgra8Unorm;
            desc.depth_format = rhi::Format::kD32F;
            desc.sample_count = sampleCount;
            desc.debug_name = "particle_render";
            particle_render_pso_ = rm_.CreateGraphicsPipeline(desc);
            if (particle_render_pso_.IsNull()) {
                return false;
            }
        }

        struct Particle {
            float position[2];
            float velocity[2];
            float color[4];
        };
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

        const rhi::Span<const uint8_t> init_data(
            reinterpret_cast<const uint8_t*>(particles.data()),
            particles.size() * sizeof(Particle));
        rhi::BufferDesc bd;
        bd.byte_size = static_cast<uint32_t>(particles.size() * sizeof(Particle));
        bd.usage = rhi::kUsageStorage | rhi::kUsageVertex;
        bd.memory = rhi::Memory::kDefault;
        bd.initial_data = init_data;
        particle_ssbo_[0] = rm_.CreateBuffer(bd);
        bd.initial_data = init_data;
        particle_ssbo_[1] = rm_.CreateBuffer(bd);

        return !particle_ssbo_[0].IsNull() && !particle_ssbo_[1].IsNull();
    }

    bool deinit() {
        // Metal Cleanup
        // Note: metal-cpp objects are wrappers. If we used NS::SharedPtr we could just let them destruct.
        // Since we have raw pointers from create/new, we should release them.
        if (metalCommandQueue) metalCommandQueue->release();

        swapChain_->Deinit();

        return true;
    }
    
private:
    // todo @iamies
    // make an engine dtor and delete this
    void* hot_arena_mem_;
    cairns::Arena hot_arena_;
    
    ////////// DO NOT MOVE ARENA BELOW THIS LINE. because c++.
    
    uint32_t frame_ = 0;
    
    MTL::Device* device_ = nullptr;
    std::vector<cairns::Scene, cairns::Allocator<cairns::Scene>> scenes_;
    std::vector<int32_t, cairns::Allocator<int32_t>> root_nodes_stack_cache_;
    
    std::vector<glm::mat4> debugSceneXforms_;
    
    cairns::FrameTransientCache<cairns::DynamicBuffersAssoc> dynBufs_;
    std::vector<cairns::LoadedMaterial> materials_;

    cairns::FrameTransientCache<cairns::BindGroupAssoc> bindGroups_;

    std::vector<std::pair<cairns::DrawKey,uint32_t>,cairns::Allocator<std::pair<cairns::DrawKey,uint32_t>>> drawListSorted_;
    std::vector<cairns::Draw,cairns::Allocator<cairns::Draw>> drawList_;
    std::vector<uint32_t> sorted_draw_indices_;
    std::vector<rhi::Handle<rhi::Texture>> resident_textures_;
    
    rhi::ResourceManager rm_;
    rhi::Handle<rhi::Buffer> mesh_master_handle_ = rhi::Handle<rhi::Buffer>::Null;
    rhi::Handle<rhi::BindGroup> bindless_bg_handle_;
    std::unordered_map<uint32_t, uint32_t> mesh_attr_id_map_;
    std::unordered_map<uint32_t, uint32_t> sampler_id_map_;

    std::unique_ptr<cairns::rhi::SwapChain> swapChain_ = nullptr;
    // command queue
    MTL::CommandQueue* metalCommandQueue = nullptr;
    // shaders
    ShaderHandle unlit_ = ShaderHandle::Null;
    // particles
    static constexpr uint32_t kParticleCount = 512;
    rhi::Handle<rhi::Kernel> particle_kernel_;
    rhi::Handle<rhi::Shader> particle_render_pso_;
    rhi::Handle<rhi::Buffer> particle_ssbo_[2];
    uint32_t particle_parity_ = 0;
    uint32_t globals_offset_ = 0;
    uint64_t last_ticks_ = 0;
    // render pass
    static constexpr size_t sampleCount = 4;
};

} // namespace cairns

#endif // CAIRNS_METAL

