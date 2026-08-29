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
#include "rhi/swap_chain.hpp"
#include "gpu_scene_registry.hpp"
#include "util/draw.hpp"

namespace cairns::rhi {

void CommandRecorder::Dispatch(Resources& res, Allocator& alloc, const ComputeDispatch& d) {
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

namespace {
MTL::LoadAction to_mtl_load(LoadOp op) {
    switch (op) {
        case LoadOp::kClear: return MTL::LoadActionClear;
        case LoadOp::kLoad: return MTL::LoadActionLoad;
        default: return MTL::LoadActionDontCare;
    }
}
}  // namespace

void CommandRecorder::BeginRenderPass(Resources& res, SwapChain&,
                                      const RenderPassDesc& desc) {
    // Swapchain pass: the imported target is the null handle -> reuse the
    // prebuilt MSAA/depth descriptor (only the clear color varies).
    const bool is_swapchain = !desc.color.empty() && desc.color[0].target.IsNull();
    if (is_swapchain) {
        const float* c = desc.color[0].clear;
        render_pass_desc_->colorAttachments()->object(0)->setClearColor(
            MTL::ClearColor(c[0], c[1], c[2], c[3]));
        enc_ = cmd_->renderCommandEncoder(render_pass_desc_);
        return;
    }

    // Offscreen pass: build a fresh (autoreleased) descriptor from the baked
    // single-sample attachments.
    MTL::RenderPassDescriptor* rpd = MTL::RenderPassDescriptor::renderPassDescriptor();
    if (!desc.color.empty()) {
        MTL::RenderPassColorAttachmentDescriptor* ca =
            rpd->colorAttachments()->object(0);
        ca->setTexture(res.GetHot(desc.color[0].target)->api_view);
        ca->setLoadAction(to_mtl_load(desc.color[0].load));
        const float* c = desc.color[0].clear;
        ca->setClearColor(MTL::ClearColor(c[0], c[1], c[2], c[3]));
        ca->setStoreAction(MTL::StoreActionStore);
    }
    if (!desc.depth.depth.IsNull()) {
        MTL::RenderPassDepthAttachmentDescriptor* da = rpd->depthAttachment();
        da->setTexture(res.GetHot(desc.depth.depth)->api_view);
        da->setLoadAction(to_mtl_load(desc.depth.load));
        da->setClearDepth(desc.depth.clear_depth);
        da->setStoreAction(MTL::StoreActionStore);
    }
    enc_ = cmd_->renderCommandEncoder(rpd);
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
    MTL::ScissorRect r{};
    r.x = static_cast<NS::UInteger>(x < 0 ? 0 : x);
    r.y = static_cast<NS::UInteger>(y < 0 ? 0 : y);
    r.width = w;
    r.height = h;
    enc_->setScissorRect(r);
}

void CommandRecorder::EndRenderPass() {
    enc_->endEncoding();
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
