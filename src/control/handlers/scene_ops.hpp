// control/handlers/scene_ops.hpp
//
// Scene/prefab/viewport/window ops. The registry surface (visible via
// tools.list) is the agent-driving contract: op names + arg shapes stay
// stable so an external VLM / pip client / MCP server can compose scripts
// against cairns_serve.

#pragma once

namespace cairns { class Engine; }

namespace cairns::control {

class CommandRegistry;

void RegisterSceneOps(CommandRegistry& registry, cairns::Engine& engine);

}  // namespace cairns::control
