#pragma once

#include <cmath>
#include <string_view>
#include <filesystem>
#include <thread>
#include <fstream>
#include <iostream>
#include <numbers>
#include <variant>

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
#include "util/timer.hpp"
#include "util/unique_ptr.hpp"

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

bool LoadTextureGpu([[maybe_unused]] Device& device, GpuAllocatorHeap& alloc_transient_heap, ResourceManager<Texture>& manager, Handle<Texture> handle) {
    ResourceObject<Texture>& hot = *manager.GetObj(handle);
    ResourceDescriptor<Texture>& cold = *manager.GetDesc(handle);
    if (!cold.src_image) return false;

    // todo @iamies dont hardcode this
    cold.storage = static_cast<int64_t>(MTL::StorageModeShared);
    if ( !alloc_transient_heap.AllocTexture(hot, cold, cold.undecorated_filename)) {
        return false;
    }

    if (!hot.texture) return false;

    MTL::Region region = MTL::Region(0, 0, 0, cold.width, cold.height, 1);
    hot.texture->replaceRegion(region, 0, cold.src_image, cold.src_bytes_per_row);
    return true;
}

bool LoadMeshGpu(Device& device, GpuAllocator& alloc, ResourceManager<Buffer>& mgr, Mesh& mesh) {
    auto process = [&](Handle<Buffer> h, const void* srcData, size_t srcSize) -> bool {
        ResourceObject<Buffer>& hot = *mgr.GetObj(h);
        ResourceDescriptor<Buffer>& cold = *mgr.GetDesc(h);
        if (srcSize == 0) return true; // Valid empty buffer

        const int64_t align = device.GetGpuAlignUboOffset();
        hot.mem = alloc.Alloc(cold.GetBytes(), align);
        hot.buffer = alloc.GetBuffer();

        if (hot.mem == OffsetAllocator::BadAllocation) return false;

        uint8_t* dst = alloc.GetCpuAddress(hot.mem.offset);
        std::memcpy(dst, srcData, srcSize);
        return true;
    };

    if (!process(mesh.posHandle, mesh.cpuPositions.data(), mesh.cpuPositions.size() * sizeof(glm::vec4))) return false;
    if (!process(mesh.attrHandle, mesh.cpuAttrs.data(), mesh.cpuAttrs.size() * sizeof(VertexAttribute))) return false;
    if (!process(mesh.indexHandle, mesh.cpuIndices.data(), mesh.cpuIndices.size() * sizeof(uint32_t))) return false;

    return true;
}

bool LoadSamplerGpu(Device& device, ResourceManager<Sampler>& sampler_mgr, Handle<Sampler> h) {
    // samplers
    MTL::SamplerDescriptor* desc = MTL::SamplerDescriptor::alloc()->init();
    auto& info = *sampler_mgr.GetDesc(h);
    desc->setSupportArgumentBuffers(true);
    desc->setMinFilter(metal::MapMinMag(info.minFilter));
    desc->setMagFilter(metal::MapMinMag(info.magFilter));
    switch(info.mipFilter) {
        case SamplerMipFilter::None:
            desc->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
            break;
        case SamplerMipFilter::Nearest:
            desc->setMipFilter(MTL::SamplerMipFilterNearest);
            break;
        case SamplerMipFilter::Linear:
            desc->setMipFilter(MTL::SamplerMipFilterLinear);
            break;
    }
    
    auto translateWrap = [](SamplerAddressMode m) {
        if (m == SamplerAddressMode::ClampToEdge) return MTL::SamplerAddressModeClampToEdge;
        if (m == SamplerAddressMode::MirroredRepeat) return MTL::SamplerAddressModeMirrorRepeat;
        return MTL::SamplerAddressModeRepeat;
    };
    desc->setSAddressMode(translateWrap(info.addressModeU));
    desc->setTAddressMode(translateWrap(info.addressModeV));
    
    //////////////////////////////////////////////////////////////////
    // WARNING: IAMIES YOU ARE TAKING AN FPT HIT FOR CLEARER IMAGES //
    // the internet says you should use it and I'm using it because the shimmering is annoying me
     desc->setMaxAnisotropy(8);
    //////////////////////////////////////////////////////////////////
    
    auto& obj = *sampler_mgr.GetObj(h);
    obj.sampler_state = device.get()->newSamplerState(desc);
    desc->release();
    
    return true;
}

bool LoadSceneGpu(Device& device,
                 GpuAllocator& alloc_unified, GpuAllocatorHeap& alloc_transient_heap,
                 Scene& scene,
                 ResourceManager<Buffer>& bufMgr, ResourceManager<Texture>& texMgr, ResourceManager<Sampler>& sampler_mgr)
{
    // 1. Load Samplers
    for ( const auto& h : scene.samplerHandles ) {
        if ( !LoadSamplerGpu(device, sampler_mgr, h)) {
            return false;
        }
    }
    
    for ( size_t i = 0; i < scene.meshes.size(); ++i ) {
        auto& mesh = scene.meshes[i];
        if ( !LoadMeshGpu(device, alloc_unified, bufMgr, mesh) ) {
            return false;
        }
    }
    
    for ( size_t i = 0; i < scene.textureHandles.size(); ++i ) {
        const auto h = scene.textureHandles[i];
        if (!LoadTextureGpu(device, alloc_transient_heap, texMgr, h)) {
            return false;
        }
    }

    return true;
}

bool InitRenderPassDescriptor(Handle<RenderPass> render_pass, Handle<Texture> msaa, Handle<Texture> depth,
                              ResourceManager<RenderPass>& render_pass_mgr, ResourceManager<Texture>& tex_mgr, SwapChain& swap_chain) {
    auto& desc = *render_pass_mgr.GetDesc(render_pass);
    auto*& renderPassDescriptor = desc.render_pass_descriptor;
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

bool UpdateRenderPassDescriptor(Handle<RenderPass> render_pass, Handle<Texture> msaa, Handle<Texture> depth, ResourceManager<RenderPass>& render_pass_mgr, ResourceManager<Texture>& texture_mgr, SwapChain& swap_chain) {
    auto& desc = *render_pass_mgr.GetDesc(render_pass);
    MTL::RenderPassDescriptor* render_pass_desc = desc.render_pass_descriptor;
    render_pass_desc->colorAttachments()->object(0)->setTexture(texture_mgr.GetObj(msaa)->texture);
    render_pass_desc->colorAttachments()->object(0)->setResolveTexture(swap_chain.GetDrawable()->texture());
    render_pass_desc->depthAttachment()->setTexture(texture_mgr.GetObj(depth)->texture);
    return true;
}

}  // namespace cairns::rhi

namespace cairns {

inline static constexpr uint32_t kHotArenaMemorySize = 1 << 29;
inline static constexpr uint32_t kTransientLinearMemorySize = 1 << 25; // 32 mb
inline static constexpr uint32_t kPermanentLinearMemorySize = 1 << 29; // 512 mb
inline static constexpr uint32_t kPermanentHeapMemorySize = 1 << 30; // 1 gb
inline static constexpr uint32_t kMaxDrawTmpsPerFrame = 16384;
inline static constexpr uint32_t kMaxMaterialBuffersPerFrame = 16384;

class Engine {
public:
    
    using TexHandle = cairns::rhi::Handle<cairns::rhi::Texture>;
    using BufHandle = cairns::rhi::Handle<cairns::rhi::Buffer>;
    using DbufHandle = cairns::rhi::Handle<cairns::rhi::DynamicBuffers>;
    using ShaderHandle = cairns::rhi::Handle<cairns::rhi::Shader>;
    using RenderPassHandle = cairns::rhi::Handle<cairns::rhi::RenderPass>;
    using MatHandle = cairns::rhi::Handle<cairns::rhi::Material>;
    using SamplerHandle = cairns::rhi::Handle<cairns::rhi::Sampler>;
    using BindGroupHandle = cairns::rhi::Handle<cairns::rhi::BindGroup>;
    
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
        // give it some extra padding just in case
        allocTransientLinear1_ = std::make_unique<cairns::rhi::GpuAllocator>("alloc transient linear", device, cairns::rhi::GpuAllocator::Category::kCat1, kTransientLinearMemorySize);
        allocTransientLinear2_ = std::make_unique<cairns::rhi::GpuAllocator>("alloc transient linear 2", device, cairns::rhi::GpuAllocator::Category::kCat1, kPermanentLinearMemorySize);
        allocTransientHeap_ = std::make_unique<cairns::rhi::GpuAllocatorHeap>("alloc transient heap", device, MTL::StorageModeShared, kPermanentHeapMemorySize);
        const bool success = allocTransientLinear1_->Valid() && allocTransientLinear2_->Valid() && allocTransientHeap_->Valid();
        return success;
    }
    
    bool initBufferManagers() {
        const int64_t buffers_align = device.GetGpuAlignUboOffset();
        { // init render pass globals
            for ( uint32_t i = 0; i < kBufferedFrames; ++i ) {
                cairns::OffsetAllocator::Allocation mem = allocTransientLinear1_->Alloc(sizeof(cairns::rhi::RenderPassGlobals), buffers_align);
                BufHandle dh = bufferManager_->New();
                auto* obj = bufferManager_->GetObj(dh);
                obj->buffer = allocTransientLinear1_->GetBuffer();
                obj->mem = mem;
                renderPassGlobals_.push_back(dh);
            }
        }
        { // init draw tmps
            drawTmpIdxs_.resize(kBufferedFrames);
            drawTmps_.resize(kBufferedFrames);
            // only ubos for now
            const int64_t draw_tmp_size = sizeof(cairns::rhi::DrawTmp);
            // "On mobile uniform buffers have 16KB binding size limitation." sl27/pg42
            assert(draw_tmp_size <= 1 << 16);
            for ( uint32_t i = 0; i < kBufferedFrames; ++i ) {
                for ( uint32_t j = 0; j < kMaxDrawTmpsPerFrame; ++j ) {
                    cairns::OffsetAllocator::Allocation mem = allocTransientLinear1_->Alloc(draw_tmp_size, buffers_align);
                    assert(mem != cairns::OffsetAllocator::BadAllocation);
                    BufHandle dh = bufferManager_->New();
                    auto* obj = bufferManager_->GetObj(dh);
                    obj->buffer = allocTransientLinear1_->GetBuffer();
                    obj->mem = mem;
                    drawTmps_[i].push_back(dh);
                }
            }
        }
        { // init material transfer buffers
            materialBufferIdxs_.resize(kBufferedFrames);
            materialBuffers_.resize(kBufferedFrames);
            const int64_t material_gpu_size = sizeof(cairns::rhi::MaterialGpu);
            for ( uint32_t i = 0; i < kBufferedFrames; ++i ) {
                for ( uint32_t j = 0; j < kMaxMaterialBuffersPerFrame; ++j ) {
                    cairns::OffsetAllocator::Allocation mem = allocTransientLinear1_->Alloc(material_gpu_size, buffers_align);
                    assert( mem != cairns::OffsetAllocator::BadAllocation);
                    BufHandle h = materialBufferManager_->New();
                    auto* obj = materialBufferManager_->GetObj(h);
                    obj->buffer = allocTransientLinear1_->GetBuffer();
                    obj->mem = mem;
                    materialBuffers_[i].push_back(h);
                }
            }
        }
        return true;
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
    
    void resetFrameTmps(uint32_t frame) {
        drawTmpIdxs_[frame] = 0;
        dbufCacheIdx_ = 0;
        bindGroupCacheIdx_ = 0;
        materialBufferIdxs_[frame] = 0;
    }
    
    bool requestResizeFrameBuffer(uint32_t width, uint32_t height) {
        resizeFrameBufferRequest_ = ResizeFrameBufferRequest{.width = width, .height = height};
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
        texManager_ = cairns::make_unique<ResourceManager<Texture>>(hot_arena_, hot_arena_, 1024);
        renderPassTexManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::Texture>>(hot_arena_, hot_arena_, 2);
        bufferManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::Buffer>>(hot_arena_, hot_arena_, 1024);
        const uint32_t estUbos = kBufferedFrames * kMaxDrawTmpsPerFrame;
        dynamicBuffersManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::DynamicBuffers>>(hot_arena_, hot_arena_, estUbos);
        // 4 because we're only pretending to be a real UGC engine at this point
        samplerManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::Sampler>>(hot_arena_, hot_arena_, 256);
        shaderManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::Shader>>(hot_arena_, hot_arena_, 1);
        renderPassManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::RenderPass>>(hot_arena_, hot_arena_, 1);
        materialBufferManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::Buffer>>(hot_arena_, hot_arena_, 1024);
        materialManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::Material>>(hot_arena_, hot_arena_, 1024);
        bindGroupManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::BindGroup>>(hot_arena_, hot_arena_, 1024);
        dynamicBuffersManager_ = cairns::make_unique<cairns::rhi::ResourceManager<cairns::rhi::DynamicBuffers>>(hot_arena_, hot_arena_, 1024);
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
        if ( !initSwapChain(window)) {
            return false;
        }
        if ( !initGpuAllocators() ) {
            return false;
        }
        if ( !initBufferManagers() ) {
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
                cairns::PrepareSceneResources(device, scene, *bufferManager_, *texManager_, *samplerManager_, *materialManager_);
                
                if (!cairns::rhi::LoadSceneGpu(device, *allocTransientLinear2_, *allocTransientHeap_, scene, *bufferManager_, *texManager_, *samplerManager_)) {
                    return false;
                }
                
                scene.CleanupTmps();
            }
            MTL::CommandBuffer* cmd = metalCommandQueue->commandBuffer();
            MTL::BlitCommandEncoder* blit = cmd->blitCommandEncoder();
            for ( size_t i = 0; i < scenes_.size(); ++i ) {
                cairns::Scene& scene = scenes_[i];
                for ( size_t k = 0; k < scene.textureHandles.size(); ++k ) {
                    const auto h = scene.textureHandles[k];
                    MTL::Texture* tex = texManager_->GetObj(h)->texture;
                    blit->generateMipmaps(tex);
                }
            }
            blit->endEncoding();
            cmd->commit();
            cmd->waitUntilCompleted();
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
    DbufHandle getDynamicBuffers() {
        if ( dbufCacheIdx_ < dbufsCache_.size() ) {
            return dbufsCache_[dbufCacheIdx_++];
        }
        else {
            auto h = dynamicBuffersManager_->New();
            dbufsCache_.push_back(h);
            ++dbufCacheIdx_;
            return h;
        }
    }
    
    BindGroupHandle getBindGroup() {
        if ( bindGroupCacheIdx_ < bindGroupsCache_.size()) {
            return bindGroupsCache_[bindGroupCacheIdx_++];
        }
        else {
            auto h = bindGroupManager_->New();
            bindGroupsCache_.push_back(h);
            ++bindGroupCacheIdx_;
            return h;
        }
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
        if ( renderPass_ == RenderPassHandle::Null ) {
            renderPass_ = renderPassManager_->New();
        }
        if ( !cairns::rhi::InitRenderPassDescriptor(renderPass_, msaaHandle_, depthHandle_, *renderPassManager_, *texManager_, *swapChain_)) {
            return false;
        }
        return true;
    }
    
    bool initFrameSemaphore() {
        frameSemaphore = dispatch_semaphore_create(kBufferedFrames);
        return true;
    }
    
    bool updateRenderPassDescriptor() {
        if ( cairns::rhi::UpdateRenderPassDescriptor(renderPass_, msaaHandle_, depthHandle_, *renderPassManager_, *renderPassTexManager_, *swapChain_)) {
            return false;
        }
        return true;
    }
    
    bool BuildMeshOpaqueDraws(uint32_t frame) {
        drawList_.clear();
        drawListSorted_.clear();
        
        const float angle_degs = SDL_GetTicks() / 1000.0 / 2.0 * 45;
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
        auto& shader_obj = *shaderManager_->GetObj(shader);
        auto& gpu_scene_registry = shader_obj.gpu_scene_registry;
        
        const BindGroupHandle bg_globals = getBindGroup();
        auto& glob_obj = *bindGroupManager_->GetObj(bg_globals);
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
            BufHandle globals_handle = renderPassGlobals_[frame];
            // todo @iamies use an actual RHI abstraction like GetUnifiedAddr(ResourceObject<Buffer>);
            auto* buffer = bufferManager_->GetObj(globals_handle)->buffer;
            cairns::OffsetAllocator::Allocation mem = bufferManager_->GetObj(globals_handle)->mem;
            void* addr = reinterpret_cast<uint8_t*>(buffer->contents()) + mem.offset;
            memcpy(addr, &render_pass_globals, sizeof(render_pass_globals));
            glob_obj.type = cairns::rhi::BindGroup::Type::kRenderPassGlobal;
            glob_obj.globals = globals_handle;
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
                    const MatHandle mat_handle = scene.materialHandles[scene_mat_idx];
                    const TexHandle tex_handle = materialManager_->GetObj(mat_handle)->color;
                    const SamplerHandle sampler_handle = materialManager_->GetObj(mat_handle)->sampler;
                    
                    const uint32_t gpu_tex_id = tex_handle.get_id();
                    const uint32_t gpu_sampler_id = gpu_scene_registry.sampler_id[sampler_handle.get_id()];
                    const uint32_t gpu_attr_idx = gpu_scene_registry.attr_id[mesh.attrHandle.get_id()];
                    
                    //                    cairns::Timer timer7("timer7", 7);
                    const BindGroupHandle bg_material = getBindGroup();
                    auto& mat_obj = *bindGroupManager_->GetObj(bg_material);
                    { // material set up
                        const cairns::rhi::MaterialGpu material_gpu {
                            .tex_color_id = gpu_tex_id,
                            .sampler_id = gpu_sampler_id,
                        };
                        const uint32_t material_buf_idx = materialBufferIdxs_[frame]++;
                        assert(material_buf_idx < kMaxMaterialBuffersPerFrame);
                        const BufHandle h = materialBuffers_[frame][material_buf_idx];
                        // todo @iamies use an actual RHI abstraction like GetUnifiedAddr(ResourceObject<Buffer>);
                        MTL::Buffer* const material_buffer = materialBufferManager_->GetObj(h)->buffer;
                        const cairns::OffsetAllocator::Allocation material_mem = materialBufferManager_->GetObj(h)->mem;
                        void* const material_addr = reinterpret_cast<uint8_t*>(material_buffer->contents()) + material_mem.offset;
                        memcpy(material_addr, &material_gpu, sizeof(material_gpu));
                        mat_obj.type = cairns::rhi::BindGroup::Type::kMaterial;
                        mat_obj.material_buffer = h;
                        mat_obj.material = mat_handle;
                    }
                    
                    //                    cairns::Timer timer8("timer8", 8);
                    const DbufHandle tmp_handle = getDynamicBuffers();
                    auto& tmp_obj = *dynamicBuffersManager_->GetObj(tmp_handle);
                    const glm::mat4 model_matrix = scene_xform * rot_matrix;
                    { // tmp draws set up
                        const cairns::rhi::DrawTmp draw_tmp {
                            .model_matrix = node.globalTransform * model_matrix,
                            .mesh_id = gpu_attr_idx,
                            .tex_id = gpu_tex_id,
                            .sampler_id = gpu_sampler_id
                        };
                        const uint32_t draw_tmp_idx = drawTmpIdxs_[frame]++;
                        assert(draw_tmp_idx < kMaxDrawTmpsPerFrame);
                        const BufHandle h = drawTmps_[frame][draw_tmp_idx];
                        // todo @iamies use an actual RHI abstraction like GetUnifiedAddr(ResourceObject<Buffer>);
                        MTL::Buffer* const draw_tmp_buffer = bufferManager_->GetObj(h)->buffer;
                        const cairns::OffsetAllocator::Allocation draw_tmp_mem = bufferManager_->GetObj(h)->mem;
                        void* const draw_tmp_addr = reinterpret_cast<uint8_t*>(draw_tmp_buffer->contents()) + draw_tmp_mem.offset;
                        memcpy(draw_tmp_addr, &draw_tmp, sizeof(draw_tmp));
                        tmp_obj.type = cairns::rhi::DynamicBuffers::Type::kTmp;
                        tmp_obj.buf = h;
                    }
                    
                    //                    cairns::Timer timer9("timer9", 9);
                    cairns::Draw draw;
                    draw.shader = shader;
                    // todo @iamies
                    // should this be -1??
                    // did I mess up making vertex attributes bind slot 0?
                    draw.bind_groups[cairns::kRenderPassGlobalBindSlot-1] = bg_globals;
                    draw.bind_groups[cairns::kMaterialBindSlot-1] = bg_material;
                    draw.bind_groups[cairns::kShaderSpecificBindSlot-1] = BindGroupHandle::Null;
                    draw.dynamic_buffers = tmp_handle;
                    draw.index_buffer = index;
                    draw.index_offset = bufferManager_->GetObj(index)->mem.offset + (prim.firstIndex * sizeof(uint32_t));
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
        const uint32_t frame = frame_++ % kBufferedFrames;
        
        dispatch_semaphore_wait(frameSemaphore, DISPATCH_TIME_FOREVER);
        
        resetFrameTmps(frame);
        
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
        
        if ( !BuildMeshOpaqueDraws(frame)) {
            return false;
        }
        
        { // sort materials next to each other
            std::sort(drawListSorted_.begin(),drawListSorted_.end());
        }
        
        timer0.End();
        
        cairns::Timer timer1("timer 1", 1);
        MTL::RenderCommandEncoder* encoder = nullptr;
        {
            auto& desc = *renderPassManager_->GetDesc(renderPass_);
            encoder = cmdBuf->renderCommandEncoder(desc.render_pass_descriptor);
        }
        
        {
            auto& shader_obj = *shaderManager_->GetObj(unlit_);
            auto& pso = shader_obj.pso;
            auto& scene_registry_handle = shader_obj.scene_registry_handle;
            
            encoder->setRenderPipelineState(pso);
            encoder->setDepthStencilState(depthStencilState);
            encoder->setFrontFacingWinding(MTL::WindingCounterClockwise);
            encoder->setCullMode(MTL::CullModeBack);
            
            // set up argument table
            auto* regObj = bufferManager_->GetObj(scene_registry_handle);
            encoder->setVertexBuffer(regObj->buffer, regObj->mem.offset, cairns::rhi::GpuSceneRegistry::kBindSlot);
            encoder->setFragmentBuffer(regObj->buffer, regObj->mem.offset, cairns::rhi::GpuSceneRegistry::kBindSlot);
            // use resource call for all textures in argument table
            encoder->useHeap(allocTransientHeap_->GetHeap());
            // use resource call for all vertex attributes in argument table
            encoder->useResource(allocTransientLinear2_->GetBuffer(), MTL::ResourceUsageRead, MTL::RenderStageVertex);
            
            // set up position vertex buffer in vertex shader
            // all vertex buffer positions live in allocTransientLinear2_
            // todo @iamies don't hard code this.
            // ... also, did this just consume one of your buffer binding slots?
            encoder->setVertexBuffer(allocTransientLinear2_->GetBuffer(), 0, 0 /* hard coded? */);
            // set up render pass globals bind group in vertex shader
            encoder->setVertexBuffer(allocTransientLinear1_->GetBuffer(), 0, cairns::kRenderPassGlobalBindSlot);
            // set up material buffer bind group in vertex shader
            encoder->setVertexBuffer(allocTransientLinear1_->GetBuffer(), 0, cairns::kMaterialBindSlot);
            // set up shader specific buffers bind group in vertex shader
            // none
            // set up per draw temporaries bind group in vertex shader
            encoder->setVertexBuffer(allocTransientLinear1_->GetBuffer(), 0, cairns::kDrawTmpBindSlot);
            
            MatHandle last_mat = MatHandle::Null;
            uint32_t triangles = 0;
            for ( size_t draw_idx = 0; draw_idx < drawListSorted_.size(); ++draw_idx ) {
                const cairns::Draw& draw = drawList_[drawListSorted_[draw_idx].second];
                { // set position buffer offset
                    const BufHandle pos = draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot];
                    auto& pos_obj = *bufferManager_->GetObj(pos);
                    const cairns::OffsetAllocator::Allocation pos_mem = pos_obj.mem;
                    encoder->setVertexBufferOffset(pos_mem.offset, 0 /*hard coded for some reason*/);
                }
                { // set up material
                    const BindGroupHandle mat_bg = draw.bind_groups[cairns::kMaterialBindSlot-1];
                    auto& mat_bg_obj = *bindGroupManager_->GetObj(mat_bg);
                    const MatHandle mat = mat_bg_obj.material;
                    if ( mat != last_mat ) {
                        last_mat = mat;
                        const BufHandle material_buffer = mat_bg_obj.material_buffer;
                        cairns::OffsetAllocator::Allocation mat_mem = materialBufferManager_->GetObj(material_buffer)->mem;
                        encoder->setVertexBufferOffset(mat_mem.offset, cairns::kMaterialBindSlot);
                    }
                }
                { // set up draw temporary
                    const DbufHandle draw_tmp_bg = draw.dynamic_buffers;
                    auto& draw_tmp_obj = *dynamicBuffersManager_->GetObj(draw_tmp_bg);
                    const BufHandle draw_tmp_buf = draw_tmp_obj.buf;
                    const cairns::OffsetAllocator::Allocation draw_tmp_mem = bufferManager_->GetObj(draw_tmp_buf)->mem;
                    encoder->setVertexBufferOffset(draw_tmp_mem.offset, cairns::kDrawTmpBindSlot);
                }
                {
                    const uint32_t index_count = draw.triangle_count * 3;
                    const uint32_t index_offset = draw.index_offset;
                    const uint32_t vertex_offset = draw.vertex_offset;
                    const BufHandle index = draw.index_buffer;
                    MTL::Buffer* const index_buffer = bufferManager_->GetObj(index)->buffer;
                    assert(index_buffer == allocTransientLinear2_->GetBuffer());
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
        
        // 5. Present and Commit
        cmdBuf->presentDrawable(swapChain_->GetDrawable());
        cmdBuf->commit();
        
        return true;
    }
    
    bool initRenderPipeline() {
        unlit_ = shaderManager_->New();
        auto& obj = *shaderManager_->GetObj(unlit_);
        auto& desc = *shaderManager_->GetDesc(unlit_);
        
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
            
            auto& metal_default_library = shaderManager_->GetDesc(unlit_)->metal_default_library;
            metal_default_library = compileMetalShader(device.get(), cubeShaderPath.string().c_str());
        }
        
        auto& metal_default_library = desc.metal_default_library;
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
        
        auto*& pso = obj.pso;
        
        pso = device.get()->newRenderPipelineState(renderPipelineDescriptor, &error);
        
        if (pso == nullptr) {
            std::cout << "Error creating render pipeline state: " << error << std::endl;
            std::exit(0);
        }
        
        cairns::rhi::GpuSceneRegistry& gpu_scene_registry = obj.gpu_scene_registry;
        gpu_scene_registry.attr_id.resize(bufferManager_->GetCapacity());
        gpu_scene_registry.sampler_id.resize(samplerManager_->GetCapacity());
        
        { // bindless resources set up
            // I picked 10 just because. This must correspond with the shader.
            auto* texArg = MTL::ArgumentDescriptor::alloc()->init();
            texArg->setDataType(MTL::DataTypeTexture);
            texArg->setIndex(cairns::rhi::GpuSceneRegistry::kTexturesSlotOffset);
            texArg->setArrayLength(cairns::rhi::GpuSceneRegistry::kMaxTextures);
            texArg->setAccess(MTL::ArgumentAccessReadOnly);
            
            // 2. Create the descriptor for vertex attribute pointers (device pointers)
            auto* attrArg = MTL::ArgumentDescriptor::alloc()->init();
            attrArg->setDataType(MTL::DataTypePointer); // Use Pointer for device VertexAttribute*
            attrArg->setIndex(cairns::rhi::GpuSceneRegistry::kMeshesSlotOffset);
            attrArg->setArrayLength(cairns::rhi::GpuSceneRegistry::kMaxMeshes);
            attrArg->setAccess(MTL::ArgumentAccessReadOnly);
            
            // 3. Create the descriptor for samplers
            auto* sampArg = MTL::ArgumentDescriptor::alloc()->init();
            sampArg->setDataType(MTL::DataTypeSampler);
            sampArg->setIndex(cairns::rhi::GpuSceneRegistry::kSamplersSlotOffset);
            sampArg->setArrayLength(cairns::rhi::GpuSceneRegistry::kMaxSamplers);
            sampArg->setAccess(MTL::ArgumentAccessReadOnly);
            
            NS::Array* args = NS::Array::array((NS::Object*[]){ texArg, attrArg, sampArg }, 3);
            MTL::ArgumentEncoder* arg_encoder = device.get()->newArgumentEncoder(args);
            
            const int64_t size = arg_encoder->encodedLength();
            const int64_t align = arg_encoder->alignment();
            // the argument table is basically a small SSBO
            // it shares all the same semantics and lifecycle ideas as a small SSBO.
            BufHandle& scene_registry_handle = obj.scene_registry_handle;
            scene_registry_handle = bufferManager_->New();
            auto* obj2 = bufferManager_->GetObj(scene_registry_handle);
            obj2->mem = allocTransientLinear1_->Alloc(size, align);
            obj2->buffer =  allocTransientLinear1_->GetBuffer();
            
            arg_encoder->setArgumentBuffer(allocTransientLinear1_->GetBuffer(), obj2->mem.offset);
            
            for ( size_t i = 0; i < scenes_.size(); ++i ) {
                cairns::Scene& scene = scenes_[i];
                for ( size_t j = 0; j < scene.textureHandles.size(); ++j ) {
                    auto h = scene.textureHandles[j];
                    auto* obj = texManager_->GetObj(h);
                    if ( obj && obj->texture) {
                        uint32_t& num_tex = gpu_scene_registry.num_tex;
                        // IN ALL CASES, THE SCENE'S TEXTURE HANDLE MANAGER SHOULD BE 1-1 WITH THE PSO SCENE REGISTRY
                        assert(h.get_id() == num_tex);
                        arg_encoder->setTexture(obj->texture, cairns::rhi::GpuSceneRegistry::kTexturesSlotOffset + num_tex);
                        //                        gpu_scene_registry.tex_id[h.get_id()] = static_cast<uint32_t>(num_tex);
                        ++num_tex;
                    }
                }
                
                for ( size_t j = 0; j < scene.meshes.size(); ++j ) {
                    auto h = scene.meshes[j].attrHandle;
                    auto* obj = bufferManager_->GetObj(h);
                    if ( obj && obj->buffer ) {
                        uint32_t& num_attr = gpu_scene_registry.num_attr;
                        arg_encoder->setBuffer(obj->buffer, obj->mem.offset, cairns::rhi::GpuSceneRegistry::kMeshesSlotOffset + num_attr);
                        gpu_scene_registry.attr_id[h.get_id()] = static_cast<uint32_t>(num_attr);
                        ++num_attr;
                    }
                }
                
                for (size_t j = 0; j < scene.samplerHandles.size(); ++j) {
                    auto h = scene.samplerHandles[j];
                    auto* obj = samplerManager_->GetObj(h);
                    uint32_t& num_sampler = gpu_scene_registry.num_sampler;
                    arg_encoder->setSamplerState(obj->sampler_state, cairns::rhi::GpuSceneRegistry::kSamplersSlotOffset + num_sampler);
                    gpu_scene_registry.sampler_id[h.get_id()] = static_cast<uint32_t>(num_sampler);
                    ++num_sampler;
                }
            }
            
            arg_encoder->release();
            arg_encoder = nullptr;
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
    
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::Texture>> texManager_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::Buffer>> bufferManager_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::DynamicBuffers>> dynamicBuffersManager_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::BindGroup>> bindGroupManager_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::Sampler>> samplerManager_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::Material>> materialManager_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::Buffer>> materialBufferManager_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::Texture>> renderPassTexManager_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::Shader>> shaderManager_;
    cairns::unique_ptr<cairns::rhi::ResourceManager<cairns::rhi::RenderPass>> renderPassManager_;
    
    std::vector<BufHandle> renderPassGlobals_;
    std::vector<uint32_t> drawTmpIdxs_;
    std::vector<std::vector<BufHandle>> drawTmps_;
    
    std::vector<uint32_t> materialBufferIdxs_;
    std::vector<std::vector<BufHandle>> materialBuffers_;
    
    uint32_t dbufCacheIdx_ = 0;
    std::vector<DbufHandle> dbufsCache_;
    
    uint32_t bindGroupCacheIdx_ = 0;
    std::vector<BindGroupHandle> bindGroupsCache_;
    
    std::vector<std::pair<cairns::DrawKey,uint32_t>,cairns::Allocator<std::pair<cairns::DrawKey,uint32_t>>> drawListSorted_;
    std::vector<cairns::Draw,cairns::Allocator<cairns::Draw>> drawList_;
    
    std::unique_ptr<cairns::rhi::GpuAllocator> allocTransientLinear1_;
    std::unique_ptr<cairns::rhi::GpuAllocator> allocTransientLinear2_;
    std::unique_ptr<cairns::rhi::GpuAllocatorHeap> allocTransientHeap_;
    
    //    std::vector<std::vector<std::function<void(void)>>> deletionRequests_;
    //    std::vector<std::function<void(void)>> deletions_;
    
    struct ResizeFrameBufferRequest {
        uint32_t width = std::numeric_limits<uint32_t>::max();
        uint32_t height = std::numeric_limits<uint32_t>::max();
    };
    std::optional<ResizeFrameBufferRequest> resizeFrameBufferRequest_ = std::nullopt;
    
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
    RenderPassHandle renderPass_ = RenderPassHandle::Null;
    // fences and semaphores
    dispatch_semaphore_t frameSemaphore;
};

} // namespace cairns

