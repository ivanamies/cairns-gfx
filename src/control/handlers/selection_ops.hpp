// control/handlers/selection_ops.hpp

#pragma once

namespace cairns {
class Engine;
}

namespace cairns::control {

class CommandRegistry;

// Register cairns.selection.* + cairns.highlight.* + cairns.pick. Selection
// is document-side state on Engine; these ops are the joint-attention surface
// for the VLM agent (cairns_serve) and the human shell (sdl-min) alike.
void RegisterSelectionOps(CommandRegistry& registry, cairns::Engine* engine);

}  // namespace cairns::control
