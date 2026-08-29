#pragma once

#include "rhi/gfx_api.hpp"
#include "rhi/device.hpp"
#include "util/size.hpp"

namespace cairns::rhi {
    
struct SwapChain {
    SwapChain() { }
    
    bool Init(Device& device, SDL_Window* window) {
        metalView_ = SDL_Metal_CreateView(window);
        if (!metalView_) {
            return false;
        }
        
        // Get the backing CAswapChain_->metalLayer_ and cast it to the metal-cpp C++ wrapper
        // SDL returns a void*, we cast to CA::swapChain_->metalLayer_* provided by QuartzCore.hpp
        metalLayer_ = static_cast<CA::MetalLayer*>(SDL_Metal_GetLayer(metalView_));
        if (!metalLayer_) {
            return false;
        }
        
        metalLayer_->setDevice(device.get());
        metalLayer_->setPixelFormat(MTL::PixelFormatBGRA8Unorm);

        size_ = cairns::Size(metalLayer_->drawableSize().width, metalLayer_->drawableSize().height);
        
        return true;

    }
    
    void Deinit() {
        if (metalView_) {
            SDL_Metal_DestroyView(metalView_);
        }
    }
    
    bool NextDrawable() {
        metalDrawable_ = metalLayer_->nextDrawable();
        return metalDrawable_ != nullptr;
    }
    
    CA::MetalDrawable* GetDrawable() const {
        return metalDrawable_;
    }
    
    void SetDrawableSize(uint32_t width, uint32_t height) {
        size_ = cairns::Size(width, height);
        metalLayer_->setDrawableSize(CGSizeMake(width, height));
    }
    
    Size GetDrawableSize() const {
        return size_;
    }
    
    MTL::PixelFormat GetPixelFormat() const {
        return metalLayer_->pixelFormat();
    }
    
private:
    Size size_ = cairns::kInvalidSize;
    CA::MetalDrawable* metalDrawable_ = nullptr;
    SDL_MetalView metalView_ = nullptr;
    CA::MetalLayer* metalLayer_ = nullptr;
};
    
} // cairns::rhi
