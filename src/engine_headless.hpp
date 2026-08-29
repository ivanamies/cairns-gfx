// engine_headless.hpp
//
// Thin facade over Engine's headless render+dump methods for TUs that must
// not pull engine.hpp (which transitively defines NS/MTL/CA private-
// implementation symbols via gfx_api.hpp; including from more than one TU
// produces duplicate symbols at link). cairns_control's render_ops.cpp uses
// only this facade; the implementation lives in engine_headless.cpp inside
// cairns_core where engine.hpp is instantiated exactly once.

#pragma once

#include <filesystem>

namespace cairns {

class Engine;

namespace headless {

bool RenderFrame(Engine* engine);
bool DumpFinalTarget(Engine* engine, const std::filesystem::path& path);

}  // namespace headless
}  // namespace cairns
