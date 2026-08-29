// rhi/webgpu/command_recorder.cpp -- WebGPU backend.
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/command_recorder.hpp"
#include "rhi/resources.hpp"
#include "rhi/allocator.hpp"

#include <cstdio>
#include <cstring>

#include "imgui.h"

#include <webgpu/webgpu.h>

namespace cairns::rhi {

void CommandRecorder::Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d) {
    (void)alloc;
    if (d.dyn_set_0.IsNull() || !plat.cmd_) { return; }
    Kernel::Hot* kh = res.GetHot(d.kernel);
    if (!kh || !kh->api_pso) { return; }
    DynamicBuffers::Hot* dh = res.dynamic_buffers.GetHot(d.dyn_set_0);
    if (!dh || !dh->plat.sets[0]) { return; }
    // Compute is its own pass; a render pass must not be open on this encoder
    // (webgpu forbids overlapping passes -- the graph closes it between nodes).
    if (plat.enc_) { wgpuRenderPassEncoderEnd(plat.enc_); plat.enc_ = nullptr; }

    // dt is the only dynamic-offset binding (count from the cold layout so
    // storage-only sets pass zero offsets).
    uint32_t dyn_count = 0;
    if (DynamicBuffers::Cold* cold = res.dynamic_buffers.GetCold(d.dyn_set_0)) {
        for (const DynamicBinding& b : cold->layout) {
            if (b.has_dynamic_offset) { ++dyn_count; }
        }
    }
    const uint32_t dyn = d.dyn_offset_0;
    WGPUComputePassEncoder cenc =
        wgpuCommandEncoderBeginComputePass(plat.cmd_, nullptr);
    wgpuComputePassEncoderSetPipeline(
        cenc, static_cast<WGPUComputePipeline>(kh->api_pso));
    wgpuComputePassEncoderSetBindGroup(cenc, 0, dh->plat.sets[0], dyn_count,
                                       dyn_count ? &dyn : nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cenc, d.groups_x, d.groups_y,
                                             d.groups_z);
    wgpuComputePassEncoderEnd(cenc);
    wgpuComputePassEncoderRelease(cenc);
}
void CommandRecorder::DispatchSkinBatches(Resources& res, Allocator& alloc, Handle<Kernel> kernel,
                                          Handle<Buffer> output_pool_buffer, Handle<Buffer> palette_buf,
                                          Handle<DynamicBuffers> dyn_set_0,
                                          std::span<const SkinDispatchBatch> batches) {
    (void)dyn_set_0;  // webgpu folds group A+B into the kernel's own set0 layout
    if (batches.empty() || kernel.IsNull() || !plat.cmd_) { return; }
    Kernel::Hot* kh = res.GetHot(kernel);
    if (!kh || !kh->api_pso || !kh->plat.set0_bgl) { return; }
    if (plat.enc_) { wgpuRenderPassEncoderEnd(plat.enc_); plat.enc_ = nullptr; }

    // Persistent dedicated buffers (offset_in_heap == 0 on webgpu) bind whole at
    // 0; per-mesh element bases ride in the WebBases UBO (binding 6). params /
    // inst_meta live in the kDynamic master at their 256-aligned byte offsets.
    WGPUBuffer dyn_master = res.plat.GetBumpMasterBuffer(alloc, Memory::kDynamic);
    uint32_t pool_heap = 0;
    WGPUBuffer pool = res.plat.GetWgpuBuffer(alloc, output_pool_buffer, &pool_heap);
    uint32_t pal_heap = 0;
    WGPUBuffer pal = palette_buf.IsNull()
                         ? dyn_master
                         : res.plat.GetWgpuBuffer(alloc, palette_buf, &pal_heap);
    if (!dyn_master || !pool || !pal) { return; }

    WGPUComputePassEncoder cenc =
        wgpuCommandEncoderBeginComputePass(plat.cmd_, nullptr);
    wgpuComputePassEncoderSetPipeline(
        cenc, static_cast<WGPUComputePipeline>(kh->api_pso));

    for (const SkinDispatchBatch& b : batches) {
        if (b.workgroups == 0 || b.pos_buffer.IsNull() ||
            b.skin_attr_buffer.IsNull()) {
            continue;
        }
        uint32_t pos_heap = 0;
        WGPUBuffer pos = res.plat.GetWgpuBuffer(alloc, b.pos_buffer, &pos_heap);
        uint32_t sa_heap = 0;
        WGPUBuffer sa = res.plat.GetWgpuBuffer(alloc, b.skin_attr_buffer, &sa_heap);
        if (!pos || !sa) { continue; }

        uint32_t wb[4];
        wb[0] = (pos_heap + b.pos_byte_offset) / 16u;       // vec4 stride
        wb[1] = (sa_heap + b.skin_attr_byte_offset) / 8u;   // vec2<u32> stride
        wb[2] = (pal_heap + b.palettes_byte_offset) / 64u;  // mat4 stride
        wb[3] = 0u;
        uint32_t wb_off = 0;
        void* wbptr = alloc.BumpAllocate(sizeof(wb), 256, Memory::kDynamic, &wb_off);
        if (!wbptr) { continue; }
        std::memcpy(wbptr, wb, sizeof(wb));

        WGPUBindGroupEntry e[7] = {};
        e[0].binding = 0; e[0].buffer = dyn_master;
        e[0].offset = b.params_byte_offset; e[0].size = 16u;
        e[1].binding = 1; e[1].buffer = pal;
        e[1].offset = 0; e[1].size = WGPU_WHOLE_SIZE;
        e[2].binding = 2; e[2].buffer = dyn_master;
        e[2].offset = b.instance_meta_byte_offset;
        e[2].size = b.instance_count * 8u;
        e[3].binding = 3; e[3].buffer = pool;
        e[3].offset = 0; e[3].size = WGPU_WHOLE_SIZE;
        e[4].binding = 4; e[4].buffer = pos;
        e[4].offset = 0; e[4].size = WGPU_WHOLE_SIZE;
        e[5].binding = 5; e[5].buffer = sa;
        e[5].offset = 0; e[5].size = WGPU_WHOLE_SIZE;
        e[6].binding = 6; e[6].buffer = dyn_master;
        e[6].offset = wb_off; e[6].size = 16u;

        WGPUBindGroupDescriptor bgd = {};
        bgd.layout = kh->plat.set0_bgl;
        bgd.entryCount = 7;
        bgd.entries = e;
        WGPUBindGroup bg = wgpuDeviceCreateBindGroup(plat.device_, &bgd);
        wgpuComputePassEncoderSetBindGroup(cenc, 0, bg, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(cenc, b.workgroups,
                                                 b.instance_count, 1);
        plat.transient_bind_groups_.push_back(bg);
    }
    wgpuComputePassEncoderEnd(cenc);
    wgpuComputePassEncoderRelease(cenc);
}
void CommandRecorder::DispatchAnimEval(Resources& res, Allocator& alloc, Handle<Kernel> kernel,
                                       const AnimEvalArgs& args) {
    (void)alloc;
    if (kernel.IsNull() || args.actor_count == 0 || args.dyn_set_0.IsNull() ||
        !plat.cmd_) {
        return;
    }
    Kernel::Hot* kh = res.GetHot(kernel);
    if (!kh || !kh->api_pso) { return; }
    DynamicBuffers::Hot* dh = res.dynamic_buffers.GetHot(args.dyn_set_0);
    if (!dh || !dh->plat.sets[0]) { return; }
    if (plat.enc_) { wgpuRenderPassEncoderEnd(plat.enc_); plat.enc_ = nullptr; }

    // The 13-binding set is prebuilt (CreateDynamicBuffers); binding 0 (records
    // UBO) is the sole dynamic offset. One workgroup per actor, 64 threads.
    const uint32_t dyn = args.records_byte_offset;
    WGPUComputePassEncoder cenc =
        wgpuCommandEncoderBeginComputePass(plat.cmd_, nullptr);
    wgpuComputePassEncoderSetPipeline(
        cenc, static_cast<WGPUComputePipeline>(kh->api_pso));
    wgpuComputePassEncoderSetBindGroup(cenc, 0, dh->plat.sets[0], 1, &dyn);
    wgpuComputePassEncoderDispatchWorkgroups(cenc, args.actor_count, 1, 1);
    wgpuComputePassEncoderEnd(cenc);
    wgpuComputePassEncoderRelease(cenc);
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
    // Per-draw pipeline rebind (lit/tam material variants stamped into
    // draw.shader by EncodeDraws) -- mirrors the metal/vk recorders. Init to
    // the pass pipeline so an unchanged draw.shader (or Null) doesn't rebind;
    // a variant with a null PSO (un-ported) is skipped, keeping the current.
    uint32_t last_shader_idx = list.pipeline.index;
    for (size_t i = 0; i < list.sorted_draws.size(); ++i) {
        const cairns::Draw& draw = list.draws[list.sorted_draws[i].second];
        if (!draw.shader.IsNull() && draw.shader.index != last_shader_idx) {
            Shader::Hot* sh = res.GetHot(draw.shader);
            if (sh && sh->api_pso) {
                wgpuRenderPassEncoderSetPipeline(
                    plat.enc_, static_cast<WGPURenderPipeline>(sh->api_pso));
                last_shader_idx = draw.shader.index;
            }
        }
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
        // Group 3 (shader-specific: the shadow map). Only stamped on draws
        // whose pipeline declares the group (webgpu validates the pair).
        if (!draw.bind_groups[2].IsNull()) {
            BindGroup::Hot* sh3 = res.GetHot(draw.bind_groups[2]);
            if (sh3 && sh3->api_descriptor_set) {
                wgpuRenderPassEncoderSetBindGroup(
                    plat.enc_, 3,
                    static_cast<WGPUBindGroup>(sh3->api_descriptor_set), 0,
                    nullptr);
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
void CommandRecorder::DrawFullscreenParams(
    Resources& res, Allocator& alloc, Handle<Shader> pipeline,
    std::span<const Handle<Texture>> textures, Handle<Sampler> sampler,
    Handle<DynamicBuffers> params_set, uint32_t params_offset) {
    // params_set carries no webgpu state: the layout already declared the
    // dynamic-offset uniform at binding N+1, and the buffer is the kDynamic
    // master directly.
    (void)params_set;
    if (!plat.enc_) { return; }
    Shader::Hot* sh = res.GetHot(pipeline);
    if (!sh || !sh->api_pso) { return; }
    WGPUBuffer master = res.plat.GetBumpMasterBuffer(alloc, Memory::kDynamic);
    if (!master || !sh->plat.bind_group_layouts[0]) { return; }
    wgpuRenderPassEncoderSetPipeline(plat.enc_,
                                     static_cast<WGPURenderPipeline>(sh->api_pso));
    // Textures at 0..N-1, sampler at N, params dyn-UBO at N+1.
    WGPUBindGroupEntry entries[8] = {};
    uint32_t n = 0;
    for (; n < textures.size() && n < 6; ++n) {
        Texture::Hot* th = res.GetHot(textures[n]);
        entries[n].binding = n;
        entries[n].textureView =
            th ? static_cast<WGPUTextureView>(th->api_view) : nullptr;
    }
    Sampler::Hot* smp = res.GetHot(sampler);
    entries[n].binding = n;
    entries[n].sampler = smp ? static_cast<WGPUSampler>(smp->api_sampler)
                             : nullptr;
    entries[n + 1].binding = n + 1;
    entries[n + 1].buffer = master;
    entries[n + 1].size = 256;
    WGPUBindGroupDescriptor bgd = {};
    bgd.layout = sh->plat.bind_group_layouts[0];
    bgd.entryCount = n + 2;
    bgd.entries = entries;
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(plat.device_, &bgd);
    wgpuRenderPassEncoderSetBindGroup(plat.enc_, 0, bg, 1, &params_offset);
    plat.transient_bind_groups_.push_back(bg);
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
                                Handle<Texture> font, Handle<Sampler> sampler,
                                const ImDrawData* dd) {
    if (!plat.enc_ || !dd || dd->CmdListsCount == 0 || dd->DisplaySize.x <= 0.0f) {
        return;
    }
    Shader::Hot* sh = res.GetHot(pipeline);
    if (!sh || !sh->api_pso) { return; }
    wgpuRenderPassEncoderSetPipeline(plat.enc_,
                                     static_cast<WGPURenderPipeline>(sh->api_pso));

    const float fsx = dd->FramebufferScale.x;
    const float fsy = dd->FramebufferScale.y;
    const float disp_w = dd->DisplaySize.x;
    const float disp_h = dd->DisplaySize.y;
    const float fb_w = disp_w * fsx;
    const float fb_h = disp_h * fsy;

    // imgui.metal push constant: ortho {scale, translate} -> dynamic UBO @group0.
    float pc[4];
    pc[0] = 2.0f / disp_w;
    pc[1] = -2.0f / disp_h;
    pc[2] = -1.0f - dd->DisplayPos.x * pc[0];
    pc[3] = 1.0f - dd->DisplayPos.y * pc[1];
    uint32_t pc_off = 0;
    void* pcptr = alloc.BumpAllocate(sizeof(pc), 256, Memory::kDynamic, &pc_off);
    if (!pcptr) { return; }
    std::memcpy(pcptr, pc, sizeof(pc));

    WGPUBuffer master = res.plat.GetBumpMasterBuffer(alloc, Memory::kDynamic);
    if (!master) { return; }

    WGPUBindGroupEntry ge0 = {};
    ge0.binding = 0;
    ge0.buffer = master;
    ge0.size = sizeof(pc);
    WGPUBindGroupDescriptor bgd0 = {};
    bgd0.layout = sh->plat.bind_group_layouts[0];
    bgd0.entryCount = 1;
    bgd0.entries = &ge0;
    WGPUBindGroup bg0 = wgpuDeviceCreateBindGroup(plat.device_, &bgd0);
    wgpuRenderPassEncoderSetBindGroup(plat.enc_, 0, bg0, 1, &pc_off);
    plat.transient_bind_groups_.push_back(bg0);

    Texture::Hot* fh = res.GetHot(font);
    Sampler::Hot* smp = res.GetHot(sampler);
    WGPUBindGroupEntry ge1[2] = {};
    ge1[0].binding = 0;
    ge1[0].textureView = fh ? static_cast<WGPUTextureView>(fh->api_view) : nullptr;
    ge1[1].binding = 1;
    ge1[1].sampler = smp ? static_cast<WGPUSampler>(smp->api_sampler) : nullptr;
    WGPUBindGroupDescriptor bgd1 = {};
    bgd1.layout = sh->plat.bind_group_layouts[1];
    bgd1.entryCount = 2;
    bgd1.entries = ge1;
    WGPUBindGroup bg1 = wgpuDeviceCreateBindGroup(plat.device_, &bgd1);
    wgpuRenderPassEncoderSetBindGroup(plat.enc_, 1, bg1, 0, nullptr);
    plat.transient_bind_groups_.push_back(bg1);

    wgpuRenderPassEncoderSetViewport(plat.enc_, 0.0f, 0.0f, fb_w, fb_h, 0.0f, 1.0f);

    const ImVec2 clip_off = dd->DisplayPos;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        const uint32_t vbytes =
            static_cast<uint32_t>(cl->VtxBuffer.Size) * sizeof(ImDrawVert);
        const uint32_t ibytes =
            static_cast<uint32_t>(cl->IdxBuffer.Size) * sizeof(ImDrawIdx);
        if (vbytes == 0 || ibytes == 0) { continue; }
        uint32_t voff = 0;
        uint32_t ioff = 0;
        void* vptr = alloc.BumpAllocate(vbytes, 16, Memory::kDynamic, &voff);
        void* iptr = alloc.BumpAllocate(ibytes, 4, Memory::kDynamic, &ioff);
        if (!vptr || !iptr) { continue; }
        std::memcpy(vptr, cl->VtxBuffer.Data, vbytes);
        std::memcpy(iptr, cl->IdxBuffer.Data, ibytes);
        wgpuRenderPassEncoderSetVertexBuffer(plat.enc_, 0, master, voff, vbytes);
        wgpuRenderPassEncoderSetIndexBuffer(
            plat.enc_, master,
            sizeof(ImDrawIdx) == 2 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32,
            ioff, ibytes);
        for (int c = 0; c < cl->CmdBuffer.Size; ++c) {
            const ImDrawCmd* cmd = &cl->CmdBuffer[c];
            float cx = (cmd->ClipRect.x - clip_off.x) * fsx;
            float cy = (cmd->ClipRect.y - clip_off.y) * fsy;
            float cz = (cmd->ClipRect.z - clip_off.x) * fsx;
            float cw = (cmd->ClipRect.w - clip_off.y) * fsy;
            cx = cx < 0.0f ? 0.0f : cx;
            cy = cy < 0.0f ? 0.0f : cy;
            cz = cz > fb_w ? fb_w : cz;
            cw = cw > fb_h ? fb_h : cw;
            if (cz <= cx || cw <= cy) { continue; }
            wgpuRenderPassEncoderSetScissorRect(
                plat.enc_, static_cast<uint32_t>(cx), static_cast<uint32_t>(cy),
                static_cast<uint32_t>(cz - cx), static_cast<uint32_t>(cw - cy));
            wgpuRenderPassEncoderDrawIndexed(plat.enc_, cmd->ElemCount, 1,
                                             cmd->IdxOffset,
                                             static_cast<int32_t>(cmd->VtxOffset), 0);
        }
    }
}
void CommandRecorder::PassTimerBegin(const char* name, bool is_compute) { (void)name; (void)is_compute; }
void CommandRecorder::PassTimerEnd() {}
void CommandRecorder::EndRenderPass(Resources& res, std::span<const Handle<Texture>> flush) {
    (void)res; (void)flush;
    if (plat.enc_) { wgpuRenderPassEncoderEnd(plat.enc_); plat.enc_ = nullptr; }
}
// WebGPU synchronizes implicitly -- the graph's barriers are no-ops here.
void CommandRecorder::BeginComputePass(Resources& res,
                                       std::span<const ResourceBarrier> invalidate,
                                       std::span<const Handle<Buffer>> flush_buffers) {
    (void)res; (void)invalidate; (void)flush_buffers;
}
void CommandRecorder::EndComputePass(Resources& res) { (void)res; }

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
