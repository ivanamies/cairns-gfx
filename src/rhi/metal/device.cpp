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
#include "rhi/init_config.hpp"
#include "rhi/swap_chain.hpp"

namespace cairns::rhi {

Device::~Device() { Deinit(); }

bool Device::Init(const InitConfig& cfg) {
    (void)cfg;
    if (inited_) {
        return true;
    }
    device_ = MTL::CreateSystemDefaultDevice();
    if (!device_) {
        return false;
    }
    queue_ = device_->newCommandQueue();
    inited_ = (queue_ != nullptr);
    return inited_;
}

void Device::Deinit() {
    if (!inited_) {
        return;
    }
    if (queue_) {
        queue_->release();
    }
    if (device_) {
        device_->release();
    }
    inited_ = false;
}

bool Device::InitSwapChain(SwapChain& sc, const InitConfig& cfg) {
    return sc.Init(device_, cfg.metal_layer);
}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
