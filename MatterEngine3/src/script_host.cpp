#include "../include/script_host.h"
extern "C" {
#include "quickjs.h"
}
#include <cstring>

namespace script_host {

// Pulls the current exception into a BakeError (best-effort location).
static BakeError harvest_exception(JSContext* ctx) {
    BakeError e; e.ok = false;
    JSValue ex = JS_GetException(ctx);
    const char* msg = JS_ToCString(ctx, ex);
    e.message = msg ? msg : "unknown script error";
    if (msg) JS_FreeCString(ctx, msg);
    JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
    if (!JS_IsUndefined(stack)) {
        const char* s = JS_ToCString(ctx, stack);
        if (s) { e.source_location = s; JS_FreeCString(ctx, s); }
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, ex);
    return e;
}

BakeResult ScriptHost::bake_source(const std::string& source,
                                   const std::string& /*params_json*/,
                                   const BakeOptions& /*opts*/,
                                   const uint64_t* /*child_hashes*/,
                                   size_t /*child_count*/) {
    BakeResult r;
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);

    // Minimal Part base so `extends Part` resolves (replaced in Task 4).
    static const char* kBootstrap = "globalThis.Part = class Part { build(p){} };\n";
    JSValue b = JS_Eval(ctx, kBootstrap, strlen(kBootstrap), "<bootstrap>",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(b)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,b); goto done; }
    JS_FreeValue(ctx, b);

    {
        // Eval the user source as a module-less global script, then find the
        // class. For SP-2 we wrap: source defines a class; we instantiate the
        // LAST defined global class via a trampoline appended to the source.
        std::string wrapped = source +
            "\n;globalThis.__partClass = (typeof Empty!=='undefined')?Empty:undefined;";
        JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<part>",
                            JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,v); goto done; }
        JS_FreeValue(ctx, v);
        // (Class discovery + build(p) call generalized in Task 3.)
    }

done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return r;
}

} // namespace script_host
