// rhi/metal/frames.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include <dispatch/dispatch.h>
#include <vector>

#include <Metal/Metal.hpp>
#include <stb_image_write.h>

#include "rhi/frames.hpp"
#include "rhi/device.hpp"
#include "rhi/resources.hpp"
#include "rhi/resource_manager.hpp"  // kFramesInFlight
#include "rhi/swap_chain.hpp"
#include "rhi/command_recorder.hpp"

namespace cairns::rhi {

Frames::~Frames() { Deinit(); }

bool Frames::Init(Device& device, Resources& resources) {
    if (inited_) {
        return true;
    }
    device_ = device.device_;
    queue_ = device.queue_;
    res_ = &resources;

    frame_semaphore_ = dispatch_semaphore_create(kFramesInFlight);
    {
        MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
        dsd->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        dsd->setDepthWriteEnabled(true);
        depth_stencil_ = device_->newDepthStencilState(dsd);
        dsd->release();
    }
    inited_ = true;
    return true;
}

// Metal MSAA/depth targets share the texture Pool index space with scene
// textures; created after scene textures load (call post scene load).
bool Frames::InitTargets(SwapChain& sc) {
    constexpr uint32_t kSampleCount = 4;
    const int32_t w = static_cast<int32_t>(sc.Width());
    const int32_t h = static_cast<int32_t>(sc.Height());
    {
        TextureDesc d;
        d.dimensions = {w, h, 1};
        d.format = Format::kBgra8Unorm;
        d.sample_count = kSampleCount;
        d.usage = kTexUsageColorTarget;
        d.memory = Memory::kDefault;
        msaa_handle_ = res_->CreateTexture(d);
        if (msaa_handle_.IsNull()) {
            return false;
        }
    }
    {
        TextureDesc d;
        d.dimensions = {w, h, 1};
        d.format = Format::kD32F;
        d.sample_count = kSampleCount;
        d.usage = kTexUsageDepthTarget;
        d.memory = Memory::kDefault;
        depth_handle_ = res_->CreateTexture(d);
        if (depth_handle_.IsNull()) {
            return false;
        }
    }
    MTL::Texture* msaa = res_->GetHot(msaa_handle_)->api_view;
    MTL::Texture* depth = res_->GetHot(depth_handle_)->api_view;
    return InitRenderPassDescriptor(render_pass_desc_, msaa, depth, sc);
}

void Frames::Deinit() {
    if (!inited_) {
        return;
    }
    if (depth_stencil_) {
        depth_stencil_->release();
    }
    if (render_pass_desc_) {
        render_pass_desc_->release();
    }
    inited_ = false;
}

void Frames::SetDumpPath(const std::filesystem::path& path) {
    dump_path_ = path;
}

FrameContext Frames::Begin(SwapChain& sc) {
    dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(frame_semaphore_),
                            DISPATCH_TIME_FOREVER);
    res_->AdvanceFrame();  // bump ring reset

    sc.NextDrawable();
    MTL::Texture* msaa = res_->GetHot(msaa_handle_)->api_view;
    MTL::Texture* depth = res_->GetHot(depth_handle_)->api_view;
    UpdateRenderPassDescriptor(render_pass_desc_, msaa, depth, sc);

    MTL::CommandBuffer* cmd = queue_->commandBuffer();
    dispatch_semaphore_t sem = static_cast<dispatch_semaphore_t>(frame_semaphore_);
    cmd->addCompletedHandler([sem](MTL::CommandBuffer*) { dispatch_semaphore_signal(sem); });

    FrameContext fc;
    fc.frame_index = 0;
    fc.swapchain_image_index = 0;
    fc.cmd.res_ = res_;
    fc.cmd.sc_ = &sc;
    fc.cmd.cmd_ = cmd;
    fc.cmd.enc_ = nullptr;
    fc.cmd.render_pass_desc_ = render_pass_desc_;
    fc.cmd.depth_stencil_ = depth_stencil_;
    return fc;
}

void Frames::End(FrameContext& fc) {
    CommandRecorder& ri = fc.cmd;
    MTL::CommandBuffer* cmd = ri.cmd_;

    if (!dump_path_.empty()) {
        MTL::Texture* drawableTex = ri.sc_->GetDrawable()->texture();
        const NS::UInteger w = drawableTex->width();
        const NS::UInteger h = drawableTex->height();
        const NS::UInteger bytesPerRow = w * 4;
        const NS::UInteger bufSize = bytesPerRow * h;
        MTL::Buffer* readback = device_->newBuffer(bufSize, MTL::ResourceStorageModeShared);
        MTL::BlitCommandEncoder* blitEnc = cmd->blitCommandEncoder();
        blitEnc->copyFromTexture(drawableTex, 0, 0, MTL::Origin{0, 0, 0}, MTL::Size{w, h, 1},
                                 readback, 0, bytesPerRow, 0);
        blitEnc->endEncoding();
        cmd->presentDrawable(ri.sc_->GetDrawable());
        cmd->commit();
        cmd->waitUntilCompleted();
        std::vector<uint8_t> rgba(bufSize);
        const uint8_t* bgra = static_cast<const uint8_t*>(readback->contents());
        for (NS::UInteger i = 0; i < w * h; ++i) {
            rgba[i * 4 + 0] = bgra[i * 4 + 2];
            rgba[i * 4 + 1] = bgra[i * 4 + 1];
            rgba[i * 4 + 2] = bgra[i * 4 + 0];
            rgba[i * 4 + 3] = bgra[i * 4 + 3];
        }
        stbi_write_png(dump_path_.string().c_str(), static_cast<int>(w),
                       static_cast<int>(h), 4, rgba.data(), static_cast<int>(bytesPerRow));
        readback->release();
        dump_path_.clear();
    } else {
        cmd->presentDrawable(ri.sc_->GetDrawable());
        cmd->commit();
    }
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
