// control/handlers/render_ops.hpp
//
// Render-bound ops registered against an Engine instance: render.frame and
// io.dumpTexture. P1C-minimal: render.frame runs Engine::RenderHeadlessFrame
// (clear-only); io.dumpTexture("final", path) reads back final_target_ via
// Engine::DumpFinalTarget. P2 grows render.frame into the full scene path.

#pragma once

namespace cairns { class Engine; }

namespace cairns::control {

class CommandRegistry;

void RegisterRenderOps(CommandRegistry& registry, cairns::Engine* engine);

}  // namespace cairns::control
