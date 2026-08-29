// rhi/metal/command_recorder.cpp
//
// Metal backend bodies for CommandRecorder. Moved out of resource_manager.cpp
// EndFrame in resource_manager.cpp can construct/destroy it.

#include "util/define.hpp"

#if CAIRNS_METAL

#include <cstring>
#include <limits>

#include <Metal/Metal.hpp>

#include "imgui.h"
#include "rhi/allocator.hpp"
#include "rhi/command_recorder.hpp"
#include "rhi/frames.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/resources.hpp"
#include "rhi/allocator.hpp"
#include "rhi/swap_chain.hpp"
#include "gpu_scene_registry.hpp"
#include "util/draw.hpp"
#include "util/timer.hpp"

#include "imgui.h"

namespace cairns::rhi {

void CommandRecorder::Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d) {
    if (cmd_ == nullptr) {
        cmd_ = queue_->commandBuffer();
    }
    MTL::ComputeCommandEncoder* cenc = cmd_->computeCommandEncoder();
    cenc->setComputePipelineState(res.GetHot(d.kernel)->api_pso);
    for (size_t i = 0; i < d.buffers.size(); ++i) {
        const BoundBuffer& b = d.buffers[i];
        uint32_t off = 0;
        MTL::Buffer* buf = res.GetMtlBuffer(alloc,b.buffer, &off);
        cenc->setBuffer(buf, off + b.offset, b.slot);
    }
    cenc->dispatchThreadgroups(MTL::Size{d.groups_x, d.groups_y, d.groups_z},
                               MTL::Size{d.local_x, d.local_y, d.local_z});
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

void CommandRecorder::BeginRenderPass(Resources& res, SwapChain&,
                                      const RenderPassDesc& desc) {
    if (cmd_ == nullptr) {
        cmd_ = queue_->commandBuffer();
    }
    const bool is_swapchain = desc.color.empty() ||
                              desc.color[0].target.IsNull();
    if (is_swapchain) {
        if (!desc.color.empty()) {
            const float* c = desc.color[0].clear;
            render_pass_desc_->colorAttachments()->object(0)->setClearColor(
                MTL::ClearColor(c[0], c[1], c[2], c[3]));
            render_pass_desc_->colorAttachments()->object(0)->setLoadAction(
                to_mtl_load(desc.color[0].load));
        }
        enc_ = cmd_->renderCommandEncoder(render_pass_desc_);
        return;
    }

    MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::alloc()->init();
    if (!desc.color.empty()) {
        const float* c = desc.color[0].clear;
        MTL::Texture* tex = res.GetHot(desc.color[0].target)->api_view;
        MTL::RenderPassColorAttachmentDescriptor* ca = rpd->colorAttachments()->object(0);
        ca->setTexture(tex);
        ca->setLoadAction(to_mtl_load(desc.color[0].load));
        ca->setStoreAction(MTL::StoreActionStore);
        ca->setClearColor(MTL::ClearColor(c[0], c[1], c[2], c[3]));
    }
    if (!desc.depth.depth.IsNull()) {
        MTL::Texture* dtex = res.GetHot(desc.depth.depth)->api_view;
        MTL::RenderPassDepthAttachmentDescriptor* da = rpd->depthAttachment();
        da->setTexture(dtex);
        da->setLoadAction(to_mtl_load(desc.depth.load));
        da->setStoreAction(MTL::StoreActionStore);
        da->setClearDepth(desc.depth.clear_depth);
    }
    enc_ = cmd_->renderCommandEncoder(rpd);
    rpd->release();
}

void CommandRecorder::DrawMeshes(Resources& res, Allocator& alloc, const MeshDrawList& list) {
    MTL::RenderCommandEncoder* enc = enc_;
    enc->setRenderPipelineState(res.GetHot(list.pipeline)->api_pso);
    enc->setDepthStencilState(depth_stencil_);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
    enc->setCullMode(MTL::CullModeBack);
    for (size_t i = 0; i < list.resident_textures.size(); ++i) {
        MTL::Texture* tex = res.GetHot(list.resident_textures[i])->api_view;
        if (tex) {
            enc->useResource(tex, MTL::ResourceUsageRead, MTL::RenderStageFragment);
        }
    }
    MTL::Buffer* dyn_master = res.GetBumpMasterBuffer(alloc, Memory::kDynamic);
    enc->setVertexBuffer(dyn_master, list.globals_offset, cairns::kRenderPassGlobalBindSlot);
    enc->setVertexBuffer(dyn_master, 0, cairns::kMaterialBindSlot);
    enc->setVertexBuffer(dyn_master, 0, cairns::kDrawTmpBindSlot);

    // Pack-meshes (slide 26): bind streams only when the mesh buffer changes;
    // drawIndexedPrimitives already selects the primitive via baseVertex.
    uint32_t last_mat_off = std::numeric_limits<uint32_t>::max();
    uint32_t last_mat_bg = std::numeric_limits<uint32_t>::max();
    MTL::Buffer* last_pos_buf = nullptr;
    uint32_t last_pos_off = std::numeric_limits<uint32_t>::max();
    MTL::Buffer* last_attr_buf = nullptr;
    uint32_t last_attr_off = std::numeric_limits<uint32_t>::max();
    for (size_t i = 0; i < list.sorted_draws.size(); ++i) {
        const cairns::Draw& draw = list.draws[list.sorted_draws[i].second];
        // set 2: per-material argument buffer, bound (fragment) on material change.
        const uint32_t mat_bg = draw.bind_groups[1].index;
        if (mat_bg != last_mat_bg) {
            last_mat_bg = mat_bg;
            BindGroup::Hot* mh = res.GetHot(draw.bind_groups[1]);
            enc->setFragmentBuffer(mh->api_descriptor_set, mh->arg_buf_offset,
                                   cairns::kMaterialBindSlot);
        }
        {
            uint32_t pos_off = 0;
            MTL::Buffer* pos_buf = res.GetMtlBuffer(
                alloc, draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot], &pos_off);
            if (pos_buf != last_pos_buf || pos_off != last_pos_off) {
                last_pos_buf = pos_buf;
                last_pos_off = pos_off;
                enc->useResource(pos_buf, MTL::ResourceUsageRead, MTL::RenderStageVertex);
                enc->setVertexBuffer(pos_buf, pos_off, 0);
            }
        }
        {
            uint32_t attr_off = 0;
            MTL::Buffer* attr_buf = res.GetMtlBuffer(
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
            MTL::Buffer* index_buffer = res.GetMtlBuffer(alloc,draw.index_buffer, &index_master_off);
            enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, draw.triangle_count * 3,
                                       MTL::IndexTypeUInt32, index_buffer, draw.index_offset, 1,
                                       draw.vertex_offset, 0);
        }
    }
}

void CommandRecorder::DrawPoints(Resources& res, Allocator& alloc, const PointDraw& pd) {
    enc_->setRenderPipelineState(res.GetHot(pd.pipeline)->api_pso);
    uint32_t off = 0;
    MTL::Buffer* buf = res.GetMtlBuffer(alloc,pd.vertex_buffer, &off);
    enc_->setVertexBuffer(buf, off, 0);
    enc_->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(pd.vertex_offset),
                               NS::UInteger(pd.vertex_count));
}

void CommandRecorder::DrawImGui(Resources& res, Allocator& alloc, Handle<Shader> pipeline,
                                Handle<Texture> font, Handle<Sampler> sampler,
                                const ImDrawData* dd) {
    if (!dd || dd->CmdListsCount == 0 || dd->DisplaySize.x <= 0.0f) {
        return;
    }
    MTL::RenderCommandEncoder* enc = enc_;
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

    MTL::Buffer* master = res.GetBumpMasterBuffer(alloc, Memory::kDynamic);
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
    MTL::RenderCommandEncoder* enc = enc_;
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
    enc_->setViewport(vp);
}

void CommandRecorder::SetScissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    MTL::ScissorRect s{};
    s.x = NS::UInteger(x < 0 ? 0 : x);
    s.y = NS::UInteger(y < 0 ? 0 : y);
    s.width = NS::UInteger(w);
    s.height = NS::UInteger(h);
    enc_->setScissorRect(s);
}

void CommandRecorder::EndRenderPass() {
    enc_->endEncoding();
}

void CommandRecorder::PassTimerBegin(const char* name) {
    pending_name_ = name;
    pending_slot_ = TimerStorage::SlotForPass(name);
}

void CommandRecorder::PassTimerEnd() {
    if (cmd_ == nullptr) {
        pending_name_ = nullptr;
        pending_slot_ = -1;
        return;
    }
    MTL::CommandBuffer* cb = cmd_;
    const char* name = pending_name_;
    const int slot = pending_slot_;
    cb->addCompletedHandler([slot, name](MTL::CommandBuffer* b) {
        const uint64_t us =
            static_cast<uint64_t>((b->GPUEndTime() - b->GPUStartTime()) * 1e6);
        TimerStorage::Span(slot, name, us);
    });
    cb->commit();
    cmd_ = nullptr;
    pending_name_ = nullptr;
    pending_slot_ = -1;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
