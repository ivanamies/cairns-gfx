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
    WGPUTextureView view = nullptr;
    if (!desc.color.empty() && !desc.color[0].target.IsNull()) {
        Texture::Hot* hot = res.GetHot(desc.color[0].target);
        if (hot) { view = static_cast<WGPUTextureView>(hot->api_view); }
    }
    if (!view) { view = static_cast<WGPUTextureView>(target.plat.view); }
    if (!view) { return; }
    WGPURenderPassColorAttachment ca = {};
    ca.view = view;
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = (!desc.color.empty() && desc.color[0].load == LoadOp::kLoad)
                    ? WGPULoadOp_Load : WGPULoadOp_Clear;
    ca.storeOp = WGPUStoreOp_Store;
    if (!desc.color.empty()) {
        ca.clearValue = {desc.color[0].clear[0], desc.color[0].clear[1],
                         desc.color[0].clear[2], desc.color[0].clear[3]};
    }
    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &ca;
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
    (void)res; (void)pipeline; (void)textures; (void)sampler;
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
