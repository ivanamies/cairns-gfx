// rhi/metal/swap_chain_plat.hpp

#pragma once

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include "rhi/swap_resolve_target.hpp"
#include "util/size.hpp"

namespace cairns::rhi {

struct SwapChainPlat {
    SwapChainPlat() = default;

    // Layer is pre-resolved by the SDL shell via SDL_Metal_CreateView +
    // SDL_Metal_GetLayer and ownership stays with the shell (it also calls
    // SDL_Metal_DestroyView). Surfaceless host never invokes this code path
    // (SwapChain is not constructed there).
    bool Init(MTL::Device* device, CA::MetalLayer* layer) {
        if (!layer) {
            return false;
        }
        metalLayer_ = layer;
        metalLayer_->setDevice(device);
        metalLayer_->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
        // Drawables must be blittable: Frames::End copies from the drawable
        // for the swapchain dump, which Metal forbids on a framebufferOnly
        // layer.
        metalLayer_->setFramebufferOnly(false);
        size_ = cairns::Size(metalLayer_->drawableSize().width,
                             metalLayer_->drawableSize().height);
        return true;
    }

    void Deinit() {}

    bool NextDrawable() {
        if (!metalLayer_) {
            return false;
        }
        metalDrawable_ = metalLayer_->nextDrawable();
        return metalDrawable_ != nullptr;
    }

    CA::MetalDrawable* GetDrawable() const { return metalDrawable_; }

    // Per-frame swap-target acquire. Pulls the next drawable and returns a
    // SwapResolveTarget describing the resolve texture + the drawable to
    // present at Frames::End. The render-to-texture (cairns_serve) path does
    // not construct a SwapChain at all -- it builds its SwapResolveTarget
    // directly via MakeSwapResolveTargetFromTexture.
    SwapResolveTarget AcquireForFrame() {
        NextDrawable();
        SwapResolveTarget t;
        t.width = size_.width;
        t.height = size_.height;
        t.plat.drawable = metalDrawable_;
        t.plat.texture = metalDrawable_ ? metalDrawable_->texture() : nullptr;
        return t;
    }

    void SetDrawableSize(uint32_t width, uint32_t height) {
        size_ = cairns::Size(width, height);
        if (metalLayer_) {
            metalLayer_->setDrawableSize(CGSizeMake(width, height));
        }
    }

    Size GetDrawableSize() const { return size_; }
    uint32_t Width() const { return size_.width; }
    uint32_t Height() const { return size_.height; }

    MTL::PixelFormat GetPixelFormat() const {
        return metalLayer_ ? metalLayer_->pixelFormat()
                           : MTL::PixelFormatBGRA8Unorm;
    }

    Size size_ = cairns::kInvalidSize;
    CA::MetalDrawable* metalDrawable_ = nullptr;
    CA::MetalLayer* metalLayer_ = nullptr;
};

}  // namespace cairns::rhi
