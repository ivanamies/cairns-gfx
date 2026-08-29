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

// Note: SetRandomSeed must run BEFORE GreaterInit's initParticles for the
// new seed to take effect on particle init. Calling at runtime updates the
// field but leaves the in-flight particle state alone.
void SetRandomSeed(Engine* engine, uint32_t seed);

// Reallocate final_target_ at the given dims. Returns false if the engine
// is null, not surfaceless, or the allocation failed. Used by
// window.resize in headless mode.
bool ResizeFinalTarget(Engine* engine, uint32_t w, uint32_t h);

uint32_t GetFinalTargetWidth(Engine* engine);
uint32_t GetFinalTargetHeight(Engine* engine);

}  // namespace headless
}  // namespace cairns
