// rhi/webgpu/command_recorder.cpp -- WebGPU backend (W2 stubs; real recording
// in W4: BeginRenderPass/DrawMeshes/EndRenderPass + DrawImGui).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/command_recorder.hpp"
#include "rhi/resources.hpp"
#include "rhi/allocator.hpp"

#include <cstdio>

#include <webgpu/webgpu.h>

namespace cairns::rhi {

void CommandRecorder::Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d) {
    (void)res; (void)alloc; (void)d;
}
void CommandRecorder::DispatchSkinBatches(Resources& res, Allocator& alloc, Handle<Kernel> kernel,
                                          Handle<Buffer> output_pool_buffer, Handle<Buffer> palette_buf,
                                          Handle<DynamicBuffers> dyn_set_0,
                                          std::span<const SkinDispatchBatch> batches) {
    (void)res; (void)alloc; (void)kernel; (void)output_pool_buffer; (void)palette_buf;
    (void)dyn_set_0; (void)batches;
}
void CommandRecorder::DispatchAnimEval(Resources& res, Allocator& alloc, Handle<Kernel> kernel,
                                       const AnimEvalArgs& args) {
    (void)res; (void)alloc; (void)kernel; (void)args;
}
void CommandRecorder::BeginRenderPass(Resources& res, const SwapResolveTarget& target,
                                      const RenderPassDesc& desc,
                                      std::span<const ResourceBarrier> invalidate) {
    (void)invalidate;
    if (plat.enc_) { wgpuRenderPassEncoderEnd(plat.enc_); plat.enc_ = nullptr; }
    if (!plat.cmd_) { return; }

    WGPURenderPassColorAttachment ca[GraphicsPipelineDesc::kMaxColorFormats] = {};
    uint32_t color_count = 0;
    for (size_t i = 0; i < desc.color.size() &&
                       color_count < GraphicsPipelineDesc::kMaxColorFormats; ++i) {
        Texture::Hot* hot = desc.color[i].target.IsNull()
                                ? nullptr : res.GetHot(desc.color[i].target);
        if (!hot) { continue; }
        WGPURenderPassColorAttachment& a = ca[color_count++];
        a.view = static_cast<WGPUTextureView>(hot->api_view);
        a.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        a.loadOp = desc.color[i].load == LoadOp::kLoad ? WGPULoadOp_Load
                                                       : WGPULoadOp_Clear;
        a.storeOp = WGPUStoreOp_Store;
        a.clearValue = {desc.color[i].clear[0], desc.color[i].clear[1],
                        desc.color[i].clear[2], desc.color[i].clear[3]};
    }
    // No graph color target -> the swap pass writes the SwapResolveTarget view
    // (headless final_target_, or the surface texture when windowed).
    if (color_count == 0) {
        WGPUTextureView sv = static_cast<WGPUTextureView>(target.plat.view);
        if (!sv) { return; }
        WGPURenderPassColorAttachment& a = ca[color_count++];
        a.view = sv;
        a.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        a.loadOp = (!desc.color.empty() && desc.color[0].load == LoadOp::kLoad)
                       ? WGPULoadOp_Load : WGPULoadOp_Clear;
        a.storeOp = WGPUStoreOp_Store;
        if (!desc.color.empty()) {
            a.clearValue = {desc.color[0].clear[0], desc.color[0].clear[1],
                            desc.color[0].clear[2], desc.color[0].clear[3]};
        }
    }

    WGPURenderPassDepthStencilAttachment ds = {};
    bool has_depth = false;
    if (!desc.depth.depth.IsNull()) {
        if (Texture::Hot* dh = res.GetHot(desc.depth.depth)) {
            ds.view = static_cast<WGPUTextureView>(dh->api_view);
            ds.depthLoadOp = desc.depth.load == LoadOp::kLoad ? WGPULoadOp_Load
                                                              : WGPULoadOp_Clear;
            ds.depthStoreOp = WGPUStoreOp_Store;
            ds.depthClearValue = desc.depth.clear_depth;
            has_depth = true;
        }
    }

    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = color_count;
    rp.colorAttachments = ca;
    rp.depthStencilAttachment = has_depth ? &ds : nullptr;
    plat.enc_ = wgpuCommandEncoderBeginRenderPass(plat.cmd_, &rp);
}
void CommandRecorder::DrawMeshes(Resources& res, Allocator& alloc, const MeshDrawList& list) {
    (void)res; (void)alloc; (void)list;
}
void CommandRecorder::DrawPoints(Resources& res, Allocator& alloc, const PointDraw& draw) {
    (void)res; (void)alloc; (void)draw;
}
void CommandRecorder::DrawFullscreen(Resources& res, Handle<Shader> pipeline,
                                     std::span<const Handle<Texture>> textures, Handle<Sampler> sampler) {
    if (!plat.enc_) { return; }
    Shader::Hot* sh = res.GetHot(pipeline);
    if (!sh || !sh->api_pso) { return; }  // un-ported pipeline (null pso): skip
    wgpuRenderPassEncoderSetPipeline(plat.enc_,
                                     static_cast<WGPURenderPipeline>(sh->api_pso));
    // N sampled textures at bindings 0..N-1 + the sampler at binding N, matching
    // the bind group layout CreateGraphicsPipeline built for this shader.
    if (!textures.empty() && sh->plat.bind_group_layouts[0]) {
        WGPUBindGroupEntry entries[8] = {};
        uint32_t n = 0;
        for (; n < textures.size() && n < 7; ++n) {
            Texture::Hot* th = res.GetHot(textures[n]);
            entries[n].binding = n;
            entries[n].textureView =
                th ? static_cast<WGPUTextureView>(th->api_view) : nullptr;
        }
        Sampler::Hot* smp = res.GetHot(sampler);
        entries[n].binding = n;
        entries[n].sampler = smp ? static_cast<WGPUSampler>(smp->api_sampler)
                                 : nullptr;
        WGPUBindGroupDescriptor bgd = {};
        bgd.layout = sh->plat.bind_group_layouts[0];
        bgd.entryCount = n + 1;
        bgd.entries = entries;
        WGPUBindGroup bg = wgpuDeviceCreateBindGroup(plat.device_, &bgd);
        wgpuRenderPassEncoderSetBindGroup(plat.enc_, 0, bg, 0, nullptr);
        plat.transient_bind_groups_.push_back(bg);
    }
    wgpuRenderPassEncoderDraw(plat.enc_, 3, 1, 0, 0);
}
void CommandRecorder::SetViewport(float x, float y, float w, float h) { (void)x; (void)y; (void)w; (void)h; }
void CommandRecorder::SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) { (void)x; (void)y; (void)w; (void)h; }
void CommandRecorder::DrawImGui(Resources& res, Allocator& alloc, Handle<Shader> pipeline,
                                Handle<Texture> font, Handle<Sampler> sampler, const ImDrawData* draw_data) {
    (void)res; (void)alloc; (void)pipeline; (void)font; (void)sampler; (void)draw_data;
}
void CommandRecorder::PassTimerBegin(const char* name, bool is_compute) { (void)name; (void)is_compute; }
void CommandRecorder::PassTimerEnd() {}
void CommandRecorder::EndRenderPass(Resources& res, std::span<const Handle<Texture>> flush) {
    (void)res; (void)flush;
    if (plat.enc_) { wgpuRenderPassEncoderEnd(plat.enc_); plat.enc_ = nullptr; }
}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
