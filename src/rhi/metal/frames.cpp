// rhi/metal/frames.cpp

#include "util/define.hpp"

#if CAIRNS_METAL

#include <dispatch/dispatch.h>
#include <vector>

#include <Metal/Metal.hpp>
#include <stb_image_write.h>

#include "rhi/frames.hpp"
#include "rhi/device.hpp"
#include "rhi/frame_capture.hpp"
#include "rhi/gpu_profiler.hpp"
#include "rhi/resources.hpp"
#include "rhi/resource_manager.hpp"  // kFramesInFlight
#include "rhi/swap_chain.hpp"
#include "rhi/swap_resolve_target.hpp"
#include "rhi/command_recorder.hpp"
#include "util/timer.hpp"

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

void init_render_pass_desc(MTL::RenderPassDescriptor*& rpd,
                            MTL::Texture* msaa, MTL::Texture* depth,
                            MTL::Texture* resolve) {
    rpd = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* color = rpd->colorAttachments()->object(0);
    MTL::RenderPassDepthAttachmentDescriptor* d = rpd->depthAttachment();
    color->setTexture(msaa);
    color->setResolveTexture(resolve);
    color->setLoadAction(MTL::LoadActionClear);
    color->setClearColor(MTL::ClearColor(41.0f / 255.0f, 42.0f / 255.0f,
                                          48.0f / 255.0f, 1.0));
    color->setStoreAction(MTL::StoreActionMultisampleResolve);
    d->setTexture(depth);
    d->setLoadAction(MTL::LoadActionClear);
    d->setStoreAction(MTL::StoreActionDontCare);
    d->setClearDepth(1.0);
}

void update_render_pass_desc(MTL::RenderPassDescriptor* rpd,
                              MTL::Texture* msaa, MTL::Texture* depth,
                              MTL::Texture* resolve) {
    rpd->colorAttachments()->object(0)->setTexture(msaa);
    rpd->colorAttachments()->object(0)->setResolveTexture(resolve);
    rpd->depthAttachment()->setTexture(depth);
}

}  // namespace

Frames::~Frames() { Deinit(); }

bool Frames::Init(Device& device) {
    if (inited_) {
        return true;
    }
    plat.device_ = device.plat.device_;
    plat.queue_ = device.plat.queue_;

    plat.frame_semaphore_ = dispatch_semaphore_create(kFramesInFlight);
    {
        MTL::DepthStencilDescriptor* dsd = MTL::DepthStencilDescriptor::alloc()->init();
        dsd->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
        dsd->setDepthWriteEnabled(true);
        plat.depth_stencil_ = plat.device_->newDepthStencilState(dsd);
        dsd->release();
    }
    inited_ = true;
    return true;
}

// Metal MSAA/depth targets share the texture Pool index space with scene
// textures; created after scene textures load (call post scene load).
bool Frames::InitTargets(Resources& resources, Allocator& alloc,
                          uint32_t width, uint32_t height) {
    make_render_targets(resources, alloc, width, height, plat.msaa_handle_,
                        plat.depth_handle_);
    if (plat.msaa_handle_.IsNull() || plat.depth_handle_.IsNull()) {
        return false;
    }
    MTL::Texture* msaa = resources.GetHot(plat.msaa_handle_)->api_view;
    MTL::Texture* depth = resources.GetHot(plat.depth_handle_)->api_view;
    // Resolve target is bound per-frame in Begin() from the SwapResolveTarget;
    // here we just allocate the descriptor with a null resolve attachment.
    init_render_pass_desc(plat.render_pass_desc_, msaa, depth, nullptr);
    return true;
}

void Frames::Deinit() {
    if (!inited_) {
        return;
    }
    if (plat.depth_stencil_) {
        plat.depth_stencil_->release();
    }
    if (plat.render_pass_desc_) {
        plat.render_pass_desc_->release();
    }
    inited_ = false;
}

// Metal: drawable resize is implicit per-frame (see frames.cpp:101-120 -- the
// MSAA + depth targets are reallocated when drawable size changes). Nothing
// to flush.
void Frames::OnSurfaceResize() {}

void Frames::WriteUnlitDescriptors(Resources& /*resources*/,
                                     Allocator& /*alloc*/) {
    // Metal: no per-frame descriptor sets in the unlit path; render
    // pass globals + per-draw uniforms are bound directly via
    // setVertexBytes / setFragmentBuffer in the recorder.
}

FrameContext Frames::Begin(Resources& resources, Allocator& alloc,
                            GpuProfiler& /*gpu_profiler*/,
                            const SwapResolveTarget& target) {
    dispatch_semaphore_wait(static_cast<dispatch_semaphore_t>(plat.frame_semaphore_),
                            DISPATCH_TIME_FOREVER);
    resources.AdvanceFrame(alloc);  // bump ring reset

    MTL::Texture* swap_tex = target.plat.texture;
    Texture::Hot* msaa_hot = resources.GetHot(plat.msaa_handle_);
    if (swap_tex &&
        (!msaa_hot || msaa_hot->api_view->width() != swap_tex->width() ||
         msaa_hot->api_view->height() != swap_tex->height())) {
        if (!plat.msaa_handle_.IsNull()) {
            resources.Destroy(alloc, plat.msaa_handle_);
        }
        if (!plat.depth_handle_.IsNull()) {
            resources.Destroy(alloc, plat.depth_handle_);
        }
        make_render_targets(resources, alloc,
                            static_cast<uint32_t>(swap_tex->width()),
                            static_cast<uint32_t>(swap_tex->height()),
                            plat.msaa_handle_, plat.depth_handle_);
    }
    MTL::Texture* msaa = resources.GetHot(plat.msaa_handle_)->api_view;
    MTL::Texture* depth = resources.GetHot(plat.depth_handle_)->api_view;
    update_render_pass_desc(plat.render_pass_desc_, msaa, depth, swap_tex);

    FrameContext fc;
    fc.frame_index = 0;
    fc.swapchain_image_index = 0;
    fc.cmd.plat.cmd_ = nullptr;
    fc.cmd.plat.queue_ = plat.queue_;
    fc.cmd.plat.enc_ = nullptr;
    fc.cmd.plat.render_pass_desc_ = plat.render_pass_desc_;
    fc.cmd.plat.depth_stencil_ = plat.depth_stencil_;
    fc.cmd.pending_name_ = nullptr;
    fc.cmd.pending_slot_ = -1;
    return fc;
}

void Frames::Present(const SwapResolveTarget& /*target*/,
                       FrameCapture& /*frame_capture*/,
                       FrameContext& /*fc*/) {
    // Metal presentDrawable is enqueued via the command buffer in
    // EndSubmit (thread-safe per Apple's command-buffer rules). No
    // main-thread-only work to do here. Kept for contract symmetry with
    // the vk path -- the engine calls this on the main thread regardless.
}

void Frames::EndSubmit(const SwapResolveTarget& target,
                        FrameCapture& frame_capture, FrameContext& fc) {
    CommandRecorder& ri = fc.cmd;
    if (ri.plat.cmd_ != nullptr) {
        // Encoded work without a PassTimerEnd -- commit the orphan so the GPU
        // sees it before the terminal buffer presents.
        ri.plat.cmd_->commit();
        ri.plat.cmd_ = nullptr;
    }

    MTL::CommandBuffer* term = plat.queue_->commandBuffer();
    MTL::Texture* swap_tex = target.plat.texture;
    CA::MetalDrawable* drawable = target.plat.drawable;

    if (frame_capture.Pending() && swap_tex) {
        const NS::UInteger w = swap_tex->width();
        const NS::UInteger h = swap_tex->height();
        const NS::UInteger bytesPerRow = w * 4;
        const NS::UInteger bufSize = bytesPerRow * h;
        MTL::Buffer* readback = plat.device_->newBuffer(bufSize, MTL::ResourceStorageModeShared);
        MTL::BlitCommandEncoder* blitEnc = term->blitCommandEncoder();
        blitEnc->copyFromTexture(swap_tex, 0, 0, MTL::Origin{0, 0, 0}, MTL::Size{w, h, 1},
                                 readback, 0, bytesPerRow, 0);
        blitEnc->endEncoding();
        if (drawable) {
            term->presentDrawable(drawable);
        }
        dispatch_semaphore_t sem = static_cast<dispatch_semaphore_t>(plat.frame_semaphore_);
        std::atomic<double>* gpu_end_slot = &plat.last_gpu_end_s_;
        term->addCompletedHandler([sem, gpu_end_slot](MTL::CommandBuffer* cb) {
            gpu_end_slot->store(cb->GPUEndTime(), std::memory_order_release);
            dispatch_semaphore_signal(sem);
        });
        if (drawable) {
            drawable->addPresentedHandler(
                [gpu_end_slot](MTL::Drawable* d) {
                    const double gpu_end_s =
                        gpu_end_slot->load(std::memory_order_acquire);
                    const double presented_s = d->presentedTime();
                    if (gpu_end_s > 0.0 && presented_s >= gpu_end_s) {
                        const uint64_t us = static_cast<uint64_t>(
                            (presented_s - gpu_end_s) * 1.0e6);
                        cairns::TimerStorage::Span(
                            cairns::TimerStorage::SlotForPass("present_pacing"),
                            "present_pacing", us);
                    }
                });
        }
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
        stbi_write_png(frame_capture.Path().string().c_str(), static_cast<int>(w),
                       static_cast<int>(h), 4, rgba.data(), static_cast<int>(bytesPerRow));
        readback->release();
        frame_capture.Clear();
    } else {
        if (drawable) {
            term->presentDrawable(drawable);
        }
        dispatch_semaphore_t sem = static_cast<dispatch_semaphore_t>(plat.frame_semaphore_);
        std::atomic<double>* gpu_end_slot = &plat.last_gpu_end_s_;
        term->addCompletedHandler([sem, gpu_end_slot](MTL::CommandBuffer* cb) {
            gpu_end_slot->store(cb->GPUEndTime(), std::memory_order_release);
            dispatch_semaphore_signal(sem);
        });
        if (drawable) {
            drawable->addPresentedHandler(
                [gpu_end_slot](MTL::Drawable* d) {
                    const double gpu_end_s =
                        gpu_end_slot->load(std::memory_order_acquire);
                    const double presented_s = d->presentedTime();
                    if (gpu_end_s > 0.0 && presented_s >= gpu_end_s) {
                        const uint64_t us = static_cast<uint64_t>(
                            (presented_s - gpu_end_s) * 1.0e6);
                        cairns::TimerStorage::Span(
                            cairns::TimerStorage::SlotForPass("present_pacing"),
                            "present_pacing", us);
                    }
                });
        }
        term->commit();
        if (!drawable) {
            // Render-to-texture: synchronous so a subsequent texture readback
            // (e.g. io.dumpTexture) sees the dump's pixels.
            term->waitUntilCompleted();
        }
    }
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
