// rhi/metal/device.cpp
//
// Metal platform device lifetime: MTL::Device + command queue. Created here
// out of resource_manager.cpp (Phase 0e). ResourceManager mirrors these handle
// values during InitDevice; Device owns their teardown (released after the
// memory allocator frees device heaps — see Engine deinit order).

#include "util/define.hpp"

#if CAIRNS_METAL

#include <Metal/Metal.hpp>

#include "rhi/device.hpp"
#include "rhi/metal/internal/device_impl.hpp"
#include "rhi/swap_chain.hpp"

namespace cairns::rhi {

Device::~Device() { Deinit(); }

bool Device::Init(SDL_Window* window) {
    (void)window;
    if (impl_) {
        return true;
    }
    impl_ = new Impl();
    impl_->device = MTL::CreateSystemDefaultDevice();
    if (!impl_->device) {
        return false;
    }
    impl_->queue = impl_->device->newCommandQueue();
    return impl_->queue != nullptr;
}

void Device::Deinit() {
    if (!impl_) {
        return;
    }
    if (impl_->queue) {
        impl_->queue->release();
    }
    if (impl_->device) {
        impl_->device->release();
    }
    delete impl_;
    impl_ = nullptr;
}

bool Device::InitSwapChain(SwapChain& sc, SDL_Window* window) {
    return sc.Init(impl_->device, window);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
