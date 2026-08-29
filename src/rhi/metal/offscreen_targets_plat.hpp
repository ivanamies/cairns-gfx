// rhi/metal/offscreen_targets_plat.hpp
//
// Metal has no offscreen-FB cache (drawable + per-pass render
// descriptors handle it intrinsically). Empty plat keeps the
// public API consistent with vk.

#pragma once

namespace cairns::rhi {

struct OffscreenTargetsPlat {};

}  // namespace cairns::rhi
