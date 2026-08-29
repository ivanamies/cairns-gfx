// control/handlers/scene_ops.hpp
//
// P2 scene/viewport/window ops. Most are stubs today (return synthetic ids
// that downstream rendering doesn't yet honor); window.resize is the one
// that does real work (destroys + reallocates final_target_).
//
// Why stub-first: the registry surface (visible via tools.list) is the
// agent-driving contract. Stabilizing the op names + arg shapes now lets
// an external VLM / pip client / mcp server compose scripts against
// cairns_serve immediately, even before the rendering side wires through.

#pragma once

namespace cairns { class Engine; }

namespace cairns::control {

class CommandRegistry;

void RegisterSceneOps(CommandRegistry& registry, cairns::Engine* engine);

}  // namespace cairns::control
