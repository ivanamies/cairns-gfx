// rhi/metal/command_recorder.cpp
//
// Metal backend bodies for CommandRecorder. Moved out of resource_manager.cpp
// (Phase 0a). The Impl lives in metal/command_recorder_impl.hpp so BeginFrame /
// EndFrame in resource_manager.cpp can construct/destroy it.

#include "util/define.hpp"

#if CAIRNS_METAL

#include <limits>

#include <Metal/Metal.hpp>

#include "rhi/command_recorder.hpp"
#include "rhi/metal/command_recorder_impl.hpp"
#include "rhi/resource_manager.hpp"
#include "rhi/resources.hpp"
#include "rhi/swap_chain.hpp"
#include "gpu_scene_registry.hpp"
#include "util/draw.hpp"

namespace cairns::rhi {

void CommandRecorder::Dispatch(const ComputeDispatch& d) {
    MTL::ComputeCommandEncoder* cenc = impl_->cmd->computeCommandEncoder();
    cenc->setComputePipelineState(impl_->res->GetHot(d.kernel)->api_pso);
    for (size_t i = 0; i < d.buffers.size(); ++i) {
        const BoundBuffer& b = d.buffers[i];
        uint32_t off = 0;
        MTL::Buffer* buf = impl_->res->GetMtlBuffer(b.buffer, &off);
        cenc->setBuffer(buf, off + b.offset, b.slot);
    }
    cenc->dispatchThreadgroups(MTL::Size{d.groups_x, d.groups_y, d.groups_z},
                               MTL::Size{d.local_x, d.local_y, d.local_z});
    cenc->endEncoding();
}

void CommandRecorder::BeginRenderPass(const RenderPassDesc& desc) {
    if (!desc.color.empty()) {
        const float* c = desc.color[0].clear;
        impl_->render_pass_desc->colorAttachments()->object(0)->setClearColor(
            MTL::ClearColor(c[0], c[1], c[2], c[3]));
    }
    impl_->enc = impl_->cmd->renderCommandEncoder(impl_->render_pass_desc);
}

void CommandRecorder::DrawMeshes(const MeshDrawList& list) {
    MTL::RenderCommandEncoder* enc = impl_->enc;
    enc->setRenderPipelineState(impl_->res->GetHot(list.pipeline)->api_pso);
    enc->setDepthStencilState(impl_->depth_stencil);
    enc->setFrontFacingWinding(MTL::WindingCounterClockwise);
    enc->setCullMode(MTL::CullModeBack);
    {
        BindGroup::Hot* bg = impl_->res->GetHot(list.bindless);
        MTL::Buffer* bg_buf = bg->api_descriptor_set;
        const uint32_t bg_off = bg->arg_buf_offset;
        enc->setVertexBuffer(bg_buf, bg_off, GpuSceneRegistry::kBindSlot);
        enc->setFragmentBuffer(bg_buf, bg_off, GpuSceneRegistry::kBindSlot);
    }
    for (size_t i = 0; i < list.resident_textures.size(); ++i) {
        MTL::Texture* tex = impl_->res->GetHot(list.resident_textures[i])->api_view;
        if (tex) {
            enc->useResource(tex, MTL::ResourceUsageRead, MTL::RenderStageFragment);
        }
    }
    uint32_t mesh_master_off = 0;
    MTL::Buffer* mesh_master =
        impl_->res->GetMtlBuffer(list.resident_buffers[0], &mesh_master_off);
    enc->useResource(mesh_master, MTL::ResourceUsageRead, MTL::RenderStageVertex);
    enc->setVertexBuffer(mesh_master, 0, 0);
    MTL::Buffer* dyn_master = impl_->res->GetBumpMasterBuffer(Memory::kDynamic);
    enc->setVertexBuffer(dyn_master, list.globals_offset, cairns::kRenderPassGlobalBindSlot);
    enc->setVertexBuffer(dyn_master, 0, cairns::kMaterialBindSlot);
    enc->setVertexBuffer(dyn_master, 0, cairns::kDrawTmpBindSlot);

    uint32_t last_mat_off = std::numeric_limits<uint32_t>::max();
    for (size_t i = 0; i < list.sorted_indices.size(); ++i) {
        const cairns::Draw& draw = list.draws[list.sorted_indices[i]];
        {
            uint32_t pos_off = 0;
            impl_->res->GetMtlBuffer(draw.vertex_buffers[cairns::Draw::kVertexBufferPosSlot],
                                    &pos_off);
            enc->setVertexBufferOffset(pos_off, 0);
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
            MTL::Buffer* index_buffer = impl_->res->GetMtlBuffer(draw.index_buffer, &index_master_off);
            enc->drawIndexedPrimitives(MTL::PrimitiveTypeTriangle, draw.triangle_count * 3,
                                       MTL::IndexTypeUInt32, index_buffer, draw.index_offset, 1,
                                       draw.vertex_offset, 0);
        }
    }
}

void CommandRecorder::DrawPoints(const PointDraw& pd) {
    impl_->enc->setRenderPipelineState(impl_->res->GetHot(pd.pipeline)->api_pso);
    uint32_t off = 0;
    MTL::Buffer* buf = impl_->res->GetMtlBuffer(pd.vertex_buffer, &off);
    impl_->enc->setVertexBuffer(buf, off, 0);
    impl_->enc->drawPrimitives(MTL::PrimitiveTypePoint, NS::UInteger(pd.vertex_offset),
                               NS::UInteger(pd.vertex_count));
}

void CommandRecorder::EndRenderPass() {
    impl_->enc->endEncoding();
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
