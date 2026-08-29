// rhi/metal/offscreen_targets.cpp -- metal no-ops.

#include "util/define.hpp"

#if CAIRNS_METAL

#include "rhi/offscreen_targets.hpp"

namespace cairns::rhi {

void OffscreenTargets::Init(Device& /*device*/) {}
void OffscreenTargets::Deinit() {}
void OffscreenTargets::FlushFramebuffers() {}

}  // namespace cairns::rhi

#endif  // CAIRNS_METAL
