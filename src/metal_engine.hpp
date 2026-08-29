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

#include "swap_chain.hpp"
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

namespace {

MTL::Library* compileMetalShader(MTL::Device* pDevice, std::string_view shaderPath) {
    // 2. Read the shader source code into a C++ string
    std::string shaderSource = cairns::ReadFileToString(shaderPath);
    if (shaderSource.empty()) {
        std::cerr << "Failed to load shader source from: " << shaderPath << std::endl;
        return nullptr;
    }
    
    // 3. Convert C++ string to Metal::NSString
    NS::String* sourceString = NS::String::string(shaderSource.c_str(), NS::StringEncoding::UTF8StringEncoding);
    
    // 4. Create compilation options
    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    // Set a specific Metal language version for compatibility
    options->setLanguageVersion(MTL::LanguageVersion::LanguageVersion2_4);
    // You can add more options here, like preprocessor macros (setPreprocessorMacros)
    
    // 5. Compile the library
    NS::Error* error = nullptr;
    // newLibrary takes the source string, options, and returns any error
    MTL::Library* pLibrary = pDevice->newLibrary(sourceString, options, &error);
    
    // 6. Check for errors and release temporary objects
    if (error) {
        // Output the descriptive error message from Metal/Foundation
        std::cerr << "Metal Compilation Error: " << error->localizedDescription()->utf8String() << std::endl;
    }
    
    // Release objects that we retained with alloc/init
    options->release();
    
    return pLibrary;
}

} // anonymous namespace

namespace cairns::rhi {

bool InitRenderPassDescriptor(MTL::RenderPassDescriptor*& renderPassDescriptor, MTL::Texture* msaa, MTL::Texture* depth,
                              SwapChain& swap_chain) {
    renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();

    MTL::RenderPassColorAttachmentDescriptor* colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = renderPassDescriptor->depthAttachment();

    colorAttachment->setTexture(msaa);
    colorAttachment->setResolveTexture(swap_chain.GetDrawable()->texture());
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setClearColor(MTL::ClearColor(41.0f/255.0f, 42.0f/255.0f, 48.0f/255.0f, 1.0));
    colorAttachment->setStoreAction(MTL::StoreActionMultisampleResolve);

    depthAttachment->setTexture(depth);
    depthAttachment->setLoadAction(MTL::LoadActionClear);
    depthAttachment->setStoreAction(MTL::StoreActionDontCare);
    depthAttachment->setClearDepth(1.0);

    return true;
}

bool UpdateRenderPassDescriptor(MTL::RenderPassDescriptor* render_pass_desc, MTL::Texture* msaa, MTL::Texture* depth, SwapChain& swap_chain) {
    render_pass_desc->colorAttachments()->object(0)->setTexture(msaa);
    render_pass_desc->colorAttachments()->object(0)->setResolveTexture(swap_chain.GetDrawable()->texture());
    render_pass_desc->depthAttachment()->setTexture(depth);
    return true;
}

}  // namespace cairns::rhi

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
        
        if ( !swapChain_->Init(device_, window)) {
            return false;
        }
        
        return true;
    }
    
    void resetFrameTmps() {
        dynBufs_.Reset();
        bindGroups_.Reset();
    }
    
    bool requestResizeFrameBuffer(uint32_t width, uint32_t height) {
        resizeFrameBufferRequest_ = ResizeFrameBufferRequest{.width = width, .height = height};
        return true;
    }

    bool RequestViewportDump(const std::filesystem::path& path) {
        dumpPath_ = path;
        return true;
    }
    
    bool resizeFrameBuffer(int width, int height) {
        swapChain_->SetDrawableSize(width, height);
        // Deallocate the textures if they have been created
        if ( !msaaHandle_.IsNull() ) {
            rm_.Destroy(msaaHandle_);
            msaaHandle_ = TexHandle::Null;
        }
        if ( !depthHandle_.IsNull() ) {
            rm_.Destroy(depthHandle_);
            depthHandle_ = TexHandle::Null;
        }
        initDepthAndMSAATextures();
        swapChain_->NextDrawable();
        updateRenderPassDescriptor();
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
        if ( !initDevice() ) {
            return false;
        }
        if ( !initCommandQueue() ) {
            return false;
        }
        {
            rhi::BackendInitParams rhi_p;
            rhi_p.device = device_;
            rhi_p.queue = metalCommandQueue;
            if (!rm_.Init(rhi_p)) {
                return false;
            }
        }
        if ( !initSwapChain(window)) {
            return false;
        }
        { // init debug assets
//             debugSceneXforms_ = cairns::GenerateDebugGridTransforms(glm::vec3(-1, -1, -3), 3, 1, 1, 1, 0.5, 9);
         debugSceneXforms_ = cairns::GenerateDebugGridTransforms(glm::vec3(-1, -1, -3), 3, 1, 1, 1, 0.005, 9);
            //            debugSceneXforms_ = cairns::GenerateDebugGridTransforms(glm::vec3(-1.25, -2.5, -3), 10, 0.25, 0.5, -0.25, 0.001, 100);
//            debugSceneXforms_ = cairns::GenerateDebugGridTransforms(glm::vec3(-1.25, -2.5, -3), 10, 0.25, 0.5, 0.25, 0.001, 3000);
            
            for ( size_t glb_idx = cairns::kDebugGlbsToParseStart; glb_idx < cairns::kDebugGlbsToParseStart + cairns::kDebugGlbsToParse; ++glb_idx ) {
                scenes_.push_back(cairns::Scene(hot_arena_));
                cairns::Scene& scene = scenes_.back();
                std::string_view file = cairns::kDebugGlbs[glb_idx];
                std::filesystem::path filepath;
                if (!cairns::GetStaticResourceFilepath(file, filepath)) {
                    printf("file missing %s\n",file.data());
                    return false;
                }
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
            uint32_t off = 0;
            mesh_master_buf_ =
                rm_.GetMtlBuffer(scenes_[0].meshes[0].posHandle, &off);
        }
        if ( !initDepthAndMSAATextures() ) {
            return false;
        }
        if ( !initRenderPassDescriptor() ) {
            return false;
        }
        if ( !initFrameSemaphore() ) {
            return false;
        }
        if ( !initRenderPipeline() ) {
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
    
    bool initDepthAndMSAATextures() {
        const int32_t w = static_cast<int32_t>(swapChain_->GetDrawableSize().width);
        const int32_t h = static_cast<int32_t>(swapChain_->GetDrawableSize().height);
        {
            rhi::TextureDesc d;
            d.dimensions = {w, h, 1};
            d.format = rhi::Format::kBgra8Unorm;
            d.sample_count = static_cast<uint32_t>(sampleCount);
            d.usage = rhi::kTexUsageColorTarget;
            d.memory = rhi::Memory::kDefault;
            msaaHandle_ = rm_.CreateTexture(d);
            if (msaaHandle_.IsNull()) {
                return false;
            }
        }
        {
            rhi::TextureDesc d;
            d.dimensions = {w, h, 1};
            d.format = rhi::Format::kD32F;
            d.sample_count = static_cast<uint32_t>(sampleCount);
            d.usage = rhi::kTexUsageDepthTarget;
            d.memory = rhi::Memory::kDefault;
            depthHandle_ = rm_.CreateTexture(d);
            if (depthHandle_.IsNull()) {
                return false;
            }
        }
        return true;
    }
    
    bool initRenderPassDescriptor() {
        MTL::Texture* msaa = rm_.GetHot(msaaHandle_)->api_view;
        MTL::Texture* depth = rm_.GetHot(depthHandle_)->api_view;
        if ( !cairns::rhi::InitRenderPassDescriptor(render_pass_descriptor_, msaa, depth, *swapChain_)) {
            return false;
        }
        return true;
    }
    
    bool initFrameSemaphore() {
        frameSemaphore = dispatch_semaphore_create(kBufferedFrames);
        return true;
    }
    
    bool updateRenderPassDescriptor() {
        MTL::Texture* msaa = rm_.GetHot(msaaHandle_)->api_view;
        MTL::Texture* depth = rm_.GetHot(depthHandle_)->api_view;
        if ( cairns::rhi::UpdateRenderPassDescriptor(render_pass_descriptor_, msaa, depth, *swapChain_)) {
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
        
        const float aspect_ratio = (1.0f * swapChain_->GetDrawableSize().width) / swapChain_->GetDrawableSize().height;
        const float fov = 90 * (std::numbers::pi / 180.0f);
        const float near_z = 0.1f;
        const float far_z = 100.0f;
        
        const glm::mat4 proj_matrix = glm::perspectiveRH_ZO(fov, aspect_ratio, near_z, far_z);
        
        const ShaderHandle shader = unlit_;
        
        const BindGroupId bg_globals = getBindGroup();
        { // set up render pass globals
            // set up camera
            const float screen_width = swapChain_->GetDrawableSize().width;
            const float screen_height = swapChain_->GetDrawableSize().height;
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
                    draw.dynamic_buffer_offsets = {};
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
        if (frame_ == 5 && dumpPath_.empty()) {
            dumpPath_ = "/tmp/cairns_dump.png";
        }
        
        dispatch_semaphore_wait(frameSemaphore, DISPATCH_TIME_FOREVER);

        resetFrameTmps();
        rm_.BeginFrame();

        if ( resizeFrameBufferRequest_ ) {
            resizeFrameBuffer(resizeFrameBufferRequest_->width, resizeFrameBufferRequest_->height);
            resizeFrameBufferRequest_ = std::nullopt;
        }
        
        if (!swapChain_->NextDrawable()) {
            return false;
        }
        updateRenderPassDescriptor();
        MTL::CommandBuffer* cmdBuf = metalCommandQueue->commandBuffer();
        cmdBuf->addCompletedHandler([&](MTL::CommandBuffer*) {
            dispatch_semaphore_signal(frameSemaphore);
        });
        
        const uint64_t now_ticks = SDL_GetTicks();
        float delta_time = 0.016f;
#if !defined(CAIRNS_FREEZE_ROT) || !CAIRNS_FREEZE_ROT
        if (last_ticks_ > 0) {
            delta_time = static_cast<float>(now_ticks - last_ticks_) / 1000.0f;
        }
#endif
        last_ticks_ = now_ticks;

        cairns::Timer timer0("timer 0", 0);

        if ( !BuildMeshOpaqueDraws()) {
            return false;
        }

        { // sort materials next to each other
            std::sort(drawListSorted_.begin(),drawListSorted_.end());
        }

        timer0.End();

        uint32_t out_off = 0;
        MTL::Buffer* out_buf = rm_.GetMtlBuffer(particle_ssbo_[1 - particle_parity_], &out_off);

        {
            MTL::ComputeCommandEncoder* cenc = cmdBuf->computeCommandEncoder();
            cenc->setComputePipelineState(rm_.GetHot(particle_kernel_)->api_pso);
            float* dt_ptr = static_cast<float*>(
                rm_.BumpAllocate(sizeof(float), sizeof(float), rhi::Memory::kDynamic));
            *dt_ptr = delta_time;
            MTL::Buffer* dyn_master = rm_.GetBumpMasterBuffer(rhi::Memory::kDynamic);
            cenc->setBuffer(dyn_master, rm_.BumpOffset(dt_ptr), 0);
            uint32_t in_off = 0;
            MTL::Buffer* in_buf = rm_.GetMtlBuffer(particle_ssbo_[particle_parity_], &in_off);
            cenc->setBuffer(in_buf, in_off, 1);
            cenc->setBuffer(out_buf, out_off, 2);
            cenc->dispatchThreadgroups(MTL::Size{kParticleCount / 256, 1, 1},
                                       MTL::Size{256, 1, 1});
            cenc->endEncoding();
        }

        cairns::Timer timer1("timer 1", 1);
        MTL::RenderCommandEncoder* encoder = nullptr;
        {
            encoder = cmdBuf->renderCommandEncoder(render_pass_descriptor_);
        }
        
        {
            MTL::RenderPipelineState* pso =
                rm_.GetHot(unlit_)->api_pso;

            encoder->setRenderPipelineState(pso);
            encoder->setDepthStencilState(depthStencilState);
            encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
            encoder->setCullMode(MTL::CullModeBack);

            {
                rhi::BindGroup::Hot* bg_hot = rm_.GetHot(bindless_bg_handle_);
                MTL::Buffer* bg_buf = bg_hot->api_descriptor_set;
                const uint32_t bg_off = bg_hot->arg_buf_offset;
                encoder->setVertexBuffer(bg_buf, bg_off,
                                         cairns::rhi::GpuSceneRegistry::kBindSlot);
                encoder->setFragmentBuffer(bg_buf, bg_off,
                                           cairns::rhi::GpuSceneRegistry::kBindSlot);
            }
            // use resource call for all textures in argument table
            for (auto& s : scenes_) {
                for (const auto th : s.textureHandles) {
                    MTL::Texture* tex = rm_.GetHot(th)->api_view;
                    if (tex) {
                        encoder->useResource(tex, MTL::ResourceUsageRead,
                                             MTL::RenderStageFragment);
                    }
                }
            }
            // use resource call for all vertex attributes in argument table
            encoder->useResource(mesh_master_buf_, MTL::ResourceUsageRead, MTL::RenderStageVertex);
            
            encoder->setVertexBuffer(mesh_master_buf_, 0, 0);
            MTL::Buffer* dyn_master =
                rm_.GetBumpMasterBuffer(rhi::Memory::kDynamic);
            // set up render pass globals bind group in vertex shader
            encoder->setVertexBuffer(dyn_master, 0, cairns::kRenderPassGlobalBindSlot);
            // set up material buffer bind group in vertex shader
            encoder->setVertexBuffer(dyn_master, 0, cairns::kMaterialBindSlot);
            // set up shader specific buffers bind group in vertex shader
            // none
            // set up per draw temporaries bind group in vertex shader
            encoder->setVertexBuffer(dyn_master, 0, cairns::kDrawTmpBindSlot);
            
            MatId last_mat = std::numeric_limits<uint32_t>::max();
            uint32_t triangles = 0;
            for ( size_t draw_idx = 0; draw_idx < drawListSorted_.size(); ++draw_idx ) {
                const cairns::Draw& draw = drawList_[drawListSorted_[draw_idx].second];
                { // set position buffer offset
                    const BufHandle pos = draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot];
                    uint32_t pos_off = 0;
                    rm_.GetMtlBuffer(pos, &pos_off);
                    encoder->setVertexBufferOffset(pos_off, 0 /*hard coded for some reason*/);
                }
                { // set up material
                    const BindGroupId mat_bg = draw.bind_groups[cairns::kMaterialBindSlot-1];
                    auto& mat_bg_obj = bindGroups_.At(mat_bg);
                    const MatId mat = mat_bg_obj.material;
                    if ( mat != last_mat ) {
                        last_mat = mat;
                        encoder->setVertexBufferOffset(mat_bg_obj.material_offset, cairns::kMaterialBindSlot);
                    }
                }
                { // set up draw temporary
                    const DynBufId draw_tmp_bg = draw.dynamic_buffers;
                    auto& draw_tmp_obj = dynBufs_.At(draw_tmp_bg);
                    encoder->setVertexBufferOffset(draw_tmp_obj.offset, cairns::kDrawTmpBindSlot);
                }
                {
                    const uint32_t index_count = draw.triangle_count * 3;
                    const uint32_t index_offset = draw.index_offset;
                    const uint32_t vertex_offset = draw.vertex_offset;
                    const BufHandle index = draw.index_buffer;
                    uint32_t index_master_off = 0;
                    MTL::Buffer* const index_buffer = rm_.GetMtlBuffer(index, &index_master_off);
                    encoder->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle,
                                                   index_count,
                                                   MTL::IndexTypeUInt32,
                                                   index_buffer,
                                                   index_offset,
                                                   1,
                                                   vertex_offset,
                                                   0);
                    triangles += draw.triangle_count;
                }
            }
            printf("draws %d triangles %d\n",(int)drawListSorted_.size(),triangles);
        }

        {
            encoder->setRenderPipelineState(rm_.GetHot(particle_render_pso_)->api_pso);
            encoder->setVertexBuffer(out_buf, out_off, 0);
            encoder->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(0), NS::UInteger(kParticleCount));
        }

        encoder->endEncoding();
        timer1.End();

        cairns::Timer::PrintReport(true);

        if (!dumpPath_.empty()) {
            MTL::Texture* drawableTex = swapChain_->GetDrawable()->texture();
            const NS::UInteger w = drawableTex->width();
            const NS::UInteger h = drawableTex->height();
            const NS::UInteger bytesPerRow = w * 4;
            const NS::UInteger bufSize = bytesPerRow * h;
            MTL::Buffer* readback = device_->newBuffer(bufSize, MTL::ResourceStorageModeShared);
            MTL::BlitCommandEncoder* blitEnc = cmdBuf->blitCommandEncoder();
            blitEnc->copyFromTexture(drawableTex, 0, 0,
                                     MTL::Origin{0, 0, 0}, MTL::Size{w, h, 1},
                                     readback, 0, bytesPerRow, 0);
            blitEnc->endEncoding();
            cmdBuf->presentDrawable(swapChain_->GetDrawable());
            cmdBuf->commit();
            cmdBuf->waitUntilCompleted();
            std::vector<uint8_t> rgba(bufSize);
            const uint8_t* bgra = static_cast<const uint8_t*>(readback->contents());
            for (NS::UInteger i = 0; i < w * h; ++i) {
                rgba[i*4+0] = bgra[i*4+2];
                rgba[i*4+1] = bgra[i*4+1];
                rgba[i*4+2] = bgra[i*4+0];
                rgba[i*4+3] = bgra[i*4+3];
            }
            stbi_write_png(dumpPath_.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba.data(), static_cast<int>(bytesPerRow));
            printf("viewport dumped -> %s\n", dumpPath_.string().c_str());
            readback->release();
            dumpPath_.clear();
            particle_parity_ ^= 1;
            return true;
        }

        particle_parity_ ^= 1;

        // 5. Present and Commit
        cmdBuf->presentDrawable(swapChain_->GetDrawable());
        cmdBuf->commit();

        return true;
    }
    
    bool initRenderPipeline() {
        MTL::Library* metal_default_library = nullptr;
        {
#if CAIRNS_ANDROID
            std::filesystem::path basePath = "";   // on Android we do not want to use basepath. Instead, assets are available at the root directory.
#elif CAIRNS_APPLE
            auto basePathPtr = SDL_GetBasePath();
            if (not basePathPtr){
                return false;
            }
            const std::filesystem::path basePath = basePathPtr;
#endif // CAIRNS_ANDROID
            const auto cubeShaderPath = basePath / "unlit.metal";

            metal_default_library = compileMetalShader(device_, cubeShaderPath.string().c_str());
        }

        MTL::Function* vertexShader = metal_default_library->newFunction(NS::String::string("cube::vertexShader", NS::ASCIIStringEncoding));
        assert(vertexShader);
        MTL::Function* fragmentShader = metal_default_library->newFunction(NS::String::string("cube::fragmentShader", NS::ASCIIStringEncoding));
        assert(fragmentShader);
        
        MTL::RenderPipelineDescriptor* renderPipelineDescriptor = MTL::RenderPipelineDescriptor::alloc()->init();
        renderPipelineDescriptor->setVertexFunction(vertexShader);
        renderPipelineDescriptor->setFragmentFunction(fragmentShader);
        assert(renderPipelineDescriptor);
        MTL::PixelFormat pixelFormat = (MTL::PixelFormat)swapChain_->GetPixelFormat();
        renderPipelineDescriptor->colorAttachments()->object(0)->setPixelFormat(pixelFormat);
        renderPipelineDescriptor->setSampleCount(sampleCount);
        renderPipelineDescriptor->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
        
        {
            MTL::VertexDescriptor* vertex_desc = nullptr;
            vertex_desc = MTL::VertexDescriptor::alloc()->init();
            MTL::VertexAttributeDescriptor* const attr0 = vertex_desc->attributes()->object(0);
            attr0->setFormat(MTL::VertexFormatFloat4);
            attr0->setOffset(0);
            attr0->setBufferIndex(cairns::kMeshPosBindSlot);
            
            MTL::VertexBufferLayoutDescriptor* const layout0 = vertex_desc->layouts()->object(0);
            layout0->setStride(sizeof(glm::vec4));
            layout0->setStepFunction(MTL::VertexStepFunctionPerVertex);
            layout0->setStepRate(1);
            renderPipelineDescriptor->setVertexDescriptor(vertex_desc);
            vertex_desc = nullptr;
        }
        
        NS::Error* error = nullptr;

        MTL::RenderPipelineState* pso =
            device_->newRenderPipelineState(renderPipelineDescriptor, &error);

        if (pso == nullptr) {
            std::cout << "Error creating render pipeline state: " << error << std::endl;
            std::exit(0);
        }

        unlit_ = rm_.CreateShader({.api_pso = pso, .debug_name = "unlit"});

        { // bindless resources set up via rhi
            auto* texArg = MTL::ArgumentDescriptor::alloc()->init();
            texArg->setDataType(MTL::DataTypeTexture);
            texArg->setIndex(cairns::rhi::GpuSceneRegistry::kTexturesSlotOffset);
            texArg->setArrayLength(cairns::rhi::GpuSceneRegistry::kMaxTextures);
            texArg->setAccess(MTL::ArgumentAccessReadOnly);

            auto* attrArg = MTL::ArgumentDescriptor::alloc()->init();
            attrArg->setDataType(MTL::DataTypePointer);
            attrArg->setIndex(cairns::rhi::GpuSceneRegistry::kMeshesSlotOffset);
            attrArg->setArrayLength(cairns::rhi::GpuSceneRegistry::kMaxMeshes);
            attrArg->setAccess(MTL::ArgumentAccessReadOnly);

            auto* sampArg = MTL::ArgumentDescriptor::alloc()->init();
            sampArg->setDataType(MTL::DataTypeSampler);
            sampArg->setIndex(cairns::rhi::GpuSceneRegistry::kSamplersSlotOffset);
            sampArg->setArrayLength(cairns::rhi::GpuSceneRegistry::kMaxSamplers);
            sampArg->setAccess(MTL::ArgumentAccessReadOnly);

            NS::Array* args = NS::Array::array((NS::Object*[]){ texArg, attrArg, sampArg }, 3);
            MTL::ArgumentEncoder* arg_encoder = device_->newArgumentEncoder(args);

            rhi::BufferDesc bd;
            bd.byte_size = static_cast<uint32_t>(arg_encoder->encodedLength());
            bd.usage = rhi::kUsageUniform | rhi::kUsageStorage;
            bd.memory = rhi::Memory::kUpload;
            rhi::Handle<rhi::Buffer> arg_buf_h = rm_.CreateBuffer(bd);
            uint32_t arg_off = 0;
            MTL::Buffer* arg_buf = rm_.GetMtlBuffer(arg_buf_h, &arg_off);

            arg_encoder->setArgumentBuffer(arg_buf, arg_off);

            mesh_attr_id_map_.clear();
            sampler_id_map_.clear();

            uint32_t num_tex = 0;
            uint32_t num_attr = 0;
            uint32_t num_sampler = 0;

            for (size_t i = 0; i < scenes_.size(); ++i) {
                cairns::Scene& scene = scenes_[i];
                for (size_t j = 0; j < scene.textureHandles.size(); ++j) {
                    auto h = scene.textureHandles[j];
                    MTL::Texture* tex = rm_.GetHot(h)->api_view;
                    if (tex) {
                        assert(h.index == num_tex);
                        arg_encoder->setTexture(tex,
                            cairns::rhi::GpuSceneRegistry::kTexturesSlotOffset + num_tex);
                        ++num_tex;
                    }
                }
                for (size_t j = 0; j < scene.meshes.size(); ++j) {
                    auto h = scene.meshes[j].attrHandle;
                    if (!h.IsNull()) {
                        uint32_t attr_off = 0;
                        MTL::Buffer* attr_buf = rm_.GetMtlBuffer(h, &attr_off);
                        arg_encoder->setBuffer(attr_buf, attr_off,
                            cairns::rhi::GpuSceneRegistry::kMeshesSlotOffset + num_attr);
                        mesh_attr_id_map_[h.index] = num_attr;
                        ++num_attr;
                    }
                }
                for (size_t j = 0; j < scene.samplerHandles.size(); ++j) {
                    auto h = scene.samplerHandles[j];
                    MTL::SamplerState* samp = rm_.GetHot(h)->api_sampler;
                    arg_encoder->setSamplerState(samp,
                        cairns::rhi::GpuSceneRegistry::kSamplersSlotOffset + num_sampler);
                    sampler_id_map_[h.index] = num_sampler;
                    ++num_sampler;
                }
            }

            arg_encoder->release();
            arg_encoder = nullptr;

            bindless_bg_handle_ = rm_.CreateBindGroupFromMtlBuffer(arg_buf, arg_off);
        }
        
        MTL::DepthStencilDescriptor* depthStencilDescriptor = MTL::DepthStencilDescriptor::alloc()->init();
        depthStencilDescriptor->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        depthStencilDescriptor->setDepthWriteEnabled(true);
        depthStencilState = device_->newDepthStencilState(depthStencilDescriptor);
        
        renderPipelineDescriptor->release();
        vertexShader->release();
        fragmentShader->release();
        
        return true;
    }
    
    bool initParticles() {
#if CAIRNS_APPLE
        auto basePathPtr = SDL_GetBasePath();
        if (!basePathPtr) {
            return false;
        }
        const std::filesystem::path basePath = basePathPtr;
#else
        const std::filesystem::path basePath = "";
#endif
        MTL::Library* lib = compileMetalShader(device_, (basePath / "particle.metal").string().c_str());
        if (!lib) {
            return false;
        }

        {
            MTL::Function* fn = lib->newFunction(NS::String::string("particle_compute", NS::ASCIIStringEncoding));
            if (!fn) {
                lib->release();
                return false;
            }
            NS::Error* err = nullptr;
            MTL::ComputePipelineState* cps = device_->newComputePipelineState(fn, &err);
            fn->release();
            if (!cps) {
                lib->release();
                return false;
            }
            particle_kernel_ = rm_.CreateKernel({.api_pso = cps, .debug_name = "particle_compute"});
        }

        {
            MTL::Function* vfn = lib->newFunction(NS::String::string("particle_vertex", NS::ASCIIStringEncoding));
            MTL::Function* ffn = lib->newFunction(NS::String::string("particle_fragment", NS::ASCIIStringEncoding));
            if (!vfn || !ffn) {
                lib->release();
                return false;
            }
            MTL::RenderPipelineDescriptor* rpd = MTL::RenderPipelineDescriptor::alloc()->init();
            rpd->setVertexFunction(vfn);
            rpd->setFragmentFunction(ffn);
            const MTL::PixelFormat pf = static_cast<MTL::PixelFormat>(swapChain_->GetPixelFormat());
            rpd->colorAttachments()->object(0)->setPixelFormat(pf);
            rpd->colorAttachments()->object(0)->setBlendingEnabled(true);
            rpd->colorAttachments()->object(0)->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
            rpd->colorAttachments()->object(0)->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
            rpd->colorAttachments()->object(0)->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
            rpd->colorAttachments()->object(0)->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
            rpd->setSampleCount(sampleCount);
            rpd->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
            rpd->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassPoint);
            NS::Error* err = nullptr;
            MTL::RenderPipelineState* rps = device_->newRenderPipelineState(rpd, &err);
            rpd->release();
            vfn->release();
            ffn->release();
            if (!rps) {
                lib->release();
                return false;
            }
            particle_render_pso_ = rm_.CreateShader({.api_pso = rps, .debug_name = "particle_render"});
        }

        lib->release();

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
    
    static constexpr uint32_t kBufferedFrames = 2;
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
    
    rhi::ResourceManager rm_;
    MTL::Buffer* mesh_master_buf_ = nullptr;
    rhi::Handle<rhi::BindGroup> bindless_bg_handle_;
    std::unordered_map<uint32_t, uint32_t> mesh_attr_id_map_;
    std::unordered_map<uint32_t, uint32_t> sampler_id_map_;

    //    std::vector<std::vector<std::function<void(void)>>> deletionRequests_;
    //    std::vector<std::function<void(void)>> deletions_;
    
    struct ResizeFrameBufferRequest {
        uint32_t width = std::numeric_limits<uint32_t>::max();
        uint32_t height = std::numeric_limits<uint32_t>::max();
    };
    std::optional<ResizeFrameBufferRequest> resizeFrameBufferRequest_ = std::nullopt;
    std::filesystem::path dumpPath_;

    std::unique_ptr<cairns::rhi::SwapChain> swapChain_ = nullptr;
    // command queue
    MTL::CommandQueue* metalCommandQueue = nullptr;
    // shaders
    ShaderHandle unlit_ = ShaderHandle::Null;
    MTL::DepthStencilState* depthStencilState = nullptr;
    // particles
    static constexpr uint32_t kParticleCount = 512;
    rhi::Handle<rhi::Kernel> particle_kernel_;
    rhi::Handle<rhi::Shader> particle_render_pso_;
    rhi::Handle<rhi::Buffer> particle_ssbo_[2];
    uint32_t particle_parity_ = 0;
    uint64_t last_ticks_ = 0;
    // render pass
    static constexpr size_t sampleCount = 4;
    TexHandle msaaHandle_ = TexHandle::Null;
    TexHandle depthHandle_ = TexHandle::Null;
    MTL::RenderPassDescriptor* render_pass_descriptor_ = nullptr;
    // fences and semaphores
    dispatch_semaphore_t frameSemaphore;
};

} // namespace cairns

#endif // CAIRNS_METAL

