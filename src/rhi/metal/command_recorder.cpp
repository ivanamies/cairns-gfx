// rhi/metal/command_recorder.cpp
//
// Metal backend bodies for CommandRecorder. Moved out of resource_manager.cpp
// EndFrame in resource_manager.cpp can construct/destroy it.

#include "util/define.hpp"

#if CAIRNS_METAL

#include <limits>

#include <Metal/Metal.hpp>

#include "rhi/command_recorder.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/resources.hpp"
#include "rhi/allocator.hpp"
#include "rhi/swap_chain.hpp"
#include "rhi/swap_resolve_target.hpp"
#include "gpu_scene_registry.hpp"
#include "util/draw.hpp"
#include "util/timer.hpp"

#include "imgui.h"

namespace cairns::rhi {

void CommandRecorder::Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d) {
    if (d.dyn_set_0.IsNull()) {
        return;
    }
    DynamicBuffers::Cold* cold = res.dynamic_buffers.GetCold(d.dyn_set_0);
    if (!cold) {
        return;
    }
    if (plat.cmd_ == nullptr) {
        plat.cmd_ = plat.queue_->commandBuffer();
    }
    MTL::ComputeCommandEncoder* cenc = plat.cmd_->computeCommandEncoder();
    cenc->setComputePipelineState(res.GetHot(d.kernel)->api_pso);
    // #222 Phase D.4: walk DynamicBuffers Cold layout.
    // has_dynamic_offset=true -> kDynamic master at d.dyn_offset_0
    // (only one dyn offset supported; particle uses binding 0 = dt).
    // has_dynamic_offset=false -> binding's backing buffer at base_off.
    MTL::Buffer* dyn_master =
        res.plat.GetBumpMasterBuffer(alloc, Memory::kDynamic);
    for (const DynamicBinding& b : cold->layout) {
        if (b.has_dynamic_offset) {
            cenc->setBuffer(dyn_master, d.dyn_offset_0, b.slot);
        } else {
            uint32_t off = 0;
            MTL::Buffer* buf =
                res.plat.GetMtlBuffer(alloc, b.backing, &off);
            cenc->setBuffer(buf, off, b.slot);
        }
    }
    cenc->dispatchThreadgroups(MTL::Size{d.groups_x, d.groups_y, d.groups_z},
                               MTL::Size{d.local_x, d.local_y, d.local_z});
    cenc->endEncoding();
}

// #221 Skinning Phase 7: Metal mirror of DispatchSkinBatches. ONE
// MTLComputeCommandEncoder for the whole batch (F4: per-call encoder is
// fatal at 500 dispatches). Per-batch setBuffer:offset:atIndex: + dispatch.
// Encoder boundary = barrier; the next render encoder sees this batch's
// writes via Metal's implicit hazard tracking.
//
// Stub today: until BuildSkinFrame populates SkinDispatchBatch's
// kDynamic byte offsets (and the kernel sees real params/palettes), this
// records the encoder if batches is non-empty. Empty path returns
// immediately and is a no-op.
void CommandRecorder::DispatchSkinBatches(
    Resources& res, Allocator& alloc, Handle<Kernel> kernel,
    Handle<Buffer> output_pool_buffer, Handle<Buffer> palette_buf,
    Handle<DynamicBuffers> /*dyn_set_0*/,
    std::span<const SkinDispatchBatch> batches) {
    // #222 Phase D.3: dyn_set_0 unused on metal (no descriptor object;
    // setBuffer:offset:atIndex: drives bindings directly). Kept in signature
    // for parity with vk and a clean engine call site.
    if (batches.empty() || kernel.IsNull()) {
        return;
    }
    if (plat.cmd_ == nullptr) {
        plat.cmd_ = plat.queue_->commandBuffer();
    }
    Kernel::Hot* khot = res.GetHot(kernel);
    if (!khot) {
        return;
    }
    MTL::ComputeCommandEncoder* cenc = plat.cmd_->computeCommandEncoder();
    cenc->setComputePipelineState(khot->api_pso);
    // anim_eval writes palette_out_buf_ in a prior encoder; that buffer
    // is HazardTrackingModeUntracked so the encoder boundary alone does
    // NOT synchronize the write. Wait on the fence anim_eval signaled.
    if (plat.compute_fence_ != nullptr) {
        cenc->waitForFence(plat.compute_fence_);
    }
    MTL::Buffer* dyn_master =
        res.plat.GetBumpMasterBuffer(alloc, Memory::kDynamic);
    uint32_t pool_master_off = 0;
    MTL::Buffer* pool_buf =
        res.plat.GetMtlBuffer(alloc, output_pool_buffer, &pool_master_off);
    // #222 Phase D.3: palette buffer is frame-wide. Resolve once outside
    // the loop instead of per-batch (per MISTAKES.md counter:1 fix).
    MTL::Buffer* pal_buf = nullptr;
    uint32_t pal_master_off = 0;
    if (!palette_buf.IsNull()) {
        pal_buf = res.plat.GetMtlBuffer(alloc, palette_buf, &pal_master_off);
    }
    constexpr size_t kCacheCap = 16;
    Handle<Buffer> cache_h[kCacheCap]{};
    MTL::Buffer* cache_buf[kCacheCap]{};
    uint32_t cache_off[kCacheCap]{};
    size_t cache_n = 0;
    auto resolve = [&](Handle<Buffer> h, MTL::Buffer** out_buf,
                       uint32_t* out_off) {
        for (size_t i = 0; i < cache_n; ++i) {
            if (cache_h[i].index == h.index &&
                cache_h[i].generation == h.generation) {
                *out_buf = cache_buf[i];
                *out_off = cache_off[i];
                return;
            }
        }
        *out_buf = res.plat.GetMtlBuffer(alloc, h, out_off);
        if (cache_n < kCacheCap) {
            cache_h[cache_n] = h;
            cache_buf[cache_n] = *out_buf;
            cache_off[cache_n] = *out_off;
            ++cache_n;
        }
    };
    for (const SkinDispatchBatch& b : batches) {
        if (b.workgroups == 0 || b.pos_buffer.IsNull() ||
            b.skin_attr_buffer.IsNull()) {
            continue;
        }
        cenc->setBuffer(dyn_master, b.params_byte_offset, 0);
        if (pal_buf == nullptr) {
            cenc->setBuffer(dyn_master, b.palettes_byte_offset, 1);
        } else {
            cenc->setBuffer(pal_buf, pal_master_off + b.palettes_byte_offset, 1);
        }
        cenc->setBuffer(dyn_master, b.instance_meta_byte_offset, 2);
        cenc->setBuffer(pool_buf, pool_master_off, 3);
        MTL::Buffer* pos_buf = nullptr;
        uint32_t pos_master_off = 0;
        resolve(b.pos_buffer, &pos_buf, &pos_master_off);
        cenc->setBuffer(pos_buf, pos_master_off + b.pos_byte_offset, 4);
        MTL::Buffer* sa_buf = nullptr;
        uint32_t sa_master_off = 0;
        resolve(b.skin_attr_buffer, &sa_buf, &sa_master_off);
        cenc->setBuffer(sa_buf, sa_master_off + b.skin_attr_byte_offset, 5);
        cenc->dispatchThreadgroups(MTL::Size{b.workgroups, b.instance_count, 1u},
                                    MTL::Size{64u, 1u, 1u});
    }
    // Skin compute writes skin_output_pool_buffer_; subsequent forward render
    // reads it via pos_stream alias as vertex stream. Heap is untracked, so
    // updateFence here + waitForFence in BeginRenderPass (beforeStages:Vertex)
    // is what makes vertex fetch see the kernel writes.
    if (plat.compute_fence_ == nullptr) {
        plat.compute_fence_ = plat.cmd_->device()->newFence();
    }
    cenc->updateFence(plat.compute_fence_);
    cenc->endEncoding();
}

// #221 Phase 5b: Metal mirror of DispatchAnimEval. One workgroup per actor,
// 64 threads. setBuffer all 13 buffers + setThreadgroupMemoryLength for the
// shared GpuTRS[256]. Encoder boundary acts as the compute->compute barrier;
// the subsequent DispatchSkinBatches sees this kernel's writes via Metal's
// implicit hazard tracking.
void CommandRecorder::DispatchAnimEval(
    Resources& res, Allocator& alloc, Handle<Kernel> kernel,
    const AnimEvalArgs& args) {
    const uint32_t actor_count = args.actor_count;
    const uint32_t records_byte_offset = args.records_byte_offset;
    if (kernel.IsNull() || actor_count == 0) {
        return;
    }
    if (plat.cmd_ == nullptr) {
        plat.cmd_ = plat.queue_->commandBuffer();
    }
    Kernel::Hot* khot = res.GetHot(kernel);
    if (!khot) {
        return;
    }
    if (plat.compute_fence_ == nullptr) {
        plat.compute_fence_ = plat.cmd_->device()->newFence();
    }
    MTL::ComputeCommandEncoder* cenc = plat.cmd_->computeCommandEncoder();
    cenc->setComputePipelineState(khot->api_pso);
    Handle<Buffer> hs[12] = {
        args.scene_headers, args.parent_buf, args.topo_buf, args.bind_pose_buf,
        args.channels_buf, args.samplers_buf, args.times_buf, args.values_buf,
        args.joint_nodes_buf, args.inverse_binds_buf, args.world_scratch,
        args.palette_out,
    };
    {
        MTL::Buffer* records_mtl =
            res.plat.GetBumpMasterBuffer(alloc, Memory::kDynamic);
        cenc->setBuffer(records_mtl, records_byte_offset, 0);
    }
    for (uint32_t i = 0; i < 12; ++i) {
        uint32_t off = 0;
        MTL::Buffer* b = res.plat.GetMtlBuffer(alloc, hs[i], &off);
        cenc->setBuffer(b, off, 1 + i);
    }
    // GpuTRS = 48 B per node; 256 max nodes per scene = 12 KB shared.
    cenc->setThreadgroupMemoryLength(256u * 48u, 0);
    cenc->dispatchThreadgroups(MTL::Size{actor_count, 1u, 1u},
                                MTL::Size{64u, 1u, 1u});
    cenc->updateFence(plat.compute_fence_);
    cenc->endEncoding();
}

static MTL::LoadAction to_mtl_load(LoadOp op) {
    switch (op) {
        case LoadOp::kClear: return MTL::LoadActionClear;
        case LoadOp::kLoad: return MTL::LoadActionLoad;
        case LoadOp::kDontCare: return MTL::LoadActionDontCare;
    }
    return MTL::LoadActionClear;
}

static MTL::StoreAction to_mtl_store(StoreOp op) {  // #222 Phase A.2
    switch (op) {
        case StoreOp::kStore: return MTL::StoreActionStore;
        case StoreOp::kDontCare: return MTL::StoreActionDontCare;
    }
    return MTL::StoreActionStore;
}

// Execute the graph's invalidate barriers on the just-created render encoder:
// wait each hazarding texture's sync_fence_ (signaled by its last writer at
// EndRenderPass). Metal has no layouts/access masks; the fence IS the barrier,
// so we ignore the abstract src/dst fields and just wait. FLAKY_TESTS #2.
static void apply_invalidate_fences(CommandRecorderPlat& plat, Resources& res,
                                    std::span<const ResourceBarrier> invalidate) {
    for (const ResourceBarrier& b : invalidate) {
        if (b.texture.IsNull()) {
            continue;
        }
        Texture::Cold* cold = res.textures.GetCold(b.texture);
        if (cold != nullptr && cold->plat.sync_fence_ != nullptr) {
            plat.enc_->waitForFence(cold->plat.sync_fence_,
                                    MTL::RenderStageVertex);
        }
    }
}

void CommandRecorder::BeginRenderPass(Resources& res, const SwapResolveTarget&,
                                      const RenderPassDesc& desc,
                                      std::span<const ResourceBarrier> invalidate) {
    if (plat.cmd_ == nullptr) {
        plat.cmd_ = plat.queue_->commandBuffer();
    }
    const bool is_swapchain = desc.color.empty() ||
                              desc.color[0].target.IsNull();
    if (is_swapchain) {
        if (!desc.color.empty()) {
            const float* c = desc.color[0].clear;
            plat.render_pass_desc_->colorAttachments()->object(0)->setClearColor(
                MTL::ClearColor(c[0], c[1], c[2], c[3]));
            plat.render_pass_desc_->colorAttachments()->object(0)->setLoadAction(
                to_mtl_load(desc.color[0].load));
        }
        plat.enc_ = plat.cmd_->renderCommandEncoder(plat.render_pass_desc_);
        if (plat.compute_fence_ != nullptr) {
            plat.enc_->waitForFence(plat.compute_fence_,
                                     MTL::RenderStageVertex);
        }
        apply_invalidate_fences(plat, res, invalidate);
        return;
    }

    MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::alloc()->init();
    // #206 MRT: bind every color attachment so the R32U id_target_ (slot 1
    // in the forward pass) actually receives unlit.frag's location-1
    // output. Previously only desc.color[0] was bound, which left the id
    // buffer un-touched -- pick readback got uninitialized memory.
    for (size_t i = 0; i < desc.color.size(); ++i) {
        const float* c = desc.color[i].clear;
        MTL::Texture* tex = res.GetHot(desc.color[i].target)->api_view;
        MTL::RenderPassColorAttachmentDescriptor* ca =
            rpd->colorAttachments()->object(static_cast<NS::UInteger>(i));
        ca->setTexture(tex);
        ca->setLoadAction(to_mtl_load(desc.color[i].load));
        ca->setStoreAction(to_mtl_store(desc.color[i].store));  // #222 Phase A.2
        ca->setClearColor(MTL::ClearColor(c[0], c[1], c[2], c[3]));
    }
    if (!desc.depth.depth.IsNull()) {
        MTL::Texture* dtex = res.GetHot(desc.depth.depth)->api_view;
        MTL::RenderPassDepthAttachmentDescriptor* da = rpd->depthAttachment();
        da->setTexture(dtex);
        da->setLoadAction(to_mtl_load(desc.depth.load));
        da->setStoreAction(to_mtl_store(desc.depth.store));  // #222 Phase A.2
        da->setClearDepth(desc.depth.clear_depth);
    }
    plat.enc_ = plat.cmd_->renderCommandEncoder(rpd);
    if (plat.compute_fence_ != nullptr) {
        plat.enc_->waitForFence(plat.compute_fence_,
                                 MTL::RenderStageVertex);
    }
    apply_invalidate_fences(plat, res, invalidate);
    rpd->release();
}

void CommandRecorder::DrawMeshes(Resources& res, Allocator& alloc, const MeshDrawList& list) {
    MTL::RenderCommandEncoder* enc = plat.enc_;
    Shader::Hot* shot = res.GetHot(list.pipeline);
    if (!shot) {
        return;
    }
    enc->setRenderPipelineState(shot->api_pso);
    enc->setDepthStencilState(plat.depth_stencil_);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
    enc->setCullMode(MTL::CullModeBack);
    for (size_t i = 0; i < list.resident_textures.size(); ++i) {
        MTL::Texture* tex = res.GetHot(list.resident_textures[i])->api_view;
        if (tex) {
            enc->useResource(tex, MTL::ResourceUsageRead, MTL::RenderStageFragment);
        }
    }
    MTL::Buffer* dyn_master = res.plat.GetBumpMasterBuffer(alloc, Memory::kDynamic);
    enc->setVertexBuffer(dyn_master, list.globals_offset, cairns::kRenderPassGlobalBindSlot);
    enc->setVertexBuffer(dyn_master, 0, cairns::kMaterialBindSlot);
    enc->setVertexBuffer(dyn_master, 0, cairns::kDrawTmpBindSlot);

    // Pack-meshes (slide 26): bind streams only when the mesh buffer changes;
    // drawIndexedPrimitives already selects the primitive via baseVertex.
    uint32_t last_mat_off = std::numeric_limits<uint32_t>::max();
    uint32_t last_bg[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    MTL::Buffer* last_pos_buf = nullptr;
    uint32_t last_pos_off = std::numeric_limits<uint32_t>::max();
    MTL::Buffer* last_attr_buf = nullptr;
    uint32_t last_attr_off = std::numeric_limits<uint32_t>::max();
    // #222 Phase E.1 (metal mirror): per-draw shader rebind on change.
    // Init last bound to list.pipeline so an unchanged draw.shader (or
    // the still-common Null sentinel) doesn't rebind.
    uint32_t last_shader_idx = list.pipeline.index;
    for (size_t i = 0; i < list.sorted_draws.size(); ++i) {
        const cairns::Draw& draw = list.draws[list.sorted_draws[i].second];
        if (!draw.shader.IsNull() && draw.shader.index != last_shader_idx) {
            Shader::Hot* sh = res.GetHot(draw.shader);
            if (sh) {
                enc->setRenderPipelineState(sh->api_pso);
                last_shader_idx = draw.shader.index;
            }
        }
        // #222 Phase E.0 (metal mirror): generic bind_groups[0..2] loop.
        // Today only slot 1 (material) is non-null; E.1/E.2/E.4 fill the
        // others. Bound as a vertex+fragment argument buffer; material
        // sets a fragment buffer (see kMaterialBindSlot).
        for (uint32_t s = 0; s < 3; ++s) {
            if (draw.bind_groups[s].IsNull()) {
                continue;
            }
            if (draw.bind_groups[s].index == last_bg[s]) {
                continue;
            }
            last_bg[s] = draw.bind_groups[s].index;
            BindGroup::Hot* mh = res.GetHot(draw.bind_groups[s]);
            if (!mh) {
                continue;
            }
            // Material today (slot 1) is fragment-side only on metal.
            // Other slots arrive with their own bind sites in E.1/E.2.
            if (s == 1) {
                enc->setFragmentBuffer(mh->api_descriptor_set,
                                        mh->arg_buf_offset,
                                        cairns::kMaterialBindSlot);
            }
        }
        {
            uint32_t pos_off = 0;
            MTL::Buffer* pos_buf = res.plat.GetMtlBuffer(
                alloc, draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot], &pos_off);
            // #222 Phase E.6 (Metal mirror): stream-0 alias resolves to the
            // final byte offset; no pos_buffer_byte_offset side channel.
            if (pos_buf != last_pos_buf || pos_off != last_pos_off) {
                last_pos_buf = pos_buf;
                last_pos_off = pos_off;
                enc->useResource(pos_buf, MTL::ResourceUsageRead, MTL::RenderStageVertex);
                enc->setVertexBuffer(pos_buf, pos_off, 0);
            }
        }
        {
            uint32_t attr_off = 0;
            MTL::Buffer* attr_buf = res.plat.GetMtlBuffer(
                alloc, draw.vertex_buffers[cairns::Draw::kVertexBufferAttrSlot], &attr_off);
            if (attr_buf != last_attr_buf || attr_off != last_attr_off) {
                last_attr_buf = attr_buf;
                last_attr_off = attr_off;
                enc->useResource(attr_buf, MTL::ResourceUsageRead, MTL::RenderStageVertex);
                enc->setVertexBuffer(attr_buf, attr_off, cairns::kMeshAttrVertexBindSlot);
            }
        }
        {
            const uint32_t mat_off = draw.dynamic_buffer_offsets[0];
            if (mat_off != last_mat_off) {
                last_mat_off = mat_off;
                enc->setVertexBufferOffset(mat_off, cairns::kMaterialBindSlot);
            }
        }
        enc->setVertexBufferOffset(draw.dynamic_buffer_offsets[1], cairns::kDrawTmpBindSlot);
        {
            uint32_t index_master_off = 0;
            MTL::Buffer* index_buffer = res.plat.GetMtlBuffer(alloc,draw.index_buffer, &index_master_off);
            enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, draw.triangle_count * 3,
                                       MTL::IndexTypeUInt32, index_buffer, draw.index_offset, 1,
                                       draw.vertex_offset, 0);
        }
    }
}

void CommandRecorder::DrawPoints(Resources& res, Allocator& alloc, const PointDraw& pd) {
    plat.enc_->setRenderPipelineState(res.GetHot(pd.pipeline)->api_pso);
    uint32_t off = 0;
    MTL::Buffer* buf = res.plat.GetMtlBuffer(alloc,pd.vertex_buffer, &off);
    plat.enc_->setVertexBuffer(buf, off, 0);
    plat.enc_->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(pd.vertex_offset),
                               NS::UInteger(pd.vertex_count));
}

void CommandRecorder::DrawImGui(Resources& res, Allocator& alloc, Handle<Shader> pipeline,
                                Handle<Texture> font, Handle<Sampler> sampler,
                                const ImDrawData* dd) {
    if (!dd || dd->CmdListsCount == 0 || dd->DisplaySize.x <= 0.0f) {
        return;
    }
    MTL::RenderCommandEncoder* enc = plat.enc_;
    enc->setRenderPipelineState(res.GetHot(pipeline)->api_pso);
    enc->setCullMode(MTL::CullModeNone);
    enc->setFragmentTexture(res.GetHot(font)->api_view, 0);
    enc->setFragmentSamplerState(res.GetHot(sampler)->api_sampler, 0);

    const float fsx = dd->FramebufferScale.x;
    const float fsy = dd->FramebufferScale.y;
    const float disp_w = dd->DisplaySize.x;
    const float disp_h = dd->DisplaySize.y;
    const float fb_w = disp_w * fsx;
    const float fb_h = disp_h * fsy;
    float pc[4];
    pc[0] = 2.0f / disp_w;
    pc[1] = -2.0f / disp_h;
    pc[2] = -1.0f - dd->DisplayPos.x * pc[0];
    pc[3] = 1.0f - dd->DisplayPos.y * pc[1];
    enc->setVertexBytes(pc, sizeof(pc), 1);

    MTL::Viewport vp{0.0, 0.0, static_cast<double>(fb_w), static_cast<double>(fb_h),
                     0.0, 1.0};
    enc->setViewport(vp);

    MTL::Buffer* master = res.plat.GetBumpMasterBuffer(alloc, Memory::kDynamic);
    const ImVec2 clip_off = dd->DisplayPos;
    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        const size_t vbytes = static_cast<size_t>(cl->VtxBuffer.Size) * sizeof(ImDrawVert);
        const size_t ibytes = static_cast<size_t>(cl->IdxBuffer.Size) * sizeof(ImDrawIdx);
        uint32_t voff = 0;
        uint32_t ioff = 0;
        void* vptr = alloc.BumpAllocate(static_cast<uint32_t>(vbytes), 16,
                                        Memory::kDynamic, &voff);
        void* iptr = alloc.BumpAllocate(static_cast<uint32_t>(ibytes), 4,
                                        Memory::kDynamic, &ioff);
        std::memcpy(vptr, cl->VtxBuffer.Data, vbytes);
        std::memcpy(iptr, cl->IdxBuffer.Data, ibytes);
        enc->setVertexBuffer(master, voff, 0);
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
            if (cz <= cx || cw <= cy) {
                continue;
            }
            MTL::ScissorRect scis{};
            scis.x = static_cast<NS::UInteger>(cx);
            scis.y = static_cast<NS::UInteger>(cy);
            scis.width = static_cast<NS::UInteger>(cz - cx);
            scis.height = static_cast<NS::UInteger>(cw - cy);
            enc->setScissorRect(scis);
            const NS::UInteger idx_off =
                ioff + static_cast<NS::UInteger>(cmd->IdxOffset) * sizeof(ImDrawIdx);
            enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle,
                                       static_cast<NS::UInteger>(cmd->ElemCount),
                                       MTL::IndexTypeUInt16, master, idx_off,
                                       NS::UInteger(1),
                                       static_cast<NS::Integer>(cmd->VtxOffset),
                                       NS::UInteger(0));
        }
    }
}

void CommandRecorder::DrawFullscreen(Resources& res, Handle<Shader> pipeline,
                                     std::span<const Handle<Texture>> textures,
                                     Handle<Sampler> sampler) {
    MTL::RenderCommandEncoder* enc = plat.enc_;
    enc->setRenderPipelineState(res.GetHot(pipeline)->api_pso);
    enc->setCullMode(MTL::CullModeNone);
    MTL::SamplerState* samp = res.GetHot(sampler)->api_sampler;
    for (uint32_t i = 0; i < textures.size(); ++i) {
        MTL::Texture* tex = res.GetHot(textures[i])->api_view;
        if (tex) {
            enc->useResource(tex, MTL::ResourceUsageRead, MTL::RenderStageFragment);
            enc->setFragmentTexture(tex, i);
        }
        enc->setFragmentSamplerState(samp, i);
    }
    enc->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(3));
}

void CommandRecorder::SetViewport(float x, float y, float w, float h) {
    MTL::Viewport vp{};
    vp.originX = x;
    vp.originY = y;
    vp.width = w;
    vp.height = h;
    vp.znear = 0.0;
    vp.zfar = 1.0;
    plat.enc_->setViewport(vp);
}

void CommandRecorder::SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    MTL::ScissorRect s{};
    s.x = NS::UInteger(x < 0 ? 0 : x);
    s.y = NS::UInteger(y < 0 ? 0 : y);
    s.width = NS::UInteger(w);
    s.height = NS::UInteger(h);
    plat.enc_->setScissorRect(s);
}

void CommandRecorder::EndRenderPass(Resources& res,
                                    std::span<const Handle<Texture>> flush) {
    // Signal each written texture's sync_fence_ after this pass's fragment
    // writes, so a later pass that hazards on it (this frame OR next) waits via
    // BeginRenderPass. Lazy-create the per-resource fence on first write; one
    // fence per texture, reused across frames -> cross-frame WAW. The metal leaf
    // of the graph's flush. FLAKY_TESTS #2 / Granite gap #1.
    for (Handle<Texture> h : flush) {
        if (h.IsNull()) {
            continue;
        }
        Texture::Cold* cold = res.textures.GetCold(h);
        if (cold == nullptr) {
            continue;
        }
        if (cold->plat.sync_fence_ == nullptr && plat.cmd_ != nullptr) {
            cold->plat.sync_fence_ = plat.cmd_->device()->newFence();
        }
        if (cold->plat.sync_fence_ != nullptr) {
            plat.enc_->updateFence(cold->plat.sync_fence_,
                                   MTL::RenderStageFragment);
        }
    }
    plat.enc_->endEncoding();
}

void CommandRecorder::PassTimerBegin(const char* name, bool /*is_compute*/) {
    pending_name_ = name;
    pending_slot_ = TimerStorage::SlotForPass(name);
}

void CommandRecorder::PassTimerEnd() {
    if (plat.cmd_ == nullptr) {
        pending_name_ = nullptr;
        pending_slot_ = -1;
        return;
    }
    MTL::CommandBuffer* cb = plat.cmd_;
    const char* name = pending_name_;
    const int slot = pending_slot_;
    cb->addCompletedHandler([slot, name](MTL::CommandBuffer* b) {
        const uint64_t us =
            static_cast<uint64_t>((b->GPUEndTime() - b->GPUStartTime()) * 1e6);
        TimerStorage::Span(slot, name, us);
    });
    cb->commit();
    plat.cmd_ = nullptr;
    pending_name_ = nullptr;
    pending_slot_ = -1;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
