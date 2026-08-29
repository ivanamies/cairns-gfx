// rhi/swap_chain.hpp
//
// Backend swapchain + render targets (MSAA color, depth) + render pass +
// framebuffers. Owns everything needed to begin a frame's render pass and
// present. Device/instance/queue/surface are created by the app and passed in.
//
// Composition-not-ifdef shape: all backend state + helpers live on
// SwapChainPlat (rhi/{metal,vulkan}/swap_chain_plat.hpp). The common
// SwapChain holds `plat` plus thin forwarders for the methods that share a
// signature across backends (Width / Height / Deinit). Backend-specific
// fields go through `sc.plat.X`. Per-frame swap-target acquire is on the
// common class because the returned SwapResolveTarget needs the OWNING
// SwapChain* (vk) or the per-backend drawable pointer (metal).

#pragma once

#include "util/define.hpp"
#include "rhi/swap_resolve_target.hpp"

#if CAIRNS_METAL
#include "rhi/metal/swap_chain_plat.hpp"
#elif CAIRNS_VULKAN
#include "rhi/vulkan/swap_chain_plat.hpp"
#endif

namespace cairns::rhi {

struct SwapChain {
    SwapChainPlat plat;

    uint32_t Width() const { return plat.Width(); }
    uint32_t Height() const { return plat.Height(); }
    void Deinit() { plat.Deinit(); }

    // Per-frame swap target. On vk the returned target carries an owner
    // pointer back to *this* SwapChain (Frames + CommandRecorder dereference
    // .plat.swap_chain -> rhi::SwapChain* for renderPass / framebuffer /
    // extent). On metal the plat-side AcquireForFrame fully populates the
    // resolve target -- no owner pointer needed.
    SwapResolveTarget AcquireForFrame() {
        SwapResolveTarget t = plat.AcquireForFrame();
#if CAIRNS_VULKAN
        t.plat.swap_chain = this;
#endif
        return t;
    }
};

#if CAIRNS_METAL

// resolve_override: when non-null, used as the MSAA-resolve texture
// instead of swap_chain.plat.GetDrawable()->texture(). Surfaceless mode
// passes the engine's final_target_ texture so the swap pass writes into
// it directly instead of a swapchain drawable.
inline bool InitRenderPassDescriptor(MTL::RenderPassDescriptor*& renderPassDescriptor,
                                     MTL::Texture* msaa, MTL::Texture* depth,
                                     SwapChain& swap_chain,
                                     MTL::Texture* resolve_override = nullptr) {
    renderPassDescriptor = MTL::RenderPassDescriptor::alloc()->init();
    MTL::RenderPassColorAttachmentDescriptor* colorAttachment =
        renderPassDescriptor->colorAttachments()->object(0);
    MTL::RenderPassDepthAttachmentDescriptor* depthAttachment =
        renderPassDescriptor->depthAttachment();
    colorAttachment->setTexture(msaa);
    MTL::Texture* resolve_tex = resolve_override
        ? resolve_override
        : (swap_chain.plat.GetDrawable() ? swap_chain.plat.GetDrawable()->texture()
                                          : nullptr);
    colorAttachment->setResolveTexture(resolve_tex);
    colorAttachment->setLoadAction(MTL::LoadActionClear);
    colorAttachment->setClearColor(MTL::ClearColor(41.0f / 255.0f, 42.0f / 255.0f,
                                                   48.0f / 255.0f, 1.0));
    colorAttachment->setStoreAction(MTL::StoreActionMultisampleResolve);
    depthAttachment->setTexture(depth);
    depthAttachment->setLoadAction(MTL::LoadActionClear);
    depthAttachment->setStoreAction(MTL::StoreActionDontCare);
    depthAttachment->setClearDepth(1.0);
    return true;
}

inline bool UpdateRenderPassDescriptor(MTL::RenderPassDescriptor* render_pass_desc,
                                       MTL::Texture* msaa, MTL::Texture* depth,
                                       SwapChain& swap_chain,
                                       MTL::Texture* resolve_override = nullptr) {
    render_pass_desc->colorAttachments()->object(0)->setTexture(msaa);
    MTL::Texture* resolve_tex = resolve_override
        ? resolve_override
        : (swap_chain.plat.GetDrawable() ? swap_chain.plat.GetDrawable()->texture()
                                          : nullptr);
    render_pass_desc->colorAttachments()->object(0)->setResolveTexture(resolve_tex);
    render_pass_desc->depthAttachment()->setTexture(depth);
    return true;
}

#endif  // CAIRNS_METAL

}  // namespace cairns::rhi
