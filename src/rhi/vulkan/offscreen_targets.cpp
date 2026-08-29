// rhi/vulkan/offscreen_targets.cpp

#include "util/define.hpp"

#if CAIRNS_VULKAN

#include "rhi/offscreen_targets.hpp"

#include "rhi/device.hpp"

namespace cairns::rhi {

void OffscreenTargets::Init(Device& device) {
    plat.cache.device = device.plat.device_;
}

void OffscreenTargets::Deinit() {
    plat.cache.Deinit();
}

void OffscreenTargets::FlushFramebuffers() {
    plat.cache.FlushFramebuffers();
}

}  // namespace cairns::rhi

#endif  // CAIRNS_VULKAN
