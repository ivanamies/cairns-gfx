// rhi/webgpu/offscreen_targets_plat.hpp
// Empty OffscreenTargetsPlat so the cross-backend header resolves; webgpu
// keeps no extra per-target platform state.
#pragma once

#include <webgpu/webgpu.h>

namespace cairns::rhi {

struct OffscreenTargetsPlat {};

}  // namespace cairns::rhi
