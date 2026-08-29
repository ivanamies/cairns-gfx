// control/handlers/render_ops.hpp
//
// Render-bound ops registered against an Engine instance: render.frame
// (one headless frame into final_target_) and io.dumpTexture (read back a
// target to PNG).

#pragma once

namespace cairns { class Engine; }

namespace cairns::control {

class CommandRegistry;

void RegisterRenderOps(CommandRegistry& registry, cairns::Engine& engine);

}  // namespace cairns::control
