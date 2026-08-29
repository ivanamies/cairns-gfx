// control/handlers/script_ops.hpp
//
// P5 stub: registers script.eval against an embedded QuickJS context.
// Every previously-registered CommandRegistry op is bound as a JS function
// on a `cairns` global so a snippet can call e.g. `cairns.app_ping()`
// instead of issuing one NDJSON line per op (matters for scenes that need
// thousands of instantiate calls -- one script.eval roundtrip vs 3200 lines).

#pragma once

#include "util/chunk_allocator.hpp"  // ScriptHost::js_heap backing

// QuickJS handles, forward-declared so callers that merely OWN a ScriptHost
// (main/serve/web) don't pull in quickjs.h; the ctor/dtor are out-of-line.
struct JSRuntime;
struct JSContext;

namespace cairns::control {

class CommandRegistry;

// Per-caller QuickJS state, owned alongside the CommandRegistry it serves (no
// process singleton). Holds the runtime + current context + the JS heap
// reservation. The runtime is created lazily in RegisterScriptOps; the dtor
// drains pending jobs then frees the context + runtime (order matters for the
// clean-shutdown asserts inside QuickJS).
struct ScriptHost {
    // Declared FIRST so it destructs LAST -- after ~ScriptHost's JS_FreeRuntime
    // has returned every JS allocation to it (outstanding == 0).
    cairns::ChunkAllocator js_heap;
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    CommandRegistry* registry = nullptr;

    ScriptHost() = default;
    ~ScriptHost();
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;
};

// Call AFTER all other ops are registered: the binder snapshots the current
// registry contents and exposes each name as `cairns.<name_safe>` in JS.
// |host| is bound into each JSContext via JS_SetContextOpaque so the JS->C++
// dispatch bridge reaches it without any static state.
void RegisterScriptOps(CommandRegistry& registry, ScriptHost& host);

}  // namespace cairns::control
