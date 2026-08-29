// src/render/worker_context.hpp
//
// Per-worker no-lock memory context. Each worker gets a WorkerContext by
// reference and pulls scratch memory from its arena slice -- never from
// malloc, never from a shared container that would need a mutex.
//
// Arena lifetime = the worker's call frame: resets at slot acquire
// (FrameArena::BeginFrame), lives until the next frame's BeginFrame on
// the same slot.

#pragma once

#include "util/cpu_arena.hpp"

namespace cairns {

struct WorkerContext {
    // Per-frame bump arena. Bump-allocate; no per-object free; resets
    // wholesale when the slot's BeginFrame fires next frame. SAFE for
    // single-worker access only; multi-worker carving means each worker
    // gets its OWN WorkerContext with its own slice.
    BumpArena& frame_arena;
};

}  // namespace cairns
