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

namespace {

void make_render_targets(Resources& res, Allocator& alloc, uint32_t w, uint32_t h,
                         Handle<Texture>& msaa_out, Handle<Texture>& depth_out) {
    constexpr uint32_t kSampleCount = 4;
    TextureDesc cd;
    cd.dimensions = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    cd.format = Format::kBgra8Unorm;
    cd.sample_count = kSampleCount;
    cd.usage = kTexUsageColorTarget;
    cd.memory = Memory::kDefault;
    msaa_out = res.CreateTexture(alloc, cd);
    TextureDesc dd;
    dd.dimensions = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
    dd.format = Format::kD32F;
    dd.sample_count = kSampleCount;
    dd.usage = kTexUsageDepthTarget;
    dd.memory = Memory::kDefault;
    depth_out = res.CreateTexture(alloc, dd);
}

}  // namespace

Frames::~Frames() { Deinit(); }

bool Frames::Init(Device& device) {
    if (inited_) {
        return true;
    }
    device_ = device.device_;
    queue_ = device.queue_;

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
bool Frames::InitTargets(Resources& resources, Allocator& alloc, SwapChain& sc) {
    make_render_targets(resources, alloc, sc.Width(), sc.Height(), msaa_handle_,
                        depth_handle_);
    if (msaa_handle_.IsNull() || depth_handle_.IsNull()) {
        return false;
    }
    MTL::Texture* msaa = resources.GetHot(msaa_handle_)->api_view;
    MTL::Texture* depth = resources.GetHot(depth_handle_)->api_view;
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

FrameContext Frames::Begin(Resources& resources, Allocator& alloc, SwapChain& sc) {
    dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(frame_semaphore_),
                            DISPATCH_TIME_FOREVER);
    resources.AdvanceFrame(alloc);  // bump ring reset

    sc.NextDrawable();
    MTL::Texture* drawable_tex = sc.GetDrawable()->texture();
    Texture::Hot* msaa_hot = resources.GetHot(msaa_handle_);
    if (drawable_tex &&
        (!msaa_hot || msaa_hot->api_view->width() != drawable_tex->width() ||
         msaa_hot->api_view->height() != drawable_tex->height())) {
        if (!msaa_handle_.IsNull()) {
            resources.Destroy(alloc, msaa_handle_);
        }
        if (!depth_handle_.IsNull()) {
            resources.Destroy(alloc, depth_handle_);
        }
        make_render_targets(resources, alloc,
                            static_cast<uint32_t>(drawable_tex->width()),
                            static_cast<uint32_t>(drawable_tex->height()),
                            msaa_handle_, depth_handle_);
    }
    MTL::Texture* msaa = resources.GetHot(msaa_handle_)->api_view;
    MTL::Texture* depth = resources.GetHot(depth_handle_)->api_view;
    UpdateRenderPassDescriptor(render_pass_desc_, msaa, depth, sc);

    FrameContext fc;
    fc.frame_index = 0;
    fc.swapchain_image_index = 0;
    fc.cmd.cmd_ = nullptr;
    fc.cmd.queue_ = queue_;
    fc.cmd.enc_ = nullptr;
    fc.cmd.render_pass_desc_ = render_pass_desc_;
    fc.cmd.depth_stencil_ = depth_stencil_;
    fc.cmd.pending_name_ = nullptr;
    fc.cmd.pending_slot_ = -1;
    return fc;
}

void Frames::End(SwapChain& sc, FrameContext& fc) {
    CommandRecorder& ri = fc.cmd;
    if (ri.cmd_ != nullptr) {
        // Encoded work without a PassTimerEnd -- commit the orphan so the GPU
        // sees it before the terminal buffer presents.
        ri.cmd_->commit();
        ri.cmd_ = nullptr;
    }

    MTL::CommandBuffer* term = queue_->commandBuffer();

    if (!dump_path_.empty()) {
        MTL::Texture* drawableTex = sc.GetDrawable()->texture();
        const NS::UInteger w = drawableTex->width();
        const NS::UInteger h = drawableTex->height();
        const NS::UInteger bytesPerRow = w * 4;
        const NS::UInteger bufSize = bytesPerRow * h;
        MTL::Buffer* readback = device_->newBuffer(bufSize, MTL::ResourceStorageModeShared);
        MTL::BlitCommandEncoder* blitEnc = term->blitCommandEncoder();
        blitEnc->copyFromTexture(drawableTex, 0, 0, MTL::Origin{0, 0, 0}, MTL::Size{w, h, 1},
                                 readback, 0, bytesPerRow, 0);
        blitEnc->endEncoding();
        term->presentDrawable(sc.GetDrawable());
        dispatch_semaphore_t sem = static_cast<dispatch_semaphore_t>(frame_semaphore_);
        term->addCompletedHandler([sem](MTL::CommandBuffer*) { dispatch_semaphore_signal(sem); });
        term->commit();
        term->waitUntilCompleted();
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
        term->presentDrawable(sc.GetDrawable());
        dispatch_semaphore_t sem = static_cast<dispatch_semaphore_t>(frame_semaphore_);
        term->addCompletedHandler([sem](MTL::CommandBuffer*) { dispatch_semaphore_signal(sem); });
        term->commit();
    }
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
