// scene/component_type.hpp
//
// A plain component-type id shared by the generic add/get/removeComponent
// ops (control side), engine_headless, and Engine. Kept dependency-free (no
// entt/glm) so the control layer can name a component type without pulling
// the engine in. This is the extension seam: CV-tool / effect /
// diffusion-stage components add an enumerator + a table row + typed
// accessors, and tools.list advertises them automatically -- no bespoke ops.

#pragma once

#include <cstdint>

namespace cairns {

enum class ComponentType : uint32_t {
    kName,
    kCamera,
    kParticleEmitter,
    kRenderable,
    kTransform,
    kInvalid,
};

}  // namespace cairns
