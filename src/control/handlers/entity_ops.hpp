// control/handlers/entity_ops.hpp
//
// #229 C4.2: per-entity ops -- the Unity-shaped control surface over a scene's
// entt registry (destroy / setTRS / getTRS / setParent / find / setName).
// Every op is explicit-scene-first ({scene?} -> 0 primary / 1 secondary; the
// active scene is only a convenience default), matching the N-node workflow.
// TRS rides as plain float arrays through engine_headless so control stays
// json / -fno-exceptions with no glm dependency.

#pragma once

namespace cairns { class Engine; }

namespace cairns::control {

class CommandRegistry;

void RegisterEntityOps(CommandRegistry& registry, cairns::Engine& engine);

}  // namespace cairns::control
