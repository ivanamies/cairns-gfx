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
#include "rhi/metal/command_recorder_impl.hpp"
#include "rhi/metal/internal/frames_impl.hpp"
#include "rhi/metal/internal/device_impl.hpp"

namespace cairns::rhi {

Frames::~Frames() { Deinit(); }

bool Frames::Init(Device& device, Resources& resources) {
    if (impl_) {
        return true;
    }
    impl_ = new Impl();
    impl_->device = device.impl_->device;
    impl_->queue = device.impl_->queue;
    impl_->res = &resources;

    impl_->frame_semaphore = dispatch_semaphore_create(kFramesInFlight);
    {
        MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
        dsd->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        dsd->setDepthWriteEnabled(true);
        impl_->depth_stencil = impl_->device->newDepthStencilState(dsd);
        dsd->release();
    }
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
        impl_->msaa_handle = impl_->res->CreateTexture(d);
        if (impl_->msaa_handle.IsNull()) {
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
        impl_->depth_handle = impl_->res->CreateTexture(d);
        if (impl_->depth_handle.IsNull()) {
            return false;
        }
    }
    MTL::Texture* msaa = impl_->res->GetHot(impl_->msaa_handle)->api_view;
    MTL::Texture* depth = impl_->res->GetHot(impl_->depth_handle)->api_view;
    return InitRenderPassDescriptor(impl_->render_pass_desc, msaa, depth, sc);
}

void Frames::Deinit() {
    if (!impl_) {
        return;
    }
    if (impl_->depth_stencil) {
        impl_->depth_stencil->release();
    }
    if (impl_->render_pass_desc) {
        impl_->render_pass_desc->release();
    }
    delete impl_;
    impl_ = nullptr;
}

void Frames::SetDumpPath(const std::filesystem::path& path) {
    impl_->dump_path = path;
}

FrameContext Frames::Begin(SwapChain& sc) {
    dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(impl_->frame_semaphore),
                            DISPATCH_TIME_FOREVER);
    impl_->res->AdvanceFrame();  // bump ring reset

    sc.NextDrawable();
    MTL::Texture* msaa = impl_->res->GetHot(impl_->msaa_handle)->api_view;
    MTL::Texture* depth = impl_->res->GetHot(impl_->depth_handle)->api_view;
    UpdateRenderPassDescriptor(impl_->render_pass_desc, msaa, depth, sc);

    MTL::CommandBuffer* cmd = impl_->queue->commandBuffer();
    dispatch_semaphore_t sem = static_cast<dispatch_semaphore_t>(impl_->frame_semaphore);
    cmd->addCompletedHandler([sem](MTL::CommandBuffer*) { dispatch_semaphore_signal(sem); });

    FrameContext fc;
    fc.frame_index = 0;
    fc.swapchain_image_index = 0;
    fc.cmd.impl_ = new CommandRecorder::Impl{impl_->res,    &sc,  cmd, nullptr,
                                             impl_->render_pass_desc,
                                             impl_->depth_stencil};
    return fc;
}

void Frames::End(FrameContext& fc) {
    CommandRecorder::Impl* ri = fc.cmd.impl_;
    MTL::CommandBuffer* cmd = ri->cmd;

    if (!impl_->dump_path.empty()) {
        MTL::Texture* drawableTex = ri->sc->GetDrawable()->texture();
        const NS::UInteger w = drawableTex->width();
        const NS::UInteger h = drawableTex->height();
        const NS::UInteger bytesPerRow = w * 4;
        const NS::UInteger bufSize = bytesPerRow * h;
        MTL::Buffer* readback = impl_->device->newBuffer(bufSize, MTL::ResourceStorageModeShared);
        MTL::BlitCommandEncoder* blitEnc = cmd->blitCommandEncoder();
        blitEnc->copyFromTexture(drawableTex, 0, 0, MTL::Origin{0, 0, 0}, MTL::Size{w, h, 1},
                                 readback, 0, bytesPerRow, 0);
        blitEnc->endEncoding();
        cmd->presentDrawable(ri->sc->GetDrawable());
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
        stbi_write_png(impl_->dump_path.string().c_str(), static_cast<int>(w),
                       static_cast<int>(h), 4, rgba.data(), static_cast<int>(bytesPerRow));
        readback->release();
        impl_->dump_path.clear();
    } else {
        cmd->presentDrawable(ri->sc->GetDrawable());
        cmd->commit();
    }

    delete ri;
    fc.cmd.impl_ = nullptr;
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
