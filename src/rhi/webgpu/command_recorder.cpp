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
    if (!plat.enc_) { return; }
    Shader::Hot* unlit = res.GetHot(list.pipeline);
    if (!unlit || !unlit->api_pso) { return; }  // un-ported (id MRT) variant
    wgpuRenderPassEncoderSetPipeline(plat.enc_,
                                     static_cast<WGPURenderPipeline>(unlit->api_pso));
    // Group 0 (globals): bind once for the pass, dynamic offset = globals_offset.
    if (!list.dyn_globals.IsNull()) {
        DynamicBuffers::Hot* dh = res.GetHot(list.dyn_globals);
        if (dh && dh->plat.sets[0]) {
            uint32_t off = list.globals_offset;
            wgpuRenderPassEncoderSetBindGroup(plat.enc_, 0, dh->plat.sets[0], 1, &off);
        }
    }
    // Pack-meshes (Aaltonen slide 26): only rebind a stream / material when it
    // changes; baseVertex + firstIndex select the primitive.
    uint32_t last_bg1 = 0xFFFFFFFFu;
    for (size_t i = 0; i < list.sorted_draws.size(); ++i) {
        const cairns::Draw& draw = list.draws[list.sorted_draws[i].second];
        // Group 1 (material): per-draw, no dynamic offset.
        if (!draw.bind_groups[1].IsNull() && draw.bind_groups[1].index != last_bg1) {
            last_bg1 = draw.bind_groups[1].index;
            BindGroup::Hot* mh = res.GetHot(draw.bind_groups[1]);
            if (mh && mh->api_descriptor_set) {
                wgpuRenderPassEncoderSetBindGroup(
                    plat.enc_, 1,
                    static_cast<WGPUBindGroup>(mh->api_descriptor_set), 0, nullptr);
            }
        }
        // Group 2 (drawtmp): per-draw dynamic offset.
        if (!draw.dynamic_buffers.IsNull()) {
            DynamicBuffers::Hot* dh = res.GetHot(draw.dynamic_buffers);
            if (dh && dh->plat.sets[0]) {
                uint32_t off = draw.dynamic_buffer_offsets[1];
                wgpuRenderPassEncoderSetBindGroup(plat.enc_, 2, dh->plat.sets[0], 1, &off);
            }
        }
        uint32_t pos_off = 0;
        WGPUBuffer pos = res.plat.GetWgpuBuffer(
            alloc, draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot], &pos_off);
        if (pos) {
            wgpuRenderPassEncoderSetVertexBuffer(plat.enc_, 0, pos, pos_off, WGPU_WHOLE_SIZE);
        }
        uint32_t attr_off = 0;
        WGPUBuffer attr = res.plat.GetWgpuBuffer(
            alloc, draw.vertex_buffers[cairns::Draw::kVertexBufferAttrSlot], &attr_off);
        if (attr) {
            wgpuRenderPassEncoderSetVertexBuffer(plat.enc_, 1, attr, attr_off, WGPU_WHOLE_SIZE);
        }
        uint32_t idx_base = 0;
        WGPUBuffer idx = res.plat.GetWgpuBuffer(alloc, draw.index_buffer, &idx_base);
        if (!idx) { continue; }
        wgpuRenderPassEncoderSetIndexBuffer(plat.enc_, idx, WGPUIndexFormat_Uint32,
                                            idx_base, WGPU_WHOLE_SIZE);
        const uint32_t first_index = (draw.index_offset - idx_base) / sizeof(uint32_t);
        wgpuRenderPassEncoderDrawIndexed(plat.enc_, draw.triangle_count * 3,
                                         draw.instance_count, first_index,
                                         static_cast<int32_t>(draw.vertex_offset),
                                         draw.instance_offset);
    }
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
void CommandRecorder::SetViewport(float x, float y, float w, float h) {
    if (!plat.enc_ || w <= 0.0f || h <= 0.0f) { return; }
    wgpuRenderPassEncoderSetViewport(plat.enc_, x, y, w, h, 0.0f, 1.0f);
}
void CommandRecorder::SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!plat.enc_ || w == 0 || h == 0) { return; }
    const uint32_t ux = x < 0 ? 0u : static_cast<uint32_t>(x);
    const uint32_t uy = y < 0 ? 0u : static_cast<uint32_t>(y);
    wgpuRenderPassEncoderSetScissorRect(plat.enc_, ux, uy, w, h);
}
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
