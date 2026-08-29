#include "control/handlers/script_ops.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "control/command_registry.hpp"
#include "control/handlers/studio_js.hpp"
#include "platform/platform.hpp"        // ReadAsset for scenario.load
#include "util/chunk_allocator.hpp"  // #229 M7: QuickJS heap backing
#include "util/json.hpp"
#include "util/memory_budget.hpp"
#include "util/misc.hpp"                 // GetBasePathSafe / GetStaticResourceFilepath

#include <cstddef>
#include <cstdio>

extern "C" {
#include "quickjs.h"
}

namespace cairns::control {

namespace {

// #229 M7: route QuickJS's allocator to a fixed ChunkAllocator reservation
// (MemoryBudget::js_heap_bytes, 256 MB). A size-class free-list is the right
// tool for QuickJS's millions of tiny allocations -- NOT the offset allocator,
// which is for the GPU range domain. Bounds the JS heap by construction; cells
// recycle within the reservation across context reloads (no OS churn). `opaque`
// is the per-runtime ChunkAllocator passed to JS_NewRuntime2.
void* JsHeapCalloc(void* opaque, size_t count, size_t size) {
    if (size != 0 && count > (static_cast<size_t>(-1) / size)) {
        return nullptr;  // overflow guard, matches libc calloc
    }
    const size_t bytes = count * size;
    void* p = static_cast<cairns::ChunkAllocator*>(opaque)->Allocate(
        static_cast<uint32_t>(bytes));
    if (p != nullptr) {
        std::memset(p, 0, bytes);
    }
    return p;
}
void* JsHeapMalloc(void* opaque, size_t size) {
    return static_cast<cairns::ChunkAllocator*>(opaque)->Allocate(
        static_cast<uint32_t>(size));
}
void JsHeapFree(void* opaque, void* ptr) {
    static_cast<cairns::ChunkAllocator*>(opaque)->Free(ptr);
}
void* JsHeapRealloc(void* opaque, void* ptr, size_t size) {
    return static_cast<cairns::ChunkAllocator*>(opaque)->Reallocate(
        ptr, static_cast<uint32_t>(size));
}
size_t JsHeapUsableSize(const void* ptr) {
    return static_cast<size_t>(cairns::ChunkAllocator::UsableSize(ptr));
}
const JSMallocFunctions kJsMallocFuncs = {
    JsHeapCalloc, JsHeapMalloc, JsHeapFree, JsHeapRealloc, JsHeapUsableSize,
};

// Lazily create the QuickJS runtime + its heap reservation on |s| (once per
// ScriptHost). The context is created separately in ResetJsContext.
void EnsureRuntime(ScriptHost& s) {
    if (s.rt) {
        return;
    }
    // #229 M7: bound the QuickJS heap to a fixed ChunkAllocator reservation.
    const uint64_t js_bytes = cairns::MemoryBudget::Default().js_heap_bytes;
    s.js_heap.InitReserved(js_bytes);
    s.rt = JS_NewRuntime2(&kJsMallocFuncs, &s.js_heap);
    if (s.rt) {
        // Belt-and-braces: QuickJS self-limits (graceful JS OOM) before the
        // ChunkAllocator's hard cap returns null.
        JS_SetMemoryLimit(s.rt, static_cast<size_t>(js_bytes));
    }
}

// Drain pending jobs, drop the current JSContext, and create a fresh one in
// the same JSRuntime. Both RegisterScriptOps (re-registration, e.g. the tests
// rebinding ops against a fresh engine per scenario) and the R3 reload op use
// this so studio_js always evals into a CLEAN global scope -- re-evaling it on
// a context that already declared its globals throws "redeclaration of <X>"
// (const/class bindings are not idempotent).
void ResetJsContext(ScriptHost& s) {
    if (!s.rt) {
        return;
    }
    if (s.ctx) {
        for (int i = 0; i < 10000; ++i) {
            JSContext* job_ctx = nullptr;
            if (JS_ExecutePendingJob(s.rt, &job_ctx) <= 0) {
                break;
            }
        }
        JS_FreeContext(s.ctx);
        s.ctx = nullptr;
    }
    s.ctx = JS_NewContext(s.rt);
    // Bind the host onto the context so JsDispatch reaches it via
    // JS_GetContextOpaque -- no static state.
    if (s.ctx) {
        JS_SetContextOpaque(s.ctx, &s);
    }
}

// Forward-decl: JsDispatch is defined below.
JSValue JsDispatch(JSContext* ctx, JSValueConst this_val, int argc,
                   JSValueConst* argv);

// Bind `cairns.dispatch` on |ctx|'s global + eval studio_js. Used by
// initial RegisterScriptOps AND by R3's reload after a fresh JSContext
// is created.
void BindAndAutoloadStudio(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue cairns_obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, cairns_obj, "dispatch",
                      JS_NewCFunction(ctx, &JsDispatch, "dispatch", 2));
    // #229: instantiate-pass count for the boot workload, platform-aware so
    // run.js renders 100 actors on mobile (Adreno/Apple tile budget) vs 300 on
    // desktop. Desktop was 500 (5 passes) but 500 actors' skinned output is
    // ~288 MB, over the 256 MB skin pool we cap at for WebGPU portability (the
    // S22/Adreno + WebGPU storage-bind floor), so desktop drops to 3 passes = 300
    // actors. Derived from the persistent budget (mobile is floored to 256 MB) to
    // avoid duplicating the __ANDROID__/iOS guard from memory_budget.hpp.
    const bool mobile =
        cairns::MemoryBudget::Default().cpu_persistent_bytes <=
        512ull * 1024 * 1024;
    JS_SetPropertyStr(ctx, cairns_obj, "instancePasses",
                      JS_NewInt32(ctx, mobile ? 1 : 3));
    JS_SetPropertyStr(ctx, global, "cairns", cairns_obj);
    JS_FreeValue(ctx, global);

    const char* src = kStudioJsSource;
    const size_t src_len = std::strlen(src);
    JSValue v = JS_Eval(ctx, src, src_len, "<studio.js>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue err = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, err);
        std::fprintf(stderr, "[Studio] autoload FAILED: %s\n",
                     msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, err);
    }
    JS_FreeValue(ctx, v);
}

// #228 R3: drop the current JSContext and start fresh -- new context in
// the same JSRuntime, re-bind cairns.dispatch, re-eval studio_js. The
// runtime is preserved so QuickJS's internal heap survives; only the
// per-context scope is reset. All state in the active script is
// host-side (cairns.dispatch is a thin command surface), so "state
// survives the swap" needs no serialization.
bool ReloadJsContext(ScriptHost& s) {
    if (!s.rt) {
        return false;
    }
    ResetJsContext(s);
    if (!s.ctx) {
        return false;
    }
    BindAndAutoloadStudio(s.ctx);
    return true;
}

// JS callable: cairns.dispatch(opName, argsObjectOrUndef) -> resultObject
// Dispatches through the C++ registry. argsObject is JSON-stringified to
// reuse the existing json::parse path; result is parsed back.
JSValue JsDispatch(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                   JSValueConst* argv) {
    ScriptHost* s = static_cast<ScriptHost*>(JS_GetContextOpaque(ctx));
    if (!s || !s->registry) {
        return JS_ThrowInternalError(ctx, "registry not bound");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "dispatch(op, args?)");
    }
    const char* op = JS_ToCString(ctx, argv[0]);
    if (!op) {
        return JS_EXCEPTION;
    }
    json req;
    req["op"] = op;
    JS_FreeCString(ctx, op);
    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        // Stringify argv[1] via JS itself, then parse back into json.
        // Each intermediate value gets explicitly freed; previously the
        // global + JSON object refs leaked once per call, which built up
        // into a shutdown-time refcount assert when scripts called
        // cairns.dispatch even once (reproduced 2026-06-05).
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue json_obj = JS_GetPropertyStr(ctx, global, "JSON");
        JSValue stringify = JS_GetPropertyStr(ctx, json_obj, "stringify");
        JSValue js_str = JS_Call(ctx, stringify, JS_UNDEFINED, 1, &argv[1]);
        JS_FreeValue(ctx, stringify);
        JS_FreeValue(ctx, json_obj);
        JS_FreeValue(ctx, global);
        if (JS_IsException(js_str)) {
            return js_str;
        }
        const char* s_cstr = JS_ToCString(ctx, js_str);
        if (s_cstr) {
            try {
                req["args"] = json::parse(s_cstr);
            } catch (...) {
                // ignore; pass-through as missing args
            }
            JS_FreeCString(ctx, s_cstr);
        }
        JS_FreeValue(ctx, js_str);
    }
    const json resp = s->registry->Dispatch(req);
    const std::string resp_str = resp.dump();
    return JS_ParseJSON(ctx, resp_str.c_str(), resp_str.size(), "<resp>");
}

}  // namespace

// ScriptHost dtor: drain pending jobs, then free context + runtime. Out-of-line
// so callers that only OWN a ScriptHost don't need quickjs.h.
ScriptHost::~ScriptHost() {
    if (rt) {
        // Drain any pending microtasks before tearing down. Without this, an
        // async eval that left state in the job queue trips a clean-shutdown
        // assert inside JS_FreeRuntime (SIGABRT after the last response flush).
        for (int i = 0; i < 10000; ++i) {
            JSContext* job_ctx = nullptr;
            if (JS_ExecutePendingJob(rt, &job_ctx) <= 0) {
                break;
            }
        }
    }
    if (ctx) {
        JS_FreeContext(ctx);
    }
    if (rt) {
        JS_FreeRuntime(rt);
    }
}

void RegisterScriptOps(CommandRegistry& registry, ScriptHost& host) {
    EnsureRuntime(host);
    host.registry = &registry;
    // Fresh context each registration so the studio autoload never redeclares
    // its globals (tests re-register per scenario against the persistent JS
    // runtime).
    ResetJsContext(host);
    BindAndAutoloadStudio(host.ctx);

    registry.Register(
        "cairns.script.reload",
        json::object(),
        "#228 R3: tear down the current JSContext and spin up a fresh "
        "one inside the same JSRuntime. Re-binds cairns.dispatch + "
        "re-evaluates studio_js. State survives because everything "
        "addressable is host-side (resident prefabs, entities, "
        "viewports). Returns {ok}.",
        [&host](const json&) -> json {
            return {{"ok", ReloadJsContext(host)}};
        });

    registry.Register(
        "cairns.script.eval",
        /*schema=*/json::object(),
        /*doc=*/"Eval a JS snippet inside the embedded QuickJS context. "
                "Use cairns.dispatch(op, args) to call any registered op.",
        [&host](const json& args) -> json {
            ScriptHost& s2 = host;
            const std::string code = args.value("code", std::string{});
            if (code.empty()) {
                throw std::runtime_error("missing 'code' arg");
            }
            JSValue v = JS_Eval(s2.ctx, code.c_str(), code.size(),
                                "<script.eval>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) {
                JSValue err = JS_GetException(s2.ctx);
                const char* msg = JS_ToCString(s2.ctx, err);
                json out = {{"error", msg ? msg : "?"}};
                if (msg) {
                    JS_FreeCString(s2.ctx, msg);
                }
                JS_FreeValue(s2.ctx, err);
                JS_FreeValue(s2.ctx, v);
                throw std::runtime_error(out["error"].get<std::string>());
            }
            // Drain microtasks so any then-callbacks resolve before we
            // stringify. Bounded so a misbehaving script can't wedge the
            // dispatch loop. NOTE: studio.js InstantiateAsync deliberately
            // returns its value directly (not wrapped in a Promise) -- the
            // caller's `await` still works (await on a non-Promise resolves
            // immediately) so the syntactic divergence stays, but QuickJS's
            // promise machinery doesn't leak shutdown state. See
            // docs/studio_notes.md "QuickJS async limitation."
            for (int i = 0; i < 1000; ++i) {
                JSContext* job_ctx = nullptr;
                const int r = JS_ExecutePendingJob(
                    JS_GetRuntime(s2.ctx), &job_ctx);
                if (r <= 0) {
                    break;
                }
            }
            // Stringify result via JSON.stringify for round-trip back into
            // our json type.
            JSValue global2 = JS_GetGlobalObject(s2.ctx);
            JSValue json_obj = JS_GetPropertyStr(s2.ctx, global2, "JSON");
            JSValue stringify = JS_GetPropertyStr(s2.ctx, json_obj, "stringify");
            JSValue js_str = JS_Call(s2.ctx, stringify, JS_UNDEFINED, 1, &v);
            json out = json::object();
            if (!JS_IsException(js_str) && !JS_IsUndefined(js_str)) {
                const char* s_cstr = JS_ToCString(s2.ctx, js_str);
                if (s_cstr) {
                    try {
                        out = {{"result", json::parse(s_cstr)}};
                    } catch (...) {
                        out = {{"result", std::string(s_cstr)}};
                    }
                    JS_FreeCString(s2.ctx, s_cstr);
                }
            }
            JS_FreeValue(s2.ctx, js_str);
            JS_FreeValue(s2.ctx, stringify);
            JS_FreeValue(s2.ctx, json_obj);
            JS_FreeValue(s2.ctx, global2);
            JS_FreeValue(s2.ctx, v);
            return out;
        });

    registry.RegisterAlias("script.eval", "cairns.script.eval");

    // General scenario REFLECTION: enumerate the bundled scenario scripts
    // (scripts/*.js) so a client can discover what's loadable without any
    // per-scenario C++. Returns {scenarios:[stem names], count}.
    registry.Register(
        "cairns.scenario.list",
        json::object(),
        "List the bundled scenario scripts (scripts/*.js) by name. "
        "Returns {scenarios:[names], count}. Pairs with cairns.scenario.load.",
        [](const json&) -> json {
            std::vector<std::string> names;
            const std::filesystem::path dir =
                std::filesystem::path(cairns::GetBasePathSafe()) / "scripts";
            std::error_code ec;
            if (std::filesystem::is_directory(dir, ec)) {
                for (const auto& e :
                     std::filesystem::directory_iterator(dir, ec)) {
                    if (e.path().extension() == ".js") {
                        names.push_back(e.path().stem().string());
                    }
                }
            }
            std::sort(names.begin(), names.end());
            json arr = json::array();
            for (const std::string& n : names) {
                arr.push_back(n);
            }
            return {{"scenarios", std::move(arr)},
                    {"count", static_cast<uint32_t>(names.size())}};
        });

    // General scenario LOAD: read scripts/<name>.js + eval it through the same
    // JS context (mirrors RunBootScript). Data-driven -- one loader for every
    // scenario, no LOAD_TEST_CASE specialization. Args: {name}.
    registry.Register(
        "cairns.scenario.load",
        json::object(),
        "Load + eval a bundled scenario by name (scripts/<name>.js). "
        "Args: {name}. Returns {ok, name}. Pairs with cairns.scenario.list.",
        [&registry](const json& args) -> json {
            const std::string name = args.value("name", std::string{});
            if (name.empty()) {
                throw std::runtime_error("cairns.scenario.load: missing 'name'");
            }
            std::filesystem::path path;
            const std::string rel = "scripts/" + name + ".js";
            if (!cairns::GetStaticResourceFilepath(rel, path)) {
                throw std::runtime_error("scenario not found: " + rel);
            }
            std::string code;
            if (!cairns::platform::ReadAsset(path, code) || code.empty()) {
                throw std::runtime_error("scenario read failed: " +
                                         path.string());
            }
            json req;
            req["op"] = "cairns.script.eval";
            req["args"] = json::object();
            req["args"]["code"] = code;
            const json resp = registry.Dispatch(req);
            if (resp.contains("error")) {
                throw std::runtime_error("scenario eval failed: " + resp.dump());
            }
            return {{"ok", true}, {"name", name}};
        });
}

}  // namespace cairns::control
