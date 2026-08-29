// engine_headless.hpp
//
// Thin facade over Engine's headless render+dump methods for TUs that must
// not pull engine.hpp (which transitively defines NS/MTL/CA private-
// implementation symbols via gfx_api.hpp; including from more than one TU
// produces duplicate symbols at link). cairns_control's render_ops.cpp uses
// only this facade; the implementation lives in engine_headless.cpp inside
// cairns_core where engine.hpp is instantiated exactly once.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "scene/selection.hpp"

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

// Windowed-mode dump: queue a swapchain readback for the NEXT presented
// frame and write it to `path`. Lives on this facade because the live agent
// transport in cairns_app reuses the same registry shape as cairns_serve.
// Returns false if the engine is null (the request itself just queues the
// path; the actual readback happens in the next Frames::End).
bool RequestWindowDump(Engine* engine, const std::filesystem::path& path);

// P4 selection / highlight / pick. Document-side state on Engine; this facade
// just forwards through. revision counters tick on every mutation so the
// protocol's cairns.selection.changed event has something to compare against.
std::vector<SelectionTarget> GetSelection(Engine* engine);
std::vector<SelectionTarget> GetHighlights(Engine* engine);
uint32_t GetSelectionRevision(Engine* engine);
void SetSelection(Engine* engine, std::vector<SelectionTarget>&& targets);
void AddSelection(Engine* engine, const SelectionTarget& target);
void RemoveSelection(Engine* engine, const SelectionTarget& target);
void ClearSelection(Engine* engine);
void SetHighlights(Engine* engine, std::vector<SelectionTarget>&& targets);
void ClearHighlights(Engine* engine);
void RequestPick(Engine* engine, int viewport, uint32_t x, uint32_t y);

}  // namespace headless
}  // namespace cairns
