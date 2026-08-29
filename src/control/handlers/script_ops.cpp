#include "control/handlers/script_ops.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "control/command_registry.hpp"
#include "control/handlers/studio_js.hpp"
#include "util/json.hpp"

#include <cstdio>

extern "C" {
#include "quickjs.h"
}

namespace cairns::control {

namespace {

// Per-context state: a pointer to the registry + the JSRuntime/JSContext
// owned by RegisterScriptOps's lambda capture (lives for the registry's
// lifetime via a unique_ptr stored as a static here).
struct JsState {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    CommandRegistry* registry = nullptr;
    ~JsState() {
        if (rt) {
            // Drain any pending microtasks before tearing down. Without
            // this, an async eval that left state in the job queue trips
            // a clean-shutdown assert inside JS_FreeRuntime (process exits
            // with SIGABRT after the last response is flushed).
            for (int i = 0; i < 10000; ++i) {
                JSContext* job_ctx = nullptr;
                const int r = JS_ExecutePendingJob(rt, &job_ctx);
                if (r <= 0) break;
            }
        }
        if (ctx) {
            JS_FreeContext(ctx);
        }
        if (rt) {
            JS_FreeRuntime(rt);
        }
    }
};

JsState& EnsureJs(CommandRegistry* reg) {
    static JsState s;
    if (!s.rt) {
        s.rt = JS_NewRuntime();
        s.ctx = JS_NewContext(s.rt);
        s.registry = reg;
    }
    return s;
}

// JS callable: cairns.dispatch(opName, argsObjectOrUndef) -> resultObject
// Dispatches through the C++ registry. argsObject is JSON-stringified to
// reuse the existing json::parse path; result is parsed back.
JSValue JsDispatch(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                   JSValueConst* argv) {
    JsState& s = EnsureJs(nullptr);  // already initialized.
    if (!s.registry) {
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
    const json resp = s.registry->Dispatch(req);
    const std::string resp_str = resp.dump();
    return JS_ParseJSON(ctx, resp_str.c_str(), resp_str.size(), "<resp>");
}

}  // namespace

void RegisterScriptOps(CommandRegistry& registry) {
    JsState& s = EnsureJs(&registry);

    // Expose JS API on a `cairns` global: cairns.dispatch(op, args).
    JSValue global = JS_GetGlobalObject(s.ctx);
    JSValue cairns_obj = JS_NewObject(s.ctx);
    JS_SetPropertyStr(s.ctx, cairns_obj, "dispatch",
                      JS_NewCFunction(s.ctx, &JsDispatch, "dispatch", 2));
    JS_SetPropertyStr(s.ctx, global, "cairns", cairns_obj);
    JS_FreeValue(s.ctx, global);

    // Autoload the studio.* surface. Any syntax error in the embedded JS
    // string fails LOUD here at engine init -- the failure is on stderr +
    // RegisterScriptOps still returns (so the registry comes up; just
    // without the studio classes). A working JS run is silent.
    {
        const char* src = kStudioJsSource;
        const size_t src_len = std::strlen(src);
        JSValue v = JS_Eval(s.ctx, src, src_len, "<studio.js>",
                            JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            JSValue err = JS_GetException(s.ctx);
            const char* msg = JS_ToCString(s.ctx, err);
            std::fprintf(stderr, "[Studio] autoload FAILED: %s\n",
                         msg ? msg : "?");
            if (msg) JS_FreeCString(s.ctx, msg);
            JS_FreeValue(s.ctx, err);
        }
        JS_FreeValue(s.ctx, v);
    }

    registry.Register(
        "cairns.script.eval",
        /*schema=*/json::object(),
        /*doc=*/"Eval a JS snippet inside the embedded QuickJS context. "
                "Use cairns.dispatch(op, args) to call any registered op.",
        [](const json& args) -> json {
            JsState& s2 = EnsureJs(nullptr);
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
}

}  // namespace cairns::control
