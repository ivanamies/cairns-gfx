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

#include "rhi/tag.hpp"
#include "rhi/sampler.hpp"
#include "rhi/device.hpp"
#include "rhi/resource.hpp"
#include "rhi/swap_chain.hpp"
#include "rhi/gpu_scene_registry.hpp"
#include "util/misc.hpp"
#include "util/offset_allocator.hpp"
#include "util/gltf_loader.hpp"
#include "util/debug_asset.hpp"
#include "util/draw.hpp"
#include "util/draw_key.hpp"
#include "util/frame_transient_cache.hpp"
#include "util/timer.hpp"
#include "util/unique_ptr.hpp"
#include "rhi2/resource_manager.hpp"

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

struct MaterialGpu {
    uint32_t tex_color_id = std::numeric_limits<uint32_t>::max();
    uint32_t wip1 = std::numeric_limits<uint32_t>::max();
    uint32_t wip2 = std::numeric_limits<uint32_t>::max();
    uint32_t wip3 = std::numeric_limits<uint32_t>::max();
    uint32_t wip4 = std::numeric_limits<uint32_t>::max();
    uint32_t sampler_id = std::numeric_limits<uint32_t>::max();
};

// todo @iamies rename this
struct DrawTmp {
    glm::mat4 model_matrix;
    uint32_t mesh_id = std::numeric_limits<uint32_t>::max();
    uint32_t tex_id = std::numeric_limits<uint32_t>::max();
    uint32_t sampler_id = std::numeric_limits<uint32_t>::max();
    uint32_t yolo_padding = std::numeric_limits<uint32_t>::max();
};

bool LoadMeshGpu(Mesh& mesh, rhi2::ResourceManager& rm) {
    auto process = [&](rhi2::Handle<rhi2::Buffer>& h, const void* srcData,
                       size_t srcSize) -> bool {
        if (srcSize == 0) {
            return true;
        }
        rhi2::BufferDesc d;
        d.byte_size = static_cast<uint32_t>(srcSize);
        d.usage = rhi2::kUsageVertex | rhi2::kUsageIndex;
        d.memory = rhi2::Memory::kDefault;
        d.initial_data = rhi2::Span<const uint8_t>(
            static_cast<const uint8_t*>(srcData), srcSize);
        h = rm.CreateBuffer(d);
        return !h.IsNull();
    };

    if (!process(mesh.posHandle, mesh.cpuPositions.data(),
                 mesh.cpuPositions.size() * sizeof(glm::vec4))) {
        return false;
    }
    if (!process(mesh.attrHandle, mesh.cpuAttrs.data(),
                 mesh.cpuAttrs.size() * sizeof(VertexAttribute))) {
        return false;
    }
    if (!process(mesh.indexHandle, mesh.cpuIndices.data(),
                 mesh.cpuIndices.size() * sizeof(uint32_t))) {
        return false;
    }
    return true;
}

bool LoadSceneGpu(Scene& scene, rhi2::ResourceManager& rm)
{
    for ( size_t i = 0; i < scene.meshes.size(); ++i ) {
        auto& mesh = scene.meshes[i];
        if ( !LoadMeshGpu(mesh, rm) ) {
            return false;
        }
    }
    return true;
}

bool InitRenderPassDescriptor(MTL::RenderPassDescriptor*& renderPassDescriptor, Handle<Texture> msaa, Handle<Texture> depth,
                              ResourceManager<Texture>& tex_mgr, SwapChain& swap_chain) {
    renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();

    MTL::RenderPassColorAttachmentDescriptor* colorAttachment = renderPassDescriptor->colorAttachments()->object(0);
    MTL::RenderPassDepthAttachmentDescriptor* depthAttachment = renderPassDescriptor->depthAttachment();
    
    colorAttachment->setTexture(tex_mgr.GetObj(msaa)->texture);
    colorAttachment->setResolveTexture(swap_chain.GetDrawable()->texture());
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setClearColor(MTL::ClearColor(41.0f/255.0f, 42.0f/255.0f, 48.0f/255.0f, 1.0));
    colorAttachment->setStoreAction(MTL::StoreActionMultisampleResolve);
    
    depthAttachment->setTexture(tex_mgr.GetObj(depth)->texture);
    depthAttachment->setLoadAction(MTL::LoadActionClear);
    depthAttachment->setStoreAction(MTL::StoreActionDontCare);
    depthAttachment->setClearDepth(1.0);
    
    return true;
}

bool UpdateRenderPassDescriptor(MTL::RenderPassDescriptor* render_pass_desc, Handle<Texture> msaa, Handle<Texture> depth, ResourceManager<Texture>& texture_mgr, SwapChain& swap_chain) {
    render_pass_desc->colorAttachments()->object(0)->setTexture(texture_mgr.GetObj(msaa)->texture);
    render_pass_desc->colorAttachments()->object(0)->setResolveTexture(swap_chain.GetDrawable()->texture());
    render_pass_desc->depthAttachment()->setTexture(texture_mgr.GetObj(depth)->texture);
    return true;
}

}  // namespace cairns::rhi

namespace cairns {

inline static constexpr uint32_t kHotArenaMemorySize = 1 << 29;
inline static constexpr uint32_t kPermanentHeapMemorySize = 1 << 30;

class Engine {
public:
    
    using TexHandle = cairns::rhi::Handle<cairns::rhi::Texture>;
    using BufHandle = rhi2::Handle<rhi2::Buffer>;
    using DynBufId = uint32_t;
    using ShaderHandle = rhi2::Handle<rhi2::Shader>;
    using MatId = uint32_t;
    using SamplerHandle = rhi2::Handle<rhi2::Sampler>;
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
    
    bool initGpuAllocators() {
        allocTransientHeap_ = std::make_unique<cairns::rhi::GpuAllocatorHeap>("alloc transient heap", device, MTL::StorageModeShared, kPermanentHeapMemorySize);
        return allocTransientHeap_->Valid();
    }
    
    bool initDevice() {
        device = cairns::rhi::Device(MTL::CreateSystemDefaultDevice());
        return device.get() != nullptr;
    }
    
    bool initSwapChain(SDL_Window* window) {
        swapChain_ = std::make_unique<cairns::rhi::SwapChain>();
        
        if ( !swapChain_->Init(device, window)) {
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
        if ( msaaHandle_ != TexHandle::Null ) {
            auto& obj = *renderPassTexManager_->GetObj(msaaHandle_);
            obj.texture->release();
            obj.texture = nullptr;
        }
        if ( depthHandle_ != TexHandle::Null ) {
            auto& obj = *renderPassTexManager_->GetObj(depthHandle_);
            obj.texture->release();
            obj.texture = nullptr;
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
        renderPassTexManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::Texture>>(hot_arena_, hot_arena_, 2);
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
            rhi2::BackendInitParams rhi2_p;
            rhi2_p.device = device.get();
            rhi2_p.queue = metalCommandQueue;
            if (!rm_.Init(rhi2_p)) {
                return false;
            }
        }
        if ( !initSwapChain(window)) {
            return false;
        }
        if ( !initGpuAllocators() ) {
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
        metalCommandQueue = device.get()->newCommandQueue();
        return metalCommandQueue != nullptr;
    }
    
    bool initDepthAndMSAATextures() {
        {
            msaaHandle_ = renderPassTexManager_->New();
            auto& obj = *renderPassTexManager_->GetObj(msaaHandle_);
            auto& desc = *renderPassTexManager_->GetDesc(msaaHandle_);
            desc.type = static_cast<int64_t>(MTL::TextureType2DMultisample);
            desc.format = static_cast<int64_t>(MTL::PixelFormatBGRA8Unorm);
            desc.width = swapChain_->GetDrawableSize().width;
            desc.height = swapChain_->GetDrawableSize().height;
            desc.sample_count = sampleCount;
            desc.usage = MTL::TextureUsageRenderTarget;
            desc.levels = 1;
            desc.storage = MTL::StorageModeShared;
            if (!allocTransientHeap_->AllocTexture(obj, desc)) {
                return false;
            }
        }
        {
            depthHandle_ = renderPassTexManager_->New();
            auto& obj = *renderPassTexManager_->GetObj(depthHandle_);
            auto& desc = *renderPassTexManager_->GetDesc(depthHandle_);
            desc.type = static_cast<int64_t>(MTL::TextureType2DMultisample);
            desc.format = static_cast<int64_t>(MTL::PixelFormatDepth32Float);
            desc.width = swapChain_->GetDrawableSize().width;
            desc.height = swapChain_->GetDrawableSize().height;
            desc.sample_count = sampleCount;
            desc.usage = MTL::TextureUsageRenderTarget;
            desc.levels = 1;
            desc.storage = MTL::StorageModeShared;
            if ( !allocTransientHeap_->AllocTexture(obj, desc) ) {
                return false;
            }
        }
        return true;
    }
    
    bool initRenderPassDescriptor() {
        if ( !cairns::rhi::InitRenderPassDescriptor(render_pass_descriptor_, msaaHandle_, depthHandle_, *renderPassTexManager_, *swapChain_)) {
            return false;
        }
        return true;
    }
    
    bool initFrameSemaphore() {
        frameSemaphore = dispatch_semaphore_create(kBufferedFrames);
        return true;
    }
    
    bool updateRenderPassDescriptor() {
        if ( cairns::rhi::UpdateRenderPassDescriptor(render_pass_descriptor_, msaaHandle_, depthHandle_, *renderPassTexManager_, *swapChain_)) {
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
                static_cast<uint32_t>(device.GetGpuAlignUboOffset());
            void* gptr = rm_.BumpAllocate(
                sizeof(cairns::rhi::RenderPassGlobals), dyn_align,
                rhi2::Memory::kDynamic);
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
                    const rhi2::Handle<rhi2::Texture> tex_handle = materials_[mat_id].color;
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
                            static_cast<uint32_t>(device.GetGpuAlignUboOffset());
                        void* mptr = rm_.BumpAllocate(
                            sizeof(cairns::rhi::MaterialGpu), dyn_align,
                            rhi2::Memory::kDynamic);
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
                            static_cast<uint32_t>(device.GetGpuAlignUboOffset());
                        void* tptr = rm_.BumpAllocate(
                            sizeof(cairns::rhi::DrawTmp), dyn_align,
                            rhi2::Memory::kDynamic);
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
        
        cairns::Timer timer0("timer 0", 0);
        
        if ( !BuildMeshOpaqueDraws()) {
            return false;
        }
        
        { // sort materials next to each other
            std::sort(drawListSorted_.begin(),drawListSorted_.end());
        }
        
        timer0.End();
        
        cairns::Timer timer1("timer 1", 1);
        MTL::RenderCommandEncoder* encoder = nullptr;
        {
            encoder = cmdBuf->renderCommandEncoder(render_pass_descriptor_);
        }
        
        {
            MTL::RenderPipelineState* pso =
                static_cast<MTL::RenderPipelineState*>(rm_.GetHot(unlit_)->api_pso);

            encoder->setRenderPipelineState(pso);
            encoder->setDepthStencilState(depthStencilState);
            encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
            encoder->setCullMode(MTL::CullModeBack);

            {
                rhi2::BindGroup::Hot* bg_hot = rm_.GetHot(bindless_bg_handle_);
                MTL::Buffer* bg_buf = static_cast<MTL::Buffer*>(bg_hot->api_descriptor_set);
                const uint32_t bg_off = bg_hot->arg_buf_offset;
                encoder->setVertexBuffer(bg_buf, bg_off,
                                         cairns::rhi::GpuSceneRegistry::kBindSlot);
                encoder->setFragmentBuffer(bg_buf, bg_off,
                                           cairns::rhi::GpuSceneRegistry::kBindSlot);
            }
            // use resource call for all textures in argument table
            encoder->useHeap(allocTransientHeap_->GetHeap());
            for (auto& s : scenes_) {
                for (const auto th : s.textureHandles) {
                    MTL::Texture* tex =
                        static_cast<MTL::Texture*>(rm_.GetHot(th)->api_view);
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
                rm_.GetBumpMasterBuffer(rhi2::Memory::kDynamic);
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

        encoder->endEncoding();
        timer1.End();

        cairns::Timer::PrintReport(true);

        if (!dumpPath_.empty()) {
            MTL::Texture* drawableTex = swapChain_->GetDrawable()->texture();
            const NS::UInteger w = drawableTex->width();
            const NS::UInteger h = drawableTex->height();
            const NS::UInteger bytesPerRow = w * 4;
            const NS::UInteger bufSize = bytesPerRow * h;
            MTL::Buffer* readback = device.get()->newBuffer(bufSize, MTL::ResourceStorageModeShared);
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
            return true;
        }

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

            metal_default_library = compileMetalShader(device.get(), cubeShaderPath.string().c_str());
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
            attr0->setBufferIndex(cairns::rhi::ResourceDescriptor<cairns::rhi::Shader>::kMeshPosBindSlot);
            
            MTL::VertexBufferLayoutDescriptor* const layout0 = vertex_desc->layouts()->object(0);
            layout0->setStride(sizeof(glm::vec4));
            layout0->setStepFunction(MTL::VertexStepFunctionPerVertex);
            layout0->setStepRate(1);
            renderPipelineDescriptor->setVertexDescriptor(vertex_desc);
            vertex_desc = nullptr;
        }
        
        NS::Error* error = nullptr;

        MTL::RenderPipelineState* pso =
            device.get()->newRenderPipelineState(renderPipelineDescriptor, &error);

        if (pso == nullptr) {
            std::cout << "Error creating render pipeline state: " << error << std::endl;
            std::exit(0);
        }

        unlit_ = rm_.CreateShader({.api_pso = pso, .debug_name = "unlit"});

        { // bindless resources set up via rhi2
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
            MTL::ArgumentEncoder* arg_encoder = device.get()->newArgumentEncoder(args);

            rhi2::BufferDesc bd;
            bd.byte_size = static_cast<uint32_t>(arg_encoder->encodedLength());
            bd.usage = rhi2::kUsageUniform | rhi2::kUsageStorage;
            bd.memory = rhi2::Memory::kUpload;
            rhi2::Handle<rhi2::Buffer> arg_buf_h = rm_.CreateBuffer(bd);
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
                    MTL::Texture* tex =
                        static_cast<MTL::Texture*>(rm_.GetHot(h)->api_view);
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
                    MTL::SamplerState* samp =
                        static_cast<MTL::SamplerState*>(rm_.GetHot(h)->api_sampler);
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
        depthStencilState = device.get()->newDepthStencilState(depthStencilDescriptor);
        
        renderPipelineDescriptor->release();
        vertexShader->release();
        fragmentShader->release();
        
        return true;
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
    
    cairns::rhi::Device device;
    std::vector<cairns::Scene, cairns::Allocator<cairns::Scene>> scenes_;
    std::vector<int32_t, cairns::Allocator<int32_t>> root_nodes_stack_cache_;
    
    std::vector<glm::mat4> debugSceneXforms_;
    
    cairns::FrameTransientCache<cairns::DynamicBuffersAssoc> dynBufs_;
    std::vector<cairns::LoadedMaterial> materials_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::Texture>> renderPassTexManager_;

    cairns::FrameTransientCache<cairns::BindGroupAssoc> bindGroups_;

    std::vector<std::pair<cairns::DrawKey,uint32_t>,cairns::Allocator<std::pair<cairns::DrawKey,uint32_t>>> drawListSorted_;
    std::vector<cairns::Draw,cairns::Allocator<cairns::Draw>> drawList_;
    
    rhi2::ResourceManager rm_;
    MTL::Buffer* mesh_master_buf_ = nullptr;
    rhi2::Handle<rhi2::BindGroup> bindless_bg_handle_;
    std::unordered_map<uint32_t, uint32_t> mesh_attr_id_map_;
    std::unordered_map<uint32_t, uint32_t> sampler_id_map_;

    std::unique_ptr<cairns::rhi::GpuAllocatorHeap> allocTransientHeap_;
    
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

