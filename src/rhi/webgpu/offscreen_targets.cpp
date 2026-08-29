// rhi/webgpu/offscreen_targets.cpp -- WebGPU backend (render passes are
// per-frame; no framebuffer cache).
#include "util/define.hpp"
#if CAIRNS_WEBGPU

#include "rhi/offscreen_targets.hpp"
#include "rhi/device.hpp"

namespace cairns::rhi {

void OffscreenTargets::Init(Device& device) { (void)device; }
void OffscreenTargets::Deinit() {}
void OffscreenTargets::FlushFramebuffers() {}

}  // namespace cairns::rhi
#endif  // CAIRNS_WEBGPU
