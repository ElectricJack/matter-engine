#include "../include/script_host.h"
extern "C" {
#include "quickjs.h"
}
#include "part_base.js.h"
#include "part_asset_v2.h"   // SP-1 v2 helper (compute_resolved_hash)
#include <cstring>
#include <regex>

namespace script_host {

// Extract the authored class name from `class <Name> extends Part`. Top-level
// `class` declarations in GLOBAL eval create LEXICAL bindings (not enumerable
// globalThis properties), so the host cannot discover them by scanning the
// global object. Instead the host appends a trampoline that assigns the named
// class to globalThis.__partClass; this is generic over the class name (no
// hardcoded "Empty"/"Rock") and deterministic.
static std::string find_part_class_name(const std::string& source) {
    std::smatch m;
    std::regex re("class\\s+([A-Za-z_$][A-Za-z0-9_$]*)\\s+extends\\s+Part\\b");
    if (std::regex_search(source, m, re)) return m[1].str();
    return std::string();
}

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

// Merge static params with caller overrides (overrides win), sort keys, and
// stringify to canonical JSON. Evals the class's `static params` but does NOT
// instantiate or call build(). Shared by bake_source and resolve_hash so both
// hash byte-identical params.
std::string ScriptHost::merge_params_canonical(const std::string& source,
                                               const std::string& params_json,
                                               BakeError& err) {
    last_merged_params_ = "{}";
    std::string className = find_part_class_name(source);
    if (className.empty()) {
        err.ok = false; err.message = "no class extending Part found";
        return last_merged_params_;
    }

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);

    JSValue base = JS_Eval(ctx, kPartBaseJS, strlen(kPartBaseJS), "<part-base>",
                           JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(base)) { err = harvest_exception(ctx); JS_FreeValue(ctx,base);
        JS_FreeContext(ctx); JS_FreeRuntime(rt); return last_merged_params_; }
    JS_FreeValue(ctx, base);

    std::string wrapped = source + "\n;globalThis.__partClass = " + className + ";\n";
    JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<part>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) { err = harvest_exception(ctx); JS_FreeValue(ctx,v);
        JS_FreeContext(ctx); JS_FreeRuntime(rt); return last_merged_params_; }
    JS_FreeValue(ctx, v);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue authored = JS_GetPropertyStr(ctx, global, "__partClass");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, authored)) {
        JS_FreeValue(ctx, authored);
        err.ok = false; err.message = "no class extending Part found";
        JS_FreeContext(ctx); JS_FreeRuntime(rt); return last_merged_params_;
    }

    JSValue staticParams = JS_GetPropertyStr(ctx, authored, "params");
    if (JS_IsUndefined(staticParams)) staticParams = JS_NewObject(ctx);
    JSValue overrides = JS_ParseJSON(ctx, params_json.c_str(), params_json.size(), "<params>");
    if (JS_IsException(overrides)) {
        err = harvest_exception(ctx);
        JS_FreeValue(ctx, staticParams); JS_FreeValue(ctx, overrides);
        JS_FreeValue(ctx, authored);
        JS_FreeContext(ctx); JS_FreeRuntime(rt); return last_merged_params_;
    }
    static const char* kMerge =
      "(function(d,o){let m=Object.assign({},d,o);"
      "let keys=Object.keys(m).sort();let r={};for(let k of keys)r[k]=m[k];"
      "return JSON.stringify(r);})";
    JSValue mergeFn = JS_Eval(ctx, kMerge, strlen(kMerge), "<merge>", JS_EVAL_TYPE_GLOBAL);
    JSValue args2[2] = { staticParams, overrides };
    JSValue mergedStr = JS_Call(ctx, mergeFn, JS_UNDEFINED, 2, args2);
    if (JS_IsException(mergedStr)) {
        err = harvest_exception(ctx);
    } else {
        const char* mjson = JS_ToCString(ctx, mergedStr);
        last_merged_params_ = mjson ? mjson : "{}";
        if (mjson) JS_FreeCString(ctx, mjson);
    }
    JS_FreeValue(ctx, mergedStr); JS_FreeValue(ctx, mergeFn);
    JS_FreeValue(ctx, overrides); JS_FreeValue(ctx, staticParams);
    JS_FreeValue(ctx, authored);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return last_merged_params_;
}

uint64_t ScriptHost::resolve_hash(const std::string& source,
                                  const std::string& params_json,
                                  const uint64_t* child_hashes,
                                  size_t child_count) {
    BakeError err;
    std::string canon = merge_params_canonical(source, params_json, err);
    if (!err.ok) return 0;   // fail-closed: caller treats 0 as resolve failure
    return part_asset::compute_resolved_hash(
        source.data(), source.size(),
        canon.data(), canon.size(),
        child_hashes, child_count);
}

BakeResult ScriptHost::bake_source(const std::string& source,
                                   const std::string& params_json,
                                   const BakeOptions& /*opts*/,
                                   const uint64_t* child_hashes,
                                   size_t child_count) {
    BakeResult r;

    // Merge static params + caller overrides into canonical JSON, and compute
    // the resolved hash over (source, merged params, child hashes). Shares the
    // exact path resolve_hash uses so both agree byte-for-byte.
    {
        BakeError merr;
        std::string canon = merge_params_canonical(source, params_json, merr);
        if (!merr.ok) { r.error = merr; return r; }
        r.resolved_hash = part_asset::compute_resolved_hash(
            source.data(), source.size(),
            canon.data(), canon.size(),
            child_hashes, child_count);
    }
    const std::string merged = last_merged_params_;

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);

    last_build_ran_ = false;
    JSValue base = JS_Eval(ctx, kPartBaseJS, strlen(kPartBaseJS), "<part-base>",
                           JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(base)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,base); goto done; }
    JS_FreeValue(ctx, base);

    {
        // Eval user source + a generic trampoline that publishes the authored
        // class (lexically declared) onto globalThis.__partClass.
        std::string className = find_part_class_name(source);
        if (className.empty()) {
            r.error.ok = false; r.error.message = "no class extending Part found";
            goto done;
        }
        std::string wrapped = source + "\n;globalThis.__partClass = " + className + ";\n";
        JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<part>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,v); goto done; }
        JS_FreeValue(ctx, v);

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue authored = JS_GetPropertyStr(ctx, global, "__partClass");
        JS_FreeValue(ctx, global);
        if (!JS_IsFunction(ctx, authored)) {
            JS_FreeValue(ctx, authored);
            r.error.ok = false; r.error.message = "no class extending Part found";
            goto done;
        }
        JSValue inst = JS_CallConstructor(ctx, authored, 0, nullptr);
        JS_FreeValue(ctx, authored);
        if (JS_IsException(inst)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,inst); goto done; }
        JSValue paramsObj = JS_ParseJSON(ctx, merged.c_str(), merged.size(), "<merged>");
        if (JS_IsException(paramsObj)) { JS_FreeValue(ctx, paramsObj); paramsObj = JS_NewObject(ctx); }
        JSAtom buildAtom = JS_NewAtom(ctx, "build");
        JSValue bret = JS_Invoke(ctx, inst, buildAtom, 1, &paramsObj);
        JS_FreeAtom(ctx, buildAtom);
        last_build_ran_ = !JS_IsException(bret);
        if (JS_IsException(bret)) r.error = harvest_exception(ctx);
        JS_FreeValue(ctx, bret);
        JS_FreeValue(ctx, paramsObj);
        JS_FreeValue(ctx, inst);
    }

done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return r;
}

} // namespace script_host
