// rhi/vulkan/offscreen_targets_plat.hpp
//
// vk owns an OffscreenTargetCache (render-pass +
// framebuffer cache keyed on attachment formats/load-store ops).
// Frames stamps it onto the recorder via fc.cmd.plat.offscreen_;
// engine flushes framebuffers on surface resize.

#pragma once

#include "rhi/vulkan/command_recorder_plat.hpp"  // OffscreenTargetCache

namespace cairns::rhi {

struct OffscreenTargetsPlat {
    OffscreenTargetCache cache;
};

}  // namespace cairns::rhi
