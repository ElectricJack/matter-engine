#include "script_host.h"
extern "C" {
#include "quickjs.h"
}
#include "part_base.js.h"
#include "tileset_base.js.h"
#include "world_base.js.h"
#include "part_asset_v2.h"   // SP-1 v2 helper (compute_resolved_hash, save_v2)
#include "triangle_emit.hpp" // direct-triangle (mesh) session buffer
#include "dsl_state.h"
#include "dsl_bindings.h"
#include "csg_lowering.h"   // NEW MatterEngine3 header
#include "module_resolver.h" // SP-7 shared-lib fold + resolution
#include "modifier_apply.h"  // Task 4: bake-time modifier region stack apply
#include "mesh_indexed.hpp"  // Task 4: weld/unweld across cells for region path
#include "tileset_layout.h"     // tile_colors (Task 5: variant hook)
#include "tileset_placement.h"  // placement_seed (Task 5: variant rng)
#include "cluster.h"                    // consumed prototype (StaticParticle, Cluster)
#include "cell.h"                       // consumed prototype (Cell, build_cell_meshes GL-free)
#include "mesh_worker_pool.h"           // consumed prototype (CellMeshResult/GroupMeshResult)
#include "blas_manager.hpp"             // consumed prototype
#include "tlas_manager.hpp"             // consumed prototype
#include "surface.h"                    // consumed prototype (CreateSurfaceScratch; self-guards extern "C")
extern "C" {
#include "material_registry.h"          // MaterialMergeGroup (fat-prim bucket seeding)
}
#include <cstdio>
#include <cstdlib>   // std::getenv (MATTER_BAKE_PROFILE)
#include <cstring>
#include <cmath>
#include <chrono>
#include <map>
#include <memory>
#include <new>       // std::bad_alloc
#include <regex>
#include <utility>   // std::pair

namespace script_host {

// Shared sentinel: the exact message merge_params_canonical sets when the source
// has no class extending Part (the tileset case — Tileset extends Part indirectly).
// Both merge_params_canonical's return sites and eval_tileset's comparison use this
// constant so a rename stays consistent (I1 fix).
static constexpr const char* kNoPartClassMsg = "no class extending Part found";

// ---------------------------------------------------------------------------
// SP-7 shared-lib module loading.
//
// When a shared-lib root is configured, a part's `import { x } from
// 'shared-lib/y'` must resolve at bake/eval time. The module_resolver gathers
// every transitively-imported module's SOURCE up front (no QuickJS); we hand
// that {canonical specifier -> source} set to QuickJS-ng via JS_SetModuleLoaderFunc.
// The loader serves source ONLY from this in-memory set (never the filesystem at
// eval time) to preserve determinism and the no-file-access contract. The part
// itself is evaluated as an ES module so its `import` statements bind.
// ---------------------------------------------------------------------------
struct ModuleStore {
    // canonical specifier (trailing ".js" stripped) -> module source.
    std::map<std::string, std::string> sources;
};

// Canonicalize a specifier the same way module_resolver does: a bare
// "shared-lib/x" stays as-is; a trailing ".js" is stripped so "shared-lib/x" and
// "shared-lib/x.js" name the same module. The part's own pseudo-name is passed
// through untouched.
static std::string canon_specifier(const std::string& s) {
    std::string r = s;
    if (r.size() >= 3 && r.compare(r.size() - 3, 3, ".js") == 0) r.resize(r.size() - 3);
    return r;
}

// QuickJS module normalizer: returns the canonical specifier (js_malloc'd).
static char* sh_module_normalize(JSContext* ctx, const char* /*base*/,
                                 const char* name, void* /*opaque*/) {
    std::string c = canon_specifier(name ? name : "");
    char* out = static_cast<char*>(js_malloc(ctx, c.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, c.c_str(), c.size() + 1);
    return out;
}

// QuickJS module loader: compile the in-memory source for `module_name` (already
// normalized) into a JSModuleDef. Fails (throws) if the specifier is not in the
// resolved set — the resolver pre-gathered everything reachable, so a miss here
// means a non-shared-lib or unresolved import, which must fail-closed.
static JSModuleDef* sh_module_loader(JSContext* ctx, const char* module_name,
                                     void* opaque) {
    ModuleStore* store = static_cast<ModuleStore*>(opaque);
    auto it = store ? store->sources.find(canon_specifier(module_name))
                    : std::map<std::string, std::string>::iterator();
    if (!store || it == store->sources.end()) {
        JS_ThrowReferenceError(ctx, "module not in resolved shared-lib set: %s",
                               module_name);
        return nullptr;
    }
    const std::string& src = it->second;
    // Compile-only: produce a module def (tag JS_TAG_MODULE) without running it;
    // QuickJS links + evaluates it as part of the importing module's evaluation.
    JSValue func = JS_Eval(ctx, src.c_str(), src.size(), module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) return nullptr;
    JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func));
    JS_FreeValue(ctx, func);
    return m;
}

// Drain the runtime's pending job queue (module top-level evaluation of synchronous
// ES modules is scheduled as a job in QuickJS-ng). Returns false if any job threw.
static bool drain_jobs(JSRuntime* rt, JSContext** pctx) {
    for (;;) {
        int r = JS_ExecutePendingJob(rt, pctx);
        if (r == 0) return true;     // no more jobs
        if (r < 0) return false;     // a job threw (exception is on *pctx)
    }
}

// Eval `source` as an ES MODULE (so its `import`s resolve via the installed
// loader) and run it to completion. On a synchronous module the returned promise
// resolves after draining the job queue. Returns the eval result value (caller
// frees) or JS_EXCEPTION on failure; sets *threw on error.
static JSValue eval_part_as_module(JSContext* ctx, JSRuntime* rt,
                                   const std::string& wrapped, bool* threw) {
    *threw = false;
    JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<part>",
                        JS_EVAL_TYPE_MODULE);
    if (JS_IsException(v)) { *threw = true; return v; }
    // Drain top-level module jobs (synchronous module bodies run here).
    JSContext* jctx = ctx;
    if (!drain_jobs(rt, &jctx)) { JS_FreeValue(ctx, v); *threw = true; return JS_EXCEPTION; }
    // A module eval yields a promise; surface a rejection as a throw.
    if (JS_IsObject(v)) {
        JSPromiseStateEnum st = JS_PromiseState(ctx, v);
        if (st == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, v);
            JS_Throw(ctx, reason);   // re-arm the exception for harvest_exception
            JS_FreeValue(ctx, v);
            *threw = true;
            return JS_EXCEPTION;
        }
    }
    return v;
}

// Per-bake interrupt context: a wall-clock deadline the QuickJS-ng VM polls via
// the interrupt handler. In install-mode (time_budget_ms == 0) the bake is
// unbounded; in dev-mode a runaway build() (e.g. `while(true){}`) is aborted once
// the deadline passes so the bake fails-closed instead of hanging the host.
struct InterruptCtx {
    std::chrono::steady_clock::time_point deadline;
    bool bounded = false;
};
static int interrupt_cb(JSRuntime*, void* opaque) {
    InterruptCtx* ic = static_cast<InterruptCtx*>(opaque);
    if (!ic || !ic->bounded) return 0;
    return std::chrono::steady_clock::now() >= ic->deadline ? 1 : 0; // 1 => interrupt
}

// Build a bake JSContext from a RAW context (no default intrinsics) and add ONLY
// the deterministic subset the authoring DSL needs. Notably NO Date intrinsic, so
// `typeof Date === "undefined"` inside a bake — there is no wall-clock source, and
// (together with the seeded Math.random + the absence of any require/fetch/os
// bindings) the bake is process-entropy-free. This is what keeps the resolved-hash
// <-> serialized-bytes contract intact.
static JSContext* new_bake_context(JSRuntime* rt, bool want_modules = false) {
    JSContext* ctx = JS_NewContextRaw(rt);
    if (!ctx) return nullptr;
    JS_AddIntrinsicBaseObjects(ctx);   // Object/Array/Math/String/Number/etc.
    JS_AddIntrinsicEval(ctx);          // required: host evals the class source
    JS_AddIntrinsicRegExpCompiler(ctx);
    JS_AddIntrinsicRegExp(ctx);
    JS_AddIntrinsicJSON(ctx);          // JS_ParseJSON / params merge
    JS_AddIntrinsicMapSet(ctx);
    JS_AddIntrinsicTypedArrays(ctx);
    JS_AddIntrinsicBigInt(ctx);
    // ES module evaluation drives an internal evaluation Promise; the Promise
    // intrinsic must be present or the module's eval-promise machinery leaks GC
    // objects. Added ONLY for the module path so the classic-script bake context
    // is byte-for-byte unchanged for SP-2/SP-3. Promise is pure/deterministic
    // (no wall-clock, no I/O), so it does not weaken the entropy-free contract.
    if (want_modules) JS_AddIntrinsicPromise(ctx);
    // Intentionally omitted: JS_AddIntrinsicDate (ambient wall-clock), and we
    // never bind require/fetch/os, so authored code has no entropy source.
    return ctx;
}

// Derive a deterministic 64-bit seed from the merged canonical params JSON. If the
// params contain a numeric "seed" field, honor it (so authors can pick a seed);
// otherwise fold the whole canonical JSON via FNV-1a so distinct params still draw
// distinct random streams. Either way the seed depends ONLY on the inputs.
static uint64_t derive_seed(const std::string& merged_json) {
    // Cheap, dependency-free scan for a top-level "seed": <number>. The merged
    // JSON is canonical (sorted keys, no whitespace), so the literal needle holds.
    const std::string needle = "\"seed\":";
    size_t pos = merged_json.find(needle);
    if (pos != std::string::npos) {
        size_t i = pos + needle.size();
        // Parse an integer (optionally negative) seed value.
        bool neg = (i < merged_json.size() && merged_json[i] == '-');
        if (neg) ++i;
        bool any = false; unsigned long long v = 0;
        for (; i < merged_json.size() && merged_json[i] >= '0' && merged_json[i] <= '9'; ++i) {
            v = v * 10ull + (unsigned)(merged_json[i] - '0'); any = true;
        }
        if (any) {
            uint64_t s = (uint64_t)v;
            if (neg) s = (uint64_t)(-(int64_t)v);
            // Mix the chosen seed with the full params so two parts that both pick
            // seed:1 but differ elsewhere still diverge.
            return s ^ part_asset::fnv1a64(merged_json.data(), merged_json.size());
        }
    }
    return part_asset::fnv1a64(merged_json.data(), merged_json.size());
}

// Extract the authored class name from `class <Name> extends Part`. Top-level
// `class` declarations in GLOBAL eval create LEXICAL bindings (not enumerable
// globalThis properties), so the host cannot discover them by scanning the
// global object. Instead the host appends a trampoline that assigns the named
// class to globalThis.__partClass; this is generic over the class name (no
// hardcoded "Empty"/"Rock") and deterministic.
static std::string find_part_class_name(const std::string& source) {
    // Perf fix: compile the regex once (static const) instead of once per call.
    static const std::regex re("class\\s+([A-Za-z_$][A-Za-z0-9_$]*)\\s+extends\\s+Part\\b");
    std::smatch m;
    if (std::regex_search(source, m, re)) return m[1].str();
    return std::string();
}

// Extract the authored class name from `class <Name> extends Tileset`.
static std::string find_tileset_class_name(const std::string& source) {
    // Perf fix: compile the regex once (static const) instead of once per call.
    static const std::regex re("class\\s+([A-Za-z_$][A-Za-z0-9_$]*)\\s+extends\\s+Tileset\\b");
    std::smatch m;
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

// Eval kPartBaseJS, then the part source + trampoline, leaving the authored class
// on globalThis.__partClass. When `store` is non-null the part is evaluated as an
// ES MODULE with the shared-lib loader installed (so its `import`s bind); when
// null it is evaluated as a classic GLOBAL script (legacy, importer-free parts).
// Either way the surrounding intrinsics are the restricted bake set (no Date/
// require/fetch/os). On failure returns false with `err` populated and the rt/ctx
// already freed; on success the caller owns rt/ctx (eval result already freed).
static bool eval_part_publish_class(const std::string& source,
                                    const std::string& className,
                                    ModuleStore* store,
                                    JSRuntime*& rt, JSContext*& ctx,
                                    BakeError& err) {
    rt = JS_NewRuntime();
    if (store) JS_SetModuleLoaderFunc(rt, sh_module_normalize, sh_module_loader, store);
    ctx = new_bake_context(rt, /*want_modules*/ store != nullptr);

    JSValue base = JS_Eval(ctx, kPartBaseJS, strlen(kPartBaseJS), "<part-base>",
                           JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(base)) { err = harvest_exception(ctx); JS_FreeValue(ctx, base);
        JS_FreeContext(ctx); JS_FreeRuntime(rt); rt = nullptr; ctx = nullptr; return false; }
    JS_FreeValue(ctx, base);

    std::string wrapped = source + "\n;globalThis.__partClass = " + className + ";\n";
    if (store) {
        bool threw = false;
        JSValue v = eval_part_as_module(ctx, rt, wrapped, &threw);
        if (threw) { err = harvest_exception(ctx); JS_FreeValue(ctx, v);
            JS_FreeContext(ctx); JS_FreeRuntime(rt); rt = nullptr; ctx = nullptr; return false; }
        JS_FreeValue(ctx, v);
    } else {
        JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<part>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) { err = harvest_exception(ctx); JS_FreeValue(ctx, v);
            JS_FreeContext(ctx); JS_FreeRuntime(rt); rt = nullptr; ctx = nullptr; return false; }
        JS_FreeValue(ctx, v);
    }
    return true;
}

// Build the in-memory module store from a fold result (used to drive the loader).
static ModuleStore store_from_fold(const module_resolver::FoldResult& fr) {
    ModuleStore s;
    for (const auto& m : fr.modules) s.sources[m.specifier] = m.source;
    return s;
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
        err.ok = false; err.message = kNoPartClassMsg;
        return last_merged_params_;
    }

    // If a shared-lib root is set, fold to gather the importable module sources so
    // the part can be evaluated as a module (its `static params`/class body may
    // reference imported values). Fail-closed on a fold error.
    ModuleStore store;
    bool use_module = false;
    if (!shared_lib_roots_.empty()) {
        module_resolver::FoldResult fr;
        std::string ferr;
        if (!fold_sources_cached(source, fr, ferr)) {
            err.ok = false;
            err.message = "module resolution failed: " + ferr;
            return last_merged_params_;
        }
        if (!fr.modules.empty()) { store = store_from_fold(fr); use_module = true; }
    }

    JSRuntime* rt = nullptr; JSContext* ctx = nullptr;
    if (!eval_part_publish_class(source, className, use_module ? &store : nullptr,
                                 rt, ctx, err))
        return last_merged_params_;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue authored = JS_GetPropertyStr(ctx, global, "__partClass");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, authored)) {
        JS_FreeValue(ctx, authored);
        err.ok = false; err.message = kNoPartClassMsg;
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

// Static discovery of a part's required children WITHOUT baking. Evals the
// class top-level in a fresh isolated bake context (same restricted intrinsics,
// no Date/require/fetch/os), reads `static requires`, evaluates it against the
// merged static+override params, and returns one RequiredChild per declared
// { module, params } entry with canonical params JSON. Fail-closed: any error
// (no requires, throw, malformed entry) yields an empty list. Never runs build().
std::vector<RequiredChild> ScriptHost::eval_requires(const std::string& source,
                                                     const std::string& params_json) {
    std::vector<RequiredChild> out;

    // Reuse the shared params-merge path so the params handed to `requires` are
    // the same canonical merged params build()/resolve_hash see.
    // Tileset classes extend Tileset (not Part directly), so merge_params_canonical
    // returns kNoPartClassMsg. In that expected case, retain the caller's canonical
    // authored root params so functional static requires(params) sees the same value
    // later passed to eval_tileset/build. Only fail closed on other merge errors.
    BakeError merr;
    std::string merged = merge_params_canonical(source, params_json, merr);
    if (!merr.ok && merr.message != kNoPartClassMsg) return out;
    if (!merr.ok)
        merged = params_json.empty() ? "{}" : params_json;

    // Try Part class first; fall back to Tileset class name for tileset roots.
    bool is_tileset = false;
    std::string className = find_part_class_name(source);
    if (className.empty()) {
        className = find_tileset_class_name(source);
        is_tileset = !className.empty();
        if (className.empty()) return out;
    }

    // Eval as a module when the part imports shared-lib code (so its class body /
    // `static requires` can reference imported values); fail-closed on a fold
    // error by returning an empty list.
    ModuleStore store;
    bool use_module = false;
    if (!shared_lib_roots_.empty()) {
        module_resolver::FoldResult fr;
        std::string ferr;
        if (!fold_sources_cached(source, fr, ferr)) return out;
        if (!fr.modules.empty()) { store = store_from_fold(fr); use_module = true; }
    }

    JSRuntime* rt = nullptr; JSContext* ctx = nullptr;
    BakeError eerr;
    if (is_tileset) {
        // Tileset sources need both kPartBaseJS and kTilesetBaseJS injected so
        // that `class X extends Tileset` evaluates without a reference error.
        rt = JS_NewRuntime();
        if (use_module && !store.sources.empty())
            JS_SetModuleLoaderFunc(rt, sh_module_normalize, sh_module_loader, &store);
        ctx = new_bake_context(rt, use_module);
        JSValue base = JS_Eval(ctx, kPartBaseJS, strlen(kPartBaseJS), "<part-base>",
                               JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(base)) {
            JS_FreeValue(ctx, base); JS_FreeContext(ctx); JS_FreeRuntime(rt); return out;
        }
        JS_FreeValue(ctx, base);
        JSValue tsbase = JS_Eval(ctx, kTilesetBaseJS, strlen(kTilesetBaseJS),
                                 "<tileset-base>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(tsbase)) {
            JS_FreeValue(ctx, tsbase); JS_FreeContext(ctx); JS_FreeRuntime(rt); return out;
        }
        JS_FreeValue(ctx, tsbase);
        std::string wrapped = source + "\n;globalThis.__partClass = " + className + ";\n";
        if (use_module) {
            bool threw = false;
            JSValue v = eval_part_as_module(ctx, rt, wrapped, &threw);
            if (threw) {
                JS_FreeValue(ctx, v); JS_FreeContext(ctx); JS_FreeRuntime(rt); return out;
            }
            JS_FreeValue(ctx, v);
        } else {
            JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<tileset>",
                                JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) {
                JS_FreeValue(ctx, v); JS_FreeContext(ctx); JS_FreeRuntime(rt); return out;
            }
            JS_FreeValue(ctx, v);
        }
    } else {
        if (!eval_part_publish_class(source, className, use_module ? &store : nullptr,
                                     rt, ctx, eerr))
            return out;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue authored = JS_GetPropertyStr(ctx, global, "__partClass");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, authored)) {
        JS_FreeValue(ctx, authored);
        JS_FreeContext(ctx); JS_FreeRuntime(rt); return out;
    }

    JSValue requiresProp = JS_GetPropertyStr(ctx, authored, "requires");
    // No `static requires` => no children (not an error; leaf parts are common).
    if (JS_IsUndefined(requiresProp) || JS_IsNull(requiresProp)) {
        JS_FreeValue(ctx, requiresProp);
        JS_FreeValue(ctx, authored);
        JS_FreeContext(ctx); JS_FreeRuntime(rt); return out;
    }

    // Parse the merged params back into an object to pass to requires(params).
    JSValue paramsObj = JS_ParseJSON(ctx, merged.c_str(), merged.size(), "<merged>");
    if (JS_IsException(paramsObj)) { JS_FreeValue(ctx, paramsObj); paramsObj = JS_NewObject(ctx); }

    // `static requires` may be a method (call it with params) or a plain array.
    JSValue list;
    if (JS_IsFunction(ctx, requiresProp)) {
        list = JS_Call(ctx, requiresProp, authored, 1, &paramsObj);
    } else {
        list = JS_DupValue(ctx, requiresProp);
    }
    JS_FreeValue(ctx, paramsObj);
    JS_FreeValue(ctx, requiresProp);

    if (JS_IsException(list) || !JS_IsArray(list)) {
        // A thrown requires() or a non-array result is fail-closed: empty.
        JS_FreeValue(ctx, list);
        JS_FreeValue(ctx, authored);
        JS_FreeContext(ctx); JS_FreeRuntime(rt); return out;
    }

    // Canonicalize each child's params the same way merge_params_canonical does
    // (sorted keys, no whitespace) so SP-3's memo identity stays stable.
    static const char* kCanon =
      "(function(o){if(o===undefined||o===null)return '{}';"
      "let keys=Object.keys(o).sort();let r={};for(let k of keys)r[k]=o[k];"
      "return JSON.stringify(r);})";
    JSValue canonFn = JS_Eval(ctx, kCanon, strlen(kCanon), "<canon>", JS_EVAL_TYPE_GLOBAL);

    uint32_t len = 0;
    {
        JSValue lenV = JS_GetPropertyStr(ctx, list, "length");
        JS_ToUint32(ctx, &len, lenV);
        JS_FreeValue(ctx, lenV);
    }
    bool ok = true;
    for (uint32_t i = 0; i < len && ok; ++i) {
        JSValue entry = JS_GetPropertyUint32(ctx, list, i);
        JSValue modV = JS_GetPropertyStr(ctx, entry, "module");
        JSValue parV = JS_GetPropertyStr(ctx, entry, "params");

        RequiredChild rc;
        const char* ms = JS_ToCString(ctx, modV);
        if (ms) { rc.module_specifier = ms; JS_FreeCString(ctx, ms); }
        else    { ok = false; }   // a child must name a module

        JSValue canonStr = JS_Call(ctx, canonFn, JS_UNDEFINED, 1, &parV);
        if (JS_IsException(canonStr)) { ok = false; }
        else {
            const char* cs = JS_ToCString(ctx, canonStr);
            rc.params_json = cs ? cs : "{}";
            if (cs) JS_FreeCString(ctx, cs);
        }
        JS_FreeValue(ctx, canonStr);
        JS_FreeValue(ctx, parV);
        JS_FreeValue(ctx, modV);
        JS_FreeValue(ctx, entry);

        if (ok) out.push_back(std::move(rc));
    }

    JS_FreeValue(ctx, canonFn);
    JS_FreeValue(ctx, list);
    JS_FreeValue(ctx, authored);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    // Fail-closed: a malformed entry invalidates the whole list (SP-3 hard-errors).
    if (!ok) out.clear();
    return out;
}

// Static discovery of a part's LOD budget statics WITHOUT baking. Evals the
// class in a fresh isolated bake context (same restricted intrinsics as
// eval_requires), reads `static lodBudgets` (array of numbers in (0,1]) and
// `static lodAnchorSize` (positive number), and returns them. Fail-closed:
// any error => empty LodBudgetSpec (schema treated as not opted in).
ScriptHost::LodBudgetSpec ScriptHost::eval_lod_budgets(const std::string& source) {
    LodBudgetSpec out;

    std::string className = find_part_class_name(source);
    if (className.empty()) return out;

    ModuleStore store;
    bool use_module = false;
    if (!shared_lib_roots_.empty()) {
        module_resolver::FoldResult fr;
        std::string ferr;
        if (!fold_sources_cached(source, fr, ferr)) return out;
        if (!fr.modules.empty()) { store = store_from_fold(fr); use_module = true; }
    }

    JSRuntime* rt = nullptr; JSContext* ctx = nullptr;
    BakeError eerr;
    if (!eval_part_publish_class(source, className, use_module ? &store : nullptr,
                                 rt, ctx, eerr))
        return out;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue authored = JS_GetPropertyStr(ctx, global, "__partClass");
    JS_FreeValue(ctx, global);
    if (JS_IsFunction(ctx, authored)) {
        JSValue budgets = JS_GetPropertyStr(ctx, authored, "lodBudgets");
        if (JS_IsArray(budgets)) {
            JSValue lenv = JS_GetPropertyStr(ctx, budgets, "length");
            uint32_t len = 0; JS_ToUint32(ctx, &len, lenv); JS_FreeValue(ctx, lenv);
            for (uint32_t i = 0; i < len; ++i) {
                JSValue el = JS_GetPropertyUint32(ctx, budgets, i);
                double d = 0.0;
                bool ok = !JS_IsException(el) && JS_IsNumber(el) &&
                          JS_ToFloat64(ctx, &d, el) == 0 && d > 0.0 && d <= 1.0;
                JS_FreeValue(ctx, el);
                if (!ok) { out.budgets.clear(); break; }  // fail closed
                out.budgets.push_back(d);
            }
        }
        JS_FreeValue(ctx, budgets);
        JSValue anchor = JS_GetPropertyStr(ctx, authored, "lodAnchorSize");
        if (JS_IsNumber(anchor)) {
            double a = 0.0;
            if (JS_ToFloat64(ctx, &a, anchor) == 0 && a > 0.0) out.anchor_size = a;
        }
        JS_FreeValue(ctx, anchor);
    }
    JS_FreeValue(ctx, authored);
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    return out;
}

uint64_t ScriptHost::resolve_hash(const std::string& source,
                                  const std::string& params_json,
                                  const uint64_t* child_hashes,
                                  size_t child_count) {
    BakeError err;
    std::string canon = merge_params_canonical(source, params_json, err);
    if (!err.ok) return 0;   // fail-closed: caller treats 0 as resolve failure

    // Fold the part source + its transitively-imported shared-lib module sources
    // into the canonical buffer that becomes source_bytes. If no shared-lib root
    // is configured, hash the raw source (legacy). Fail-closed: a fold error
    // (missing/illegal module) makes resolve fail (0). The folded buffer here is
    // byte-identical to the one bake_source hashes, so the two ALWAYS agree.
    module_resolver::FoldResult fr;
    const char* src_bytes = source.data();
    size_t      src_len   = source.size();
    if (!shared_lib_roots_.empty()) {
        std::string ferr;
        if (!module_resolver::fold_sources(source, shared_lib_roots_, fr, ferr))
            return 0;   // fail-closed
        src_bytes = fr.folded.data();
        src_len   = fr.folded.size();
    }
    return part_asset::compute_resolved_hash(
        src_bytes, src_len,
        canon.data(), canon.size(),
        child_hashes, child_count);
}

// Guarantee one TriEx per triangle after a modifier stack. Retopo output has
// no TriEx; rebuild with face normals and the group's material/tint (uniform
// within a material-merge-group, so proto = the group's first input TriEx).
static void ensure_triex(MeshIndexed& m, const TriEx& proto) {
    const size_t ntris = m.indices.size() / 3;
    if (m.triex.size() == ntris) return;
    m.triex.assign(ntris, TriEx{});
    for (size_t t = 0; t < ntris; ++t) {
        const float3 p0 = m.positions[m.indices[3*t]];
        const float3 p1 = m.positions[m.indices[3*t + 1]];
        const float3 p2 = m.positions[m.indices[3*t + 2]];
        const float ex1 = p1.x-p0.x, ey1 = p1.y-p0.y, ez1 = p1.z-p0.z;
        const float ex2 = p2.x-p0.x, ey2 = p2.y-p0.y, ez2 = p2.z-p0.z;
        float nx = ey1*ez2 - ez1*ey2, ny = ez1*ex2 - ex1*ez2, nz = ex1*ey2 - ey1*ex2;
        const float len = std::sqrt(nx*nx + ny*ny + nz*nz);
        if (len > 1e-20f) { nx /= len; ny /= len; nz /= len; }
        else              { nx = 0; ny = 0; nz = 1; }
        TriEx& e = m.triex[t];
        e.materialId = proto.materialId;
        e.tint = proto.tint;
        e.N0 = e.N1 = e.N2 = make_float3(nx, ny, nz);
    }
}

// Lower + cell-mesh one BuildBuffer and register its triangles.
//   stack == nullptr: existing per-cell path, byte-identical to before —
//     each cell/group registers its own BLAS entry.
//   stack != nullptr: region path — per material-merge-group, repacked
//     Tri/TriEx are ACCUMULATED across cells, welded into one indexed mesh
//     (cross-cell seams become interior edges), run through the modifier
//     stack, and registered as ONE BLAS entry.
static void mesh_sdf_ops(const dsl::BuildBuffer& buf,
                         const std::vector<dsl::ModifierSpec>* stack,
                         const std::string& label,
                         BLASManager& blas,
                         TLASManager& tlas) {
    dsl::LoweredField f = dsl::lower_build_buffer(buf);
    const float cell_size = 1.0f;   // smallest_cell_size (matches Cluster default)
    const float base_detail = buf.ops.empty()
                                  ? 0.1f : buf.ops[0].spacing;

    // group_id -> accumulated (Tri, TriEx) across all cells. std::map for
    // deterministic group iteration order.
    std::map<uint32_t, std::pair<std::vector<Tri>, std::vector<TriEx>>> region_acc;

    // Build the additive particle vector once (Cell reads it by index).
    const std::vector<StaticParticle>& particles = f.additive;

    // Ordered-CSG: a global FieldStages carrying just the stage-op list. The
    // per-cell FieldStages (with the cell-local particle->stage map) is built
    // inside Cell::build_group_mesh; here we only hand over the op order and
    // the additive->stage map. Staging engages only when there is >1 stage or
    // any fat primitive, so the common single-union part stays byte-identical.
    FieldStages gstages{};
    gstages.stageOp = f.stages.empty() ? nullptr : f.stages.data();
    gstages.stageCount = (int)f.stages.size();
    const FieldStages* gstagesPtr = (f.stages.size() > 1 || !f.fat.empty())
                                        ? &gstages : nullptr;
    const FatPrim* fatPtr = f.fat.empty() ? nullptr : f.fat.data();
    int fatCount = (int)f.fat.size();
    const int* clusterStage = f.additive_stage.empty() ? nullptr : f.additive_stage.data();

    // Determine the set of integer cell coordinates touched by any additive
    // particle, using the prototype's influence_radius = radius * 2 halo.
    // Perf fix: instead of O(cells × particles) assignment, invert the loop —
    // for each particle touch its cell range and push the particle index into
    // every cell where intersects_sphere(pos, radius) is true. This is
    // O(particles × avg_cells_per_particle) which is always ≤ the old cost.
    std::map<std::tuple<int,int,int>, std::unique_ptr<Cell>> cells;
    auto cell_key = [](int x,int y,int z){ return std::make_tuple(x,y,z); };

    // Additive particles: create cells AND assign particle indices in one pass.
    for (uint32_t i = 0; i < particles.size(); ++i) {
        const StaticParticle& sp = particles[i];
        float inf = sp.radius * 2.0f;
        int x0 = (int)std::floor((sp.position.x - inf) / cell_size);
        int x1 = (int)std::floor((sp.position.x + inf) / cell_size);
        int y0 = (int)std::floor((sp.position.y - inf) / cell_size);
        int y1 = (int)std::floor((sp.position.y + inf) / cell_size);
        int z0 = (int)std::floor((sp.position.z - inf) / cell_size);
        int z1 = (int)std::floor((sp.position.z + inf) / cell_size);
        for (int x=x0;x<=x1;++x) for (int y=y0;y<=y1;++y) for (int z=z0;z<=z1;++z) {
            auto k = cell_key(x,y,z);
            auto& cp = cells[k];
            if (!cp) cp = std::make_unique<Cell>(Vector3{(float)x,(float)y,(float)z},
                                                 0, cell_size);
            // Use unchecked variant: each (i, cell) pair is visited at most once.
            if (cp->intersects_sphere(sp.position, sp.radius))
                cp->add_particle_index_unchecked(i, sp.materialId);
        }
    }
    // Fat primitives (oriented box / ellipsoid) expand the cell set too, so a
    // pure-box part still gets cells to mesh its surface. ONLY Union-stage
    // prims can create surface: Difference/Intersection stages fold as
    // max(field, ±d), which never turns empty space solid, so a subtractive
    // prim's bounds must not spawn cells (a huge carve box — e.g. Rock facet
    // cuts — would otherwise mesh thousands of provably-empty cells).
    for (const FatPrim& fp : f.fat) {
        if (f.stages[fp.stage] != CSG_STAGE_UNION) continue;
        float inf = fp.boundRadius * 2.0f;
        int x0 = (int)std::floor((fp.center.x - inf) / cell_size);
        int x1 = (int)std::floor((fp.center.x + inf) / cell_size);
        int y0 = (int)std::floor((fp.center.y - inf) / cell_size);
        int y1 = (int)std::floor((fp.center.y + inf) / cell_size);
        int z0 = (int)std::floor((fp.center.z - inf) / cell_size);
        int z1 = (int)std::floor((fp.center.z + inf) / cell_size);
        for (int x=x0;x<=x1;++x) for (int y=y0;y<=y1;++y) for (int z=z0;z<=z1;++z) {
            auto k = cell_key(x,y,z);
            if (!cells[k])
                cells[k] = std::make_unique<Cell>(Vector3{(float)x,(float)y,(float)z},
                                                  0, cell_size);
        }
    }

    // Perf fix: build per-cell carve lists in one O(carve × avg_cells) pass
    // instead of O(cells × carve) inside the per-cell loop below.
    std::map<std::tuple<int,int,int>, std::vector<Particle>> cell_carve;
    for (const Particle& cp : f.carve) {
        float inf = cp.radius * 2.0f;   // generous halo to ensure no cell missed
        int x0 = (int)std::floor((cp.position.x - inf) / cell_size);
        int x1 = (int)std::floor((cp.position.x + inf) / cell_size);
        int y0 = (int)std::floor((cp.position.y - inf) / cell_size);
        int y1 = (int)std::floor((cp.position.y + inf) / cell_size);
        int z0 = (int)std::floor((cp.position.z - inf) / cell_size);
        int z1 = (int)std::floor((cp.position.z + inf) / cell_size);
        for (int x=x0;x<=x1;++x) for (int y=y0;y<=y1;++y) for (int z=z0;z<=z1;++z) {
            auto k = cell_key(x,y,z);
            if (cells.count(k)) {
                // Mirrors the original 1.5x slack test from the per-cell loop.
                if (cells[k]->intersects_sphere(cp.position, cp.radius * 1.5f))
                    cell_carve[k].push_back(cp);
            }
        }
    }

    // One scratch shared across all cells. The consumed mesher's marching-cubes
    // pass leaves the per-vertex normal buffer (scratch->pool.normals) UNWRITTEN;
    // compute_surface_normals_impl then reads the incoming (uninitialized) normal
    // for any degenerate vertex (vertex on a particle center / no neighbor in the
    // gradient search) before normalizing it. A fresh scratch per cell hands each
    // cell a freshly realloc'd (garbage) buffer, so those degenerate reads vary
    // run-to-run and the saved .part normals are nondeterministic. Sharing one
    // scratch keeps the pool buffer stable across the (deterministic) cell
    // sequence; we also clear it to a fixed pattern up front so the very first
    // cell's degenerate reads are deterministic too. surface.c/cell.cpp are
    // read-only, so this is the only lever available to make the bake byte-stable.
    SurfaceScratch* scratch = CreateSurfaceScratch();
    for (auto& kv : cells) {
        Cell* cell = kv.second.get();
        // Particle indices were already assigned during the inverted pass above;
        // no per-cell O(particles) scan needed here.
        // Seed an (empty) merge-group bucket for every fat primitive overlapping
        // this cell so build_cell_meshes iterates the group and meshes the box/
        // ellipsoid even when no additive sphere shares the cell. The fat field
        // eval pulls its own surface; the bucket just makes the group visible.
        for (const FatPrim& fp : f.fat) {
            // Same Union-only rule as the cell-creation pass above: a carved
            // surface belongs to the additive material's group, so subtractive
            // prims never need to seed a bucket.
            if (f.stages[fp.stage] != CSG_STAGE_UNION) continue;
            if (cell->intersects_sphere(fp.center, fp.boundRadius * 1.5f)) {
                uint32_t g = (uint32_t)MaterialMergeGroup(fp.materialId);
                cell->material_particle_indices[g]; // default-inserts empty bucket
            }
        }
        if (cell->material_particle_indices.empty()) continue;

        // Use the pre-built carve list for this cell (empty if no carve overlap).
        static const std::vector<Particle> kEmptyCarve;
        auto carve_it = cell_carve.find(kv.first);
        const std::vector<Particle>& carve =
            (carve_it != cell_carve.end()) ? carve_it->second : kEmptyCarve;
        const Particle* carvePtr = carve.empty() ? nullptr : carve.data();
        int carveCount = (int)carve.size();

        CellMeshResult res = cell->build_cell_meshes(
            particles, scratch, /*simplification*/1.0f, base_detail,
            /*max_pow*/6, /*uniform_detail*/0.0f, carvePtr, carveCount,
            gstagesPtr, fatPtr, fatCount, clusterStage);

        // Register each group's GL-free triangle arrays directly into the BLAS
        // and place an identity instance in the TLAS.
        for (GroupMeshResult& g : res.groups) {
            if (g.triangles.empty()) continue;
            // Determinism: Tri unions a float3 (12B) with an __m128 (16B), so
            // each vertex slot has 4 padding bytes the mesher never writes.
            // save_v2 serializes the entry's Tri bytes verbatim, so that stale
            // stack garbage would make re-bakes byte-differ. Re-pack each Tri
            // through a value-initialized copy (zeroed padding) before
            // registering so the saved .part is byte-stable across bakes.
            std::vector<Tri> norm(g.triangles.size());
            for (size_t i = 0; i < g.triangles.size(); ++i) {
                Tri t;
                std::memset(&t, 0, sizeof(Tri));   // zero union padding too
                t.vertex0 = g.triangles[i].vertex0;
                t.vertex1 = g.triangles[i].vertex1;
                t.vertex2 = g.triangles[i].vertex2;
                t.centroid = g.triangles[i].centroid;
                norm[i] = t;
            }
            // TriEx is 16-byte aligned (float4 tint) with trailing padding
            // bytes the mesher leaves uninitialized; re-pack through a
            // memset-zeroed copy for the same byte-stability reason as Tri.
            // (Value-init {} does not reliably zero trailing alignment
            // padding for a class with default member initializers.)
            //
            // Per-vertex normals: keep the mesher's smooth (SDF-gradient) normals,
            // which are deterministic given the single shared SurfaceScratch above
            // (a fresh-per-cell scratch handed each cell a freshly realloc'd, and
            // therefore garbage, normal buffer; the marching-cubes pass never
            // writes that buffer and compute_surface_normals_impl reads the
            // uninitialized value for degenerate vertices, which then varied
            // run-to-run). As a robustness guard against any residual degenerate
            // vertex whose normal comes back non-finite or non-unit, fall back to
            // the deterministic geometric face normal derived from the (byte-
            // identical) Tri vertices.
            std::vector<TriEx> normEx(g.triangle_normals.size());
            for (size_t i = 0; i < g.triangle_normals.size(); ++i) {
                TriEx e;
                std::memset(&e, 0, sizeof(TriEx));   // zero all bytes incl. padding
                const TriEx& s = g.triangle_normals[i];
                e.uv0=s.uv0; e.uv1=s.uv1; e.uv2=s.uv2;
                auto finite_unit = [](const float3& v){
                    float l2 = v.x*v.x + v.y*v.y + v.z*v.z;
                    return std::isfinite(l2) && l2 > 0.25f && l2 < 4.0f; // |v| in [0.5,2]
                };
                if (finite_unit(s.N0) && finite_unit(s.N1) && finite_unit(s.N2)) {
                    e.N0=s.N0; e.N1=s.N1; e.N2=s.N2;
                } else {
                    // Deterministic geometric face normal from the byte-identical Tri.
                    const Tri& tr = norm[i];
                    float ax=tr.vertex1.x-tr.vertex0.x, ay=tr.vertex1.y-tr.vertex0.y, az=tr.vertex1.z-tr.vertex0.z;
                    float bx=tr.vertex2.x-tr.vertex0.x, by=tr.vertex2.y-tr.vertex0.y, bz=tr.vertex2.z-tr.vertex0.z;
                    float nx=ay*bz-az*by, ny=az*bx-ax*bz, nz=ax*by-ay*bx;
                    float nl=std::sqrt(nx*nx+ny*ny+nz*nz);
                    if (nl>1e-12f) { nx/=nl; ny/=nl; nz/=nl; } else { nx=0; ny=0; nz=1; }
                    float3 fn = make_float3(nx,ny,nz);
                    e.N0=fn; e.N1=fn; e.N2=fn;
                }
                e.materialId=s.materialId;
                // tint/ao: derive deterministically rather than copy the mesher's
                // values. AO is not baked in SP-2 (default 1.0 = unoccluded), and
                // the authored per-material default tint (alpha 0 => use material
                // albedo) is used. This keeps the saved asset independent of the
                // mesher's per-triangle nearest-particle tint lookup.
                e.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);
                e.ao0 = 1.0f; e.ao1 = 1.0f; e.ao2 = 1.0f;
                normEx[i]=e;
            }
            if (!stack) {
                BLASHandle h = blas.register_triangles(norm, normEx);
                if (h != INVALID_BLAS_HANDLE) {
                    tlas.load_identity();
                    tlas.draw(h, g.group_id);
                }
            } else {
                auto& acc = region_acc[g.group_id];
                acc.first.insert(acc.first.end(), norm.begin(), norm.end());
                acc.second.insert(acc.second.end(), normEx.begin(), normEx.end());
            }
        }
    }
    DestroySurfaceScratch(scratch);

    if (stack) {
        for (auto& entry : region_acc) {
            const uint32_t group_id = entry.first;
            auto& acc = entry.second;
            if (acc.first.empty()) continue;
            MeshIndexed welded = from_tri(acc.first, &acc.second);
            char glabel[128];
            std::snprintf(glabel, sizeof(glabel), "%s group %u", label.c_str(), group_id);
            MeshIndexed done = modifier_apply::apply_stack(std::move(welded), *stack, glabel);
            if (done.positions.empty() || done.indices.empty()) continue;
            ensure_triex(done, acc.second[0]);
            std::vector<Tri> tris; std::vector<TriEx> triex;
            to_tri(done, tris, triex);
            // memset-zero repack (byte-stable .part) — same field-by-field
            // repack the per-cell path above performs on its norm/normEx.
            std::vector<Tri> norm(tris.size());
            std::vector<TriEx> normEx(triex.size());
            for (size_t i = 0; i < tris.size(); ++i) {
                std::memset(&norm[i], 0, sizeof(Tri));
                norm[i].vertex0 = tris[i].vertex0;
                norm[i].vertex1 = tris[i].vertex1;
                norm[i].vertex2 = tris[i].vertex2;
                norm[i].centroid = tris[i].centroid;

                std::memset(&normEx[i], 0, sizeof(TriEx));
                const TriEx& s = triex[i];
                normEx[i].uv0 = s.uv0; normEx[i].uv1 = s.uv1; normEx[i].uv2 = s.uv2;
                normEx[i].N0  = s.N0;  normEx[i].N1  = s.N1;  normEx[i].N2  = s.N2;
                normEx[i].materialId = s.materialId;
                normEx[i].tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);
                normEx[i].ao0 = 1.0f; normEx[i].ao1 = 1.0f; normEx[i].ao2 = 1.0f;
            }
            BLASHandle h = blas.register_triangles(norm, normEx);
            if (h != INVALID_BLAS_HANDLE) {
                tlas.load_identity();
                tlas.draw(h, group_id);
            }
        }
    }
}

BakeResult ScriptHost::bake_source(const std::string& source,
                                   const std::string& params_json,
                                   const BakeOptions& opts,
                                   const uint64_t* child_hashes,
                                   size_t child_count,
                                   const std::string* child_modules,
                                   const std::string* child_params) {
    BakeResult r;

    // Hoist rt/ctx to outer scope so the catch handler can clean them up if
    // bad_alloc fires after QuickJS init. Both start null; the catch guard
    // checks before calling Free so a pre-init throw is safe.
    JSRuntime* rt  = nullptr;
    JSContext* ctx = nullptr;

    // Outer boundary: any std::bad_alloc thrown by build()'s particle table,
    // the DSL buffer, or the marching-cubes mesh accumulation surfaces here
    // as a structured error rather than aborting the viewer.
    try {

    // MATTER_BAKE_PROFILE=1: per-bake phase timing line on stderr (diagnostic).
    static const bool prof_on = std::getenv("MATTER_BAKE_PROFILE") != nullptr;
    using prof_clock = std::chrono::steady_clock;
    prof_clock::time_point prof_t0 = prof_clock::now();
    prof_clock::time_point prof_t  = prof_t0;
    double prof_fold = 0, prof_ctx = 0, prof_eval = 0, prof_merge = 0,
           prof_build = 0, prof_mesh = 0, prof_save = 0;
    std::string prof_class;
    auto prof_lap = [&]() -> double {
        prof_clock::time_point n = prof_clock::now();
        double ms = std::chrono::duration<double, std::milli>(n - prof_t).count();
        prof_t = n;
        return ms;
    };

    // Perf fix: fold sources once (removing the redundant fold that was inside
    // merge_params_canonical) and spin up a single JSRuntime for the whole bake.
    // The canonical params merge is performed in the bake context itself after the
    // class is evaluated, eliminating the second JSRuntime that merge_params_canonical
    // previously created. The fold result, merged string, and resolved_hash are all
    // byte-identical to the previous double-RT path.
    module_resolver::FoldResult fold;
    if (!shared_lib_roots_.empty()) {
        std::string ferr;
        if (!fold_sources_cached(source, fold, ferr)) {
            // Fail-closed: a missing/illegal shared module aborts the bake with
            // no artifact written, matching the existing fail-closed pattern.
            r.error.ok = false;
            r.error.message = "module resolution failed: " + ferr;
            return r;
        }
    }

    // If the part imports shared-lib modules, serve their (already-folded) source
    // to the QuickJS module loader and evaluate the part as an ES module so the
    // `import`s bind. The loader reads ONLY this in-memory set (no filesystem at
    // eval) to keep the bake deterministic and file-access-free.
    ModuleStore store = store_from_fold(fold);
    const bool use_module = !store.sources.empty();
    prof_fold = prof_lap();

    rt = JS_NewRuntime();
    if (use_module) JS_SetModuleLoaderFunc(rt, sh_module_normalize, sh_module_loader, &store);

    // Install a wall-clock interrupt so a runaway build() fails-closed (dev-mode)
    // instead of hanging. Unbounded when time_budget_ms == 0 (install-mode).
    InterruptCtx ic;
    ic.bounded = opts.time_budget_ms > 0;
    ic.deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(opts.time_budget_ms);
    JS_SetInterruptHandler(rt, interrupt_cb, &ic);

    ctx = new_bake_context(rt, /*want_modules*/ use_module);

    // C++-owned authoring state for this bake; native DSL bindings mutate it.
    // RNG seed and child-hash table are installed below after the class is evaluated
    // (they depend on `merged` which is extracted from the class's static params).
    dsl::DslState state;
    JS_SetContextOpaque(ctx, &state);
    state.set_budget(ic.deadline, ic.bounded);
    // `merged` is populated after class eval (single-RT merge step below).
    std::string merged;

    last_build_ran_ = false;
    last_buffer_.clear();
    last_ambient_probe_.clear();
    {
    JSValue base = JS_Eval(ctx, kPartBaseJS, strlen(kPartBaseJS), "<part-base>",
                           JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(base)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,base); goto done; }
    JS_FreeValue(ctx, base);
    dsl::install_bindings(ctx);
    prof_ctx = prof_lap();

    {
        // Eval user source + a generic trampoline that publishes the authored
        // class (lexically declared) onto globalThis.__partClass.
        std::string className = find_part_class_name(source);
        if (className.empty()) {
            r.error.ok = false; r.error.message = kNoPartClassMsg;
            goto done;
        }
        prof_class = className;
        std::string wrapped = source + "\n;globalThis.__partClass = " + className + ";\n";
        if (use_module) {
            bool threw = false;
            JSValue v = eval_part_as_module(ctx, rt, wrapped, &threw);
            if (threw) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,v); goto done; }
            JS_FreeValue(ctx, v);
        } else {
            JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<part>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,v); goto done; }
            JS_FreeValue(ctx, v);
        }
        prof_eval = prof_lap();

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue authored = JS_GetPropertyStr(ctx, global, "__partClass");
        JS_FreeValue(ctx, global);
        if (!JS_IsFunction(ctx, authored)) {
            JS_FreeValue(ctx, authored);
            r.error.ok = false; r.error.message = kNoPartClassMsg;
            goto done;
        }

        // Single-RT merge: extract static params from the class and merge with
        // caller overrides in this context — same logic as merge_params_canonical
        // but without spinning up a second JSRuntime. Produces byte-identical
        // `merged` and `r.resolved_hash` since the merge JS snippet and inputs
        // (static params, params_json) are identical to the old double-RT path.
        {
            JSValue staticParams = JS_GetPropertyStr(ctx, authored, "params");
            if (JS_IsUndefined(staticParams)) staticParams = JS_NewObject(ctx);
            JSValue overrides = JS_ParseJSON(ctx, params_json.c_str(), params_json.size(), "<params>");
            if (JS_IsException(overrides)) {
                r.error = harvest_exception(ctx);
                JS_FreeValue(ctx, staticParams); JS_FreeValue(ctx, overrides);
                JS_FreeValue(ctx, authored); goto done;
            }
            static const char* kMerge =
              "(function(d,o){let m=Object.assign({},d,o);"
              "let keys=Object.keys(m).sort();let r={};for(let k of keys)r[k]=m[k];"
              "return JSON.stringify(r);})";
            JSValue mergeFn = JS_Eval(ctx, kMerge, strlen(kMerge), "<merge>",
                                      JS_EVAL_TYPE_GLOBAL);
            JSValue args2[2] = { staticParams, overrides };
            JSValue mergedStr = JS_Call(ctx, mergeFn, JS_UNDEFINED, 2, args2);
            if (JS_IsException(mergedStr)) {
                r.error = harvest_exception(ctx);
                JS_FreeValue(ctx, mergedStr); JS_FreeValue(ctx, mergeFn);
                JS_FreeValue(ctx, overrides); JS_FreeValue(ctx, staticParams);
                JS_FreeValue(ctx, authored); goto done;
            }
            const char* mjson = JS_ToCString(ctx, mergedStr);
            merged = mjson ? mjson : "{}";
            last_merged_params_ = merged;
            if (mjson) JS_FreeCString(ctx, mjson);
            JS_FreeValue(ctx, mergedStr); JS_FreeValue(ctx, mergeFn);
            JS_FreeValue(ctx, overrides); JS_FreeValue(ctx, staticParams);

            // Compute resolved_hash (same inputs as the old double-RT path: folded
            // source bytes, canonical merged params, child hashes).
            const char* src_bytes = fold.folded.empty() ? source.data() : fold.folded.data();
            size_t      src_len   = fold.folded.empty() ? source.size() : fold.folded.size();
            r.resolved_hash = part_asset::compute_resolved_hash(
                src_bytes, src_len,
                merged.data(), merged.size(),
                child_hashes, child_count);

            // Seed the deterministic RNG and install the child-hash table now that
            // `merged` is available (deferred from the old pre-eval setup block).
            state.set_rng(derive_seed(merged));
            {
                std::map<std::string, uint64_t> name2hash;
                if (child_modules && child_hashes)
                    for (size_t ci = 0; ci < child_count; ++ci) {
                        name2hash[child_modules[ci]] = child_hashes[ci];
                        if (child_params) {
                            std::string key = child_modules[ci];
                            key.push_back('\x1f');
                            key += child_params[ci];
                            name2hash[key] = child_hashes[ci];
                        }
                    }
                state.set_child_hashes(std::move(name2hash));
            }
            // Thread the world field binding so terrainVolume can call the mesher.
            state.set_world(opts.world);
        }
        prof_merge = prof_lap();

        JSValue inst = JS_CallConstructor(ctx, authored, 0, nullptr);
        JS_FreeValue(ctx, authored);
        if (JS_IsException(inst)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx,inst); goto done; }
        JSValue paramsObj = JS_ParseJSON(ctx, merged.c_str(), merged.size(), "<merged>");
        if (JS_IsException(paramsObj)) { JS_FreeValue(ctx, paramsObj); paramsObj = JS_NewObject(ctx); }
        JSAtom buildAtom = JS_NewAtom(ctx, "build");
        JSValue bret = JS_Invoke(ctx, inst, buildAtom, 1, &paramsObj);
        JS_FreeAtom(ctx, buildAtom);
        last_build_ran_ = !JS_IsException(bret);
        if (JS_IsException(bret)) {
            r.error = harvest_exception(ctx);
            // Distinguish a time-budget abort (interrupt) from an authored throw so
            // callers get a structured, actionable message.
            if (ic.bounded && std::chrono::steady_clock::now() >= ic.deadline) {
                r.error.ok = false;
                r.error.message = "time budget exceeded (interrupt)";
            }
        }
        JS_FreeValue(ctx, bret);
        JS_FreeValue(ctx, paramsObj);
        JS_FreeValue(ctx, inst);
        prof_build = prof_lap();

        // Capture globalThis.__amb (a probe authored code may set) so tests can
        // assert the bake context exposes no ambient Date/require/fetch/os bindings.
        JSValue g2 = JS_GetGlobalObject(ctx);
        JSValue amb = JS_GetPropertyStr(ctx, g2, "__amb");
        if (JS_IsString(amb)) {
            const char* s = JS_ToCString(ctx, amb);
            if (s) { last_ambient_probe_ = s; JS_FreeCString(ctx, s); }
        }
        JS_FreeValue(ctx, amb);
        JS_FreeValue(ctx, g2);
    }
    } // close DslState-scope block

    // P3 lazy-emission flush point: build end. An unclaimed POLYGON profile (one
    // authored with beginShape(POLYGON)/endShape but never extruded) flat-fills
    // here as a triangulated face. No-op when nothing is retained. Guarded on a
    // clean build so a failed bake emits no stray geometry.
    if (r.error.ok && !state.has_error())
        state.flush_retained_profile();

    // Fail-closed: a DSL session/transform misuse during build surfaces here.
    // Note: we copy (not move) here because lower_build_buffer and the base_detail
    // probe below still need state.buffer() on the success path. The move would
    // have emptied the buffer before those reads, causing zero-triangle bakes.
    last_buffer_ = state.buffer();
    if (r.error.ok && state.has_error()) {
        r.error.ok = false;
        r.error.message = state.error();
    }
    // A session left open at end of build is a misuse.
    if (r.error.ok && state.session() != dsl::Session::None) {
        r.error.ok = false;
        r.error.message = "session left open at end of build";
    }
    if (r.error.ok && state.modifier_region_open()) {
        r.error.ok = false;
        r.error.message =
            "modifier region left open at end of build (beginModifier without endModifier)";
    }
    // G7: an unbalanced transform stack (a pushMatrix without a matching
    // popMatrix) leaves depth > 1. Fail closed, same path as the open-session
    // check, so authoring bugs surface instead of silently leaking a frame.
    if (r.error.ok && state.stack_depth() != 1) {
        r.error.ok = false;
        r.error.message = "transform stack left unbalanced at end of build "
                          "(pushMatrix without matching popMatrix)";
    }

    // Success path: lower the build buffer to particles, surface per-cell BLAS,
    // and serialize one .part via SP-1's save_v2. Fail-closed: writes nothing on
    // any error branch above (r.error.ok already false there).
    //
    // The prototype's Cluster::force_rebuild_all_cells commits each cell mesh via
    // Cell::commit_cell_meshes, which calls raylib UploadMesh (a GL upload). The
    // bake is headless (no GL context), so we cannot drive Cluster. Instead we
    // partition particles into Cells exactly as the prototype does, mesh each cell
    // GL-FREE via Cell::build_cell_meshes, and register the resulting triangle
    // arrays straight into the BLAS (the same register_triangles path the headless
    // part-asset tests use), skipping the GL UploadMesh entirely.
    if (r.error.ok) {
        // Bake-time TLAS holds one draw per marching-cubes mesh group (≈ one per
        // surface cell). A detailed/large trunk touches well over the default 100
        // cells, and exceeding the cap silently drops geometry from the baked part,
        // so size it generously for the part bake.
        BLASManager blas; TLASManager tlas(65536);

        // Partition state.buffer().ops into (base, regions[]) by the recorded
        // ModifierRegion op ranges. Regions are ordered and non-overlapping
        // (dsl_state disallows nesting), so a single walk suffices.
        const std::vector<dsl::ModifierRegion>& regions = state.modifier_regions();
        dsl::BuildBuffer base_buf;
        std::vector<dsl::BuildBuffer> region_bufs(regions.size());
        {
            const std::vector<dsl::BuildOp>& all_ops = state.buffer().ops;
            size_t ri = 0;
            for (size_t i = 0; i < all_ops.size(); ++i) {
                while (ri < regions.size() && i >= regions[ri].op_end) ++ri;
                if (ri < regions.size() && i >= regions[ri].op_begin)
                    region_bufs[ri].ops.push_back(all_ops[i]);
                else
                    base_buf.ops.push_back(all_ops[i]);
            }
        }
        char plabel[64];
        std::snprintf(plabel, sizeof(plabel), "part %016llx",
                      (unsigned long long)r.resolved_hash);
        if (!base_buf.ops.empty())
            mesh_sdf_ops(base_buf, nullptr, plabel, blas, tlas);
        for (size_t ri = 0; ri < regions.size(); ++ri) {
            if (region_bufs[ri].ops.empty()) continue;
            char rlabel[96];
            std::snprintf(rlabel, sizeof(rlabel), "%s region %zu", plabel, ri);
            mesh_sdf_ops(region_bufs[ri], &regions[ri].stack, rlabel, blas, tlas);
        }

        // Direct-triangle session: register the DSL's accumulated mesh triangles as
        // one more BLAS with an identity TLAS instance. These are literal surfaces
        // (leaf blades, line-tube twigs) that never entered the SDF/voxel path.
        // Re-pack each Tri/TriEx through a memset-zeroed copy for the same byte-
        // stability reason as the voxel path above (union/alignment padding the
        // emitter never writes would otherwise make re-bakes byte-differ).
        const tri_emit::TriangleBuildBuffer* tb = state.triangle_buffer();
        if (tb && !tb->triangles().empty()) {
            const std::vector<Tri>&   src_t = tb->triangles();
            const std::vector<TriEx>& src_e = tb->tri_extra();
            // Partition the direct-triangle stream into base + one bucket per
            // modifier region using each region's recorded [tri_begin, tri_end).
            // Single walk over the flat stream (regions are ordered/non-overlapping).
            std::vector<Tri>   base_t;   std::vector<TriEx> base_e;
            std::vector<std::vector<Tri>>   region_t(regions.size());
            std::vector<std::vector<TriEx>> region_e(regions.size());
            {
                size_t ri = 0;
                for (size_t i = 0; i < src_t.size(); ++i) {
                    while (ri < regions.size() && i >= regions[ri].tri_end) ++ri;
                    if (ri < regions.size() && i >= regions[ri].tri_begin) {
                        region_t[ri].push_back(src_t[i]);
                        if (i < src_e.size()) region_e[ri].push_back(src_e[i]);
                    } else {
                        base_t.push_back(src_t[i]);
                        if (i < src_e.size()) base_e.push_back(src_e[i]);
                    }
                }
            }
            // Base direct-triangle path: register each triangle as-is.
            if (!base_t.empty()) {
                std::vector<Tri>   norm(base_t.size());
                std::vector<TriEx> normEx(base_t.size());
                for (size_t i = 0; i < base_t.size(); ++i) {
                    Tri t; std::memset(&t, 0, sizeof(Tri));
                    t.vertex0 = base_t[i].vertex0; t.vertex1 = base_t[i].vertex1;
                    t.vertex2 = base_t[i].vertex2; t.centroid = base_t[i].centroid;
                    norm[i] = t;
                }
                for (size_t i = 0; i < base_t.size(); ++i) {
                    TriEx e; std::memset(&e, 0, sizeof(TriEx));
                    if (i < base_e.size()) {
                        const TriEx& s = base_e[i];
                        e.uv0=s.uv0; e.uv1=s.uv1; e.uv2=s.uv2;
                        e.N0=s.N0; e.N1=s.N1; e.N2=s.N2;
                        e.materialId=s.materialId;
                        e.tint=s.tint;
                    } else {
                        e.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);
                    }
                    e.ao0 = e.ao1 = e.ao2 = 1.0f;
                    normEx[i]=e;
                }
                BLASHandle h = blas.register_triangles(norm, normEx);
                if (h != INVALID_BLAS_HANDLE) {
                    tlas.load_identity();
                    tlas.draw(h, 0);
                }
            }

            // Each non-empty region chunk: weld -> apply_stack -> ensure_triex ->
            // unweld -> memset-zero repack -> register as one BLAS entry (group 0).
            for (size_t ri = 0; ri < regions.size(); ++ri) {
                if (region_t[ri].empty()) continue;
                MeshIndexed welded = from_tri(region_t[ri], &region_e[ri]);
                char rlabel[128];
                std::snprintf(rlabel, sizeof(rlabel),
                              "part %016llx region %zu (tris)",
                              (unsigned long long)r.resolved_hash, ri);
                MeshIndexed done = modifier_apply::apply_stack(std::move(welded),
                                                               regions[ri].stack, rlabel);
                if (done.positions.empty() || done.indices.empty()) continue;
                TriEx proto{};
                proto.tint = make_float4(1.0f, 1.0f, 1.0f, 0.0f);
                if (!region_e[ri].empty()) proto = region_e[ri][0];
                ensure_triex(done, proto);
                std::vector<Tri> tris; std::vector<TriEx> triex;
                to_tri(done, tris, triex);
                std::vector<Tri>   norm(tris.size());
                std::vector<TriEx> normEx(triex.size());
                for (size_t i = 0; i < tris.size(); ++i) {
                    std::memset(&norm[i], 0, sizeof(Tri));
                    norm[i].vertex0 = tris[i].vertex0;
                    norm[i].vertex1 = tris[i].vertex1;
                    norm[i].vertex2 = tris[i].vertex2;
                    norm[i].centroid = tris[i].centroid;

                    std::memset(&normEx[i], 0, sizeof(TriEx));
                    const TriEx& s = triex[i];
                    normEx[i].uv0 = s.uv0; normEx[i].uv1 = s.uv1; normEx[i].uv2 = s.uv2;
                    normEx[i].N0  = s.N0;  normEx[i].N1  = s.N1;  normEx[i].N2  = s.N2;
                    normEx[i].materialId = s.materialId;
                    normEx[i].tint = s.tint;
                    normEx[i].ao0 = 1.0f; normEx[i].ao1 = 1.0f; normEx[i].ao2 = 1.0f;
                }
                BLASHandle h = blas.register_triangles(norm, normEx);
                if (h != INVALID_BLAS_HANDLE) {
                    tlas.load_identity();
                    tlas.draw(h, 0);
                }
            }
        }

        tlas.build(blas);
        // Persist the child instances placed via placeChild() during build().
        std::vector<part_asset::ChildInstance> kids;
        kids.reserve(state.children().size());
        for (const auto& c : state.children()) {
            part_asset::ChildInstance ci;
            ci.child_resolved_hash = c.hash;
            std::memcpy(ci.transform, c.transform, sizeof ci.transform);
            kids.push_back(ci);
        }
        // Build the write path: if opts.parts_dir is non-empty, make it absolute
        // by joining parts_dir + "/" + cache_path_resolved(...).  Otherwise fall
        // back to the legacy cwd-relative "parts/<hash>.part" so callers that
        // chdir() themselves (e.g. existing tests) keep working unchanged.
        std::string rel_path = part_asset::cache_path_resolved(r.resolved_hash);
        std::string path = opts.parts_dir.empty()
                           ? rel_path
                           : opts.parts_dir + "/" + rel_path;
        part_asset::LodLevels lods{};   // SP-2 writes no LOD array.
        prof_mesh = prof_lap();
        bool ok = part_asset::save_v2(path, blas, tlas,
                                      kids.empty() ? nullptr : kids.data(), kids.size(),
                                      lods, r.resolved_hash);
        if (!ok) { r.error.ok = false; r.error.message = "save_v2 failed"; }
        else {
            r.written_path = path;
            part_asset::FlattenHints hints;
            {
                const auto& kids = state.children();
                for (size_t i = 0; i < kids.size(); ++i)
                    if (kids[i].instanced)
                        hints.child_px[(uint32_t)i] = kids[i].inline_below_px;
            }
            if (!hints.child_px.empty()) {
                std::string hpath = opts.parts_dir.empty()
                    ? part_asset::cache_path_hints(r.resolved_hash)
                    : opts.parts_dir + "/" + part_asset::cache_path_hints(r.resolved_hash);
                part_asset::save_flatten_hints(hpath, hints);
            }
        }
        prof_save = prof_lap();
    }

done:
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (prof_on) {
        double total = std::chrono::duration<double, std::milli>(
                           prof_clock::now() - prof_t0).count();
        std::fprintf(stderr,
            "[bake_profile] %s total=%.1f fold=%.1f ctx=%.1f eval=%.1f "
            "merge=%.1f build=%.1f mesh=%.1f save=%.1f free=%.1f\n",
            prof_class.empty() ? "?" : prof_class.c_str(), total,
            prof_fold, prof_ctx, prof_eval, prof_merge, prof_build,
            prof_mesh, prof_save,
            total - (prof_fold + prof_ctx + prof_eval + prof_merge +
                     prof_build + prof_mesh + prof_save));
    }
    return r;

    } catch (const std::bad_alloc& e) {
        // r.resolved_hash may or may not have been populated by the pre-eval
        // hash step; carry whatever was set at throw time so the log ties the
        // OOM to the offending part when possible.
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "OOM in script_host::bake_source "
                      "(root=%016llx, phase=bake_source): %s",
                      (unsigned long long)r.resolved_hash, e.what());
        r.error.ok = false;
        r.error.message = buf;
        r.written_path.clear();
        // ctx/rt were hoisted to outer scope; free whatever was allocated before
        // the throw. Guards are necessary: a pre-init throw leaves them null.
        if (ctx) JS_FreeContext(ctx);
        if (rt)  JS_FreeRuntime(rt);
        return r;
    }
}

// ---------------------------------------------------------------------------
// eval_world: evaluate a World-definition class, return the field program text
// and biome table JSON. Mirrors eval_requires/eval_tileset structurally.
// ---------------------------------------------------------------------------
WorldEvalResult ScriptHost::eval_world(const std::string& source,
                                       const std::string& params_json) {
    WorldEvalResult r;

    // 1. Find class name.
    std::string className = matter::world_script_detail::find_world_class_name(source);
    if (className.empty()) {
        r.message = "no class extending World found";
        return r;
    }

    // 2. Merge static params + overrides (canonical JSON).
    //    World classes extend World, not Part, so merge_params_canonical will
    //    return kNoPartClassMsg. Accept its fallback "{}" and then manually
    //    read the class's `static params` + overlay caller overrides — always,
    //    even when params_json is "{}", so that static defaults (e.g. worldSeed:42)
    //    are picked up when the caller passes no overrides.
    std::string merged;
    {
        BakeError merr;
        merged = merge_params_canonical(source, params_json, merr);
        if (!merr.ok) {
            // Expected: no `extends Part` in World source.  Merge manually.
            // Always run this block (not just when params_json != "{}") so that
            // `static params` defaults are included even with an empty override.
            merged = "{}";
            JSRuntime* prt = JS_NewRuntime();
            JSContext* pctx = JS_NewContext(prt);
            // Evaluate kWorldBaseJS + source to get the class, read static params.
            std::string setup = std::string(kWorldBaseJS) + "\n" + source
                                + "\n;globalThis.__worldClass = " + className + ";\n";
            JSValue sv = JS_Eval(pctx, setup.c_str(), setup.size(), "<world-merge>",
                                 JS_EVAL_TYPE_GLOBAL);
            if (!JS_IsException(sv)) {
                JSValue global = JS_GetGlobalObject(pctx);
                JSValue cls    = JS_GetPropertyStr(pctx, global, "__worldClass");
                JSValue spar   = JS_GetPropertyStr(pctx, cls, "params");
                JS_FreeValue(pctx, cls);
                JS_FreeValue(pctx, global);
                // Merge: start with static params, overlay caller (overrides win).
                static const char* kMerge =
                    "(function(sp,ov){"
                    "let base=sp||{};"
                    "let over=ov||{};"
                    "let m=Object.assign({},base,over);"
                    "let keys=Object.keys(m).sort();"
                    "let r={};for(let k of keys)r[k]=m[k];"
                    "return JSON.stringify(r);})";
                JSValue mfn = JS_Eval(pctx, kMerge, strlen(kMerge), "<merge>",
                                      JS_EVAL_TYPE_GLOBAL);
                JSValue pjv = JS_ParseJSON(pctx, params_json.c_str(),
                                           params_json.size(), "<params>");
                if (!JS_IsException(mfn) && !JS_IsUndefined(spar) &&
                    !JS_IsException(pjv)) {
                    JSValue args[2] = { spar, pjv };
                    JSValue res = JS_Call(pctx, mfn, JS_UNDEFINED, 2, args);
                    if (!JS_IsException(res)) {
                        const char* cs = JS_ToCString(pctx, res);
                        if (cs) { merged = cs; JS_FreeCString(pctx, cs); }
                    }
                    JS_FreeValue(pctx, res);
                }
                JS_FreeValue(pctx, pjv);
                JS_FreeValue(pctx, mfn);
                JS_FreeValue(pctx, spar);
            }
            JS_FreeValue(pctx, sv);
            JS_FreeContext(pctx);
            JS_FreeRuntime(prt);
        }
    }

    // 2b. Fold shared-lib imports so any `import` statement in the world source
    //     resolves correctly — same step eval_requires takes before setting up its
    //     runtime. Without this, a World module that imports a shared-lib helper
    //     would fail with a module-not-found error at eval time.
    module_resolver::FoldResult fold;
    ModuleStore fold_store;
    bool use_module = false;
    if (!shared_lib_roots_.empty()) {
        std::string ferr;
        if (!fold_sources_cached(source, fold, ferr)) {
            r.message = "module resolution failed: " + ferr;
            return r;
        }
        if (!fold.modules.empty()) {
            fold_store = store_from_fold(fold);
            use_module = true;
        }
    }

    // 3. Fresh runtime + context (hermetic bake: one JSRuntime per eval).
    JSRuntime* rt  = JS_NewRuntime();
    // Install the module loader when the world source has shared-lib imports,
    // exactly as eval_requires does for Part/Tileset sources.
    if (use_module)
        JS_SetModuleLoaderFunc(rt, sh_module_normalize, sh_module_loader, &fold_store);
    // Use new_bake_context (not JS_NewContext) so ES-module intrinsics are enabled
    // when use_module is true — mirrors eval_part_publish_class's context setup.
    JSContext* ctx = new_bake_context(rt, use_module);

    auto done = [&]() { JS_FreeContext(ctx); JS_FreeRuntime(rt); };

    // 4. Evaluate kWorldBaseJS into the context (defines FieldNode, noise2, …, World).
    {
        JSValue v = JS_Eval(ctx, kWorldBaseJS, strlen(kWorldBaseJS),
                            "<world-base>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(v)) {
            BakeError e = harvest_exception(ctx);
            r.message = "world-base eval failed: " + e.message;
            JS_FreeValue(ctx, v); done(); return r;
        }
        JS_FreeValue(ctx, v);
    }

    // 5. Evaluate the World class + publish it as __worldClass.
    //    When the world source has shared-lib imports the module loader (installed
    //    above) resolves them from fold_store — evaluate as an ES module so the
    //    import bindings are wired.  Without imports, fall back to global eval.
    //    Mirrors eval_part_publish_class exactly (Finding 1 fix).
    {
        std::string wrapped = source + "\n;globalThis.__worldClass = " + className + ";\n";
        if (use_module) {
            bool threw = false;
            JSValue v = eval_part_as_module(ctx, rt, wrapped, &threw);
            if (threw) {
                BakeError e = harvest_exception(ctx);
                r.message = e.message;
                JS_FreeValue(ctx, v); done(); return r;
            }
            JS_FreeValue(ctx, v);
        } else {
            JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(),
                                "<world>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) {
                BakeError e = harvest_exception(ctx);
                r.message = e.message;
                JS_FreeValue(ctx, v); done(); return r;
            }
            JS_FreeValue(ctx, v);
        }
    }

    // 6. Instantiate the class and call field(mergedParams).
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue cls    = JS_GetPropertyStr(ctx, global, "__worldClass");
    JS_FreeValue(ctx, global);

    if (!JS_IsFunction(ctx, cls)) {
        r.message = "world class not a constructor";
        JS_FreeValue(ctx, cls); done(); return r;
    }

    JSValue inst = JS_CallConstructor(ctx, cls, 0, nullptr);
    if (JS_IsException(inst)) {
        BakeError e = harvest_exception(ctx);
        r.message = "world constructor threw: " + e.message;
        JS_FreeValue(ctx, cls); done(); return r;
    }

    // Parse merged params into a JS object.
    JSValue paramsObj = JS_ParseJSON(ctx, merged.c_str(), merged.size(), "<merged>");
    if (JS_IsException(paramsObj)) paramsObj = JS_NewObject(ctx);

    // Call field(params).
    JSValue fieldFn = JS_GetPropertyStr(ctx, inst, "field");
    JSValue fieldResult = JS_UNDEFINED;
    if (!JS_IsFunction(ctx, fieldFn)) {
        r.message = "world.field() is not a function";
        JS_FreeValue(ctx, paramsObj); JS_FreeValue(ctx, fieldFn);
        JS_FreeValue(ctx, inst); JS_FreeValue(ctx, cls); done(); return r;
    }
    fieldResult = JS_Call(ctx, fieldFn, inst, 1, &paramsObj);
    JS_FreeValue(ctx, fieldFn);
    JS_FreeValue(ctx, paramsObj);

    if (JS_IsException(fieldResult)) {
        BakeError e = harvest_exception(ctx);
        r.message = e.message;
        JS_FreeValue(ctx, fieldResult);
        JS_FreeValue(ctx, inst); JS_FreeValue(ctx, cls); done(); return r;
    }

    // 7. Read the density/moisture/relief/seaLevel from fieldResult.
    int density_reg  = -1;
    int moisture_reg = -1;
    int relief_reg   = -1;
    float sea_level  = 0.0f;
    float mount_relief_thresh = 0.65f;
    float rocky_moist_thresh  = 0.35f;

    auto get_reg = [&](const char* key, int& out) {
        JSValue v = JS_GetPropertyStr(ctx, fieldResult, key);
        if (!JS_IsException(v) && !JS_IsUndefined(v)) {
            JSValue rv = JS_GetPropertyStr(ctx, v, "r");
            if (!JS_IsException(rv)) {
                uint32_t u = 0;
                JS_ToUint32(ctx, &u, rv);
                out = (int)u;
            }
            JS_FreeValue(ctx, rv);
        }
        JS_FreeValue(ctx, v);
    };
    get_reg("density",  density_reg);
    get_reg("moisture", moisture_reg);
    get_reg("relief",   relief_reg);

    {
        JSValue v = JS_GetPropertyStr(ctx, fieldResult, "seaLevel");
        if (!JS_IsException(v) && !JS_IsUndefined(v)) {
            double d = 0.0;
            JS_ToFloat64(ctx, &d, v);
            sea_level = (float)d;
        }
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, fieldResult);

    if (density_reg < 0 || moisture_reg < 0 || relief_reg < 0) {
        r.message = "field() must return { density, moisture, relief, seaLevel }";
        JS_FreeValue(ctx, inst); JS_FreeValue(ctx, cls); done(); return r;
    }

    // 8. Check biomeThresholds static.
    {
        JSValue bt = JS_GetPropertyStr(ctx, cls, "biomeThresholds");
        if (!JS_IsException(bt) && !JS_IsUndefined(bt)) {
            JSValue mr = JS_GetPropertyStr(ctx, bt, "mountRelief");
            JSValue rm = JS_GetPropertyStr(ctx, bt, "rockyMoisture");
            if (!JS_IsException(mr) && !JS_IsUndefined(mr)) {
                double d = 0.65; JS_ToFloat64(ctx, &d, mr); mount_relief_thresh = (float)d;
            }
            if (!JS_IsException(rm) && !JS_IsUndefined(rm)) {
                double d = 0.35; JS_ToFloat64(ctx, &d, rm); rocky_moist_thresh  = (float)d;
            }
            JS_FreeValue(ctx, mr);
            JS_FreeValue(ctx, rm);
        }
        JS_FreeValue(ctx, bt);
    }

    // 9. Read `static world` constants.
    {
        JSValue wc = JS_GetPropertyStr(ctx, cls, "world");
        if (!JS_IsException(wc) && !JS_IsUndefined(wc)) {
            JSValue ss   = JS_GetPropertyStr(ctx, wc, "sectorSize");
            JSValue ymin = JS_GetPropertyStr(ctx, wc, "yMin");
            JSValue ymax = JS_GetPropertyStr(ctx, wc, "yMax");
            if (!JS_IsException(ss) && !JS_IsUndefined(ss)) {
                double d = 16.0; JS_ToFloat64(ctx, &d, ss); r.sector_size = (float)d;
            }
            if (!JS_IsException(ymin) && !JS_IsUndefined(ymin)) {
                double d = -64.0; JS_ToFloat64(ctx, &d, ymin); r.y_min = (float)d;
            }
            if (!JS_IsException(ymax) && !JS_IsUndefined(ymax)) {
                double d = 192.0; JS_ToFloat64(ctx, &d, ymax); r.y_max = (float)d;
            }
            JS_FreeValue(ctx, ss);
            JS_FreeValue(ctx, ymin);
            JS_FreeValue(ctx, ymax);
        }
        JS_FreeValue(ctx, wc);
    }

    // 10. Read globalThis.__world_ops (array of op-line strings accumulated by FieldNode).
    {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue ops = JS_GetPropertyStr(ctx, g, "__world_ops");
        JS_FreeValue(ctx, g);

        uint32_t len = 0;
        JSValue lenV = JS_GetPropertyStr(ctx, ops, "length");
        JS_ToUint32(ctx, &len, lenV);
        JS_FreeValue(ctx, lenV);

        std::string prog;
        for (uint32_t i = 0; i < len; ++i) {
            JSValue line = JS_GetPropertyUint32(ctx, ops, i);
            const char* s = JS_ToCString(ctx, line);
            if (s) { prog += s; prog += '\n'; JS_FreeCString(ctx, s); }
            JS_FreeValue(ctx, line);
        }
        JS_FreeValue(ctx, ops);

        // Append directive lines.
        prog += "height r"   + std::to_string(density_reg)  + '\n';
        prog += "moisture r" + std::to_string(moisture_reg) + '\n';
        prog += "relief r"   + std::to_string(relief_reg)   + '\n';
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "seaLevel %g\n", (double)sea_level);
            prog += buf;
            std::snprintf(buf, sizeof(buf), "biome %g %g\n",
                          (double)mount_relief_thresh, (double)rocky_moist_thresh);
            prog += buf;
        }
        r.field_program = std::move(prog);
    }

    // 11. Call biomes() if present, JSON-stringify result.
    {
        JSValue biomesFn = JS_GetPropertyStr(ctx, inst, "biomes");
        if (JS_IsFunction(ctx, biomesFn)) {
            JSValue biomesResult = JS_Call(ctx, biomesFn, inst, 0, nullptr);
            if (!JS_IsException(biomesResult)) {
                JSValue global2 = JS_GetGlobalObject(ctx);
                JSValue jsonObj  = JS_GetPropertyStr(ctx, global2, "JSON");
                JSValue stringifyFn = JS_GetPropertyStr(ctx, jsonObj, "stringify");
                JS_FreeValue(ctx, global2);
                JS_FreeValue(ctx, jsonObj);
                JSValue sresult = JS_Call(ctx, stringifyFn, JS_UNDEFINED,
                                          1, &biomesResult);
                if (!JS_IsException(sresult)) {
                    const char* s = JS_ToCString(ctx, sresult);
                    if (s) { r.biomes_json = s; JS_FreeCString(ctx, s); }
                }
                JS_FreeValue(ctx, sresult);
                JS_FreeValue(ctx, stringifyFn);
            }
            JS_FreeValue(ctx, biomesResult);
        }
        JS_FreeValue(ctx, biomesFn);
    }

    JS_FreeValue(ctx, inst);
    JS_FreeValue(ctx, cls);
    done();

    r.ok = true;
    return r;
}

// Evaluate a Tileset root: fresh isolated context, records DSL verbs into a
// TilesetSpec via dsl::DslState + tileset::TilesetState. No geometry artifact
// is written. Mirrors bake_source step-for-step (params canonicalization,
// module fold, hash, context creation, child-hash table, RNG seed) with the
// differences described in the brief (enable_tileset, kTilesetBaseJS injection,
// class extraction via `extends Tileset`, no mesher, no .part output).
TilesetEvalResult ScriptHost::eval_tileset(const std::string& source,
                                           const std::string& params_json,
                                           const BakeOptions& opts,
                                           const uint64_t* child_hashes,
                                           size_t child_count,
                                           const std::string* child_modules,
                                           const std::string* child_params) {
    TilesetEvalResult r;

    // Hoist rt/ctx and the DSL state to outer scope so the catch handler can
    // perform the same cleanup as ts_done if bad_alloc fires after QuickJS
    // init. rt/ctx start null; the catch guards check before calling Free so
    // a pre-init throw is safe. state must live OUTSIDE the try: locals
    // constructed inside a try block are destroyed during unwinding before
    // the catch handler runs, so a pointer into try-scope state would dangle.
    JSRuntime* rt  = nullptr;
    JSContext* ctx = nullptr;
    dsl::DslState state;

    try {

    // Merge static params + caller overrides into canonical JSON and compute the
    // resolved hash (same fold+hash path as bake_source / resolve_hash).
    // `merged` is declared at this outer scope so it outlives the hash block and
    // is available when forwarding params to build() below.
    std::string merged;
    module_resolver::FoldResult fold;
    {
        BakeError merr;
        // Tileset classes extend Tileset (which in turn extends Part), so
        // find_part_class_name (which looks for `extends Part`) won't match.
        // merge_params_canonical falls back to "{}" and sets err.ok=false with
        // kNoPartClassMsg. For tilesets, that is the expected case (no static
        // params). Accept the fallback and only fail-closed on a genuinely bad
        // params_json parse.
        merged = merge_params_canonical(source, params_json, merr);
        if (!merr.ok && merr.message != kNoPartClassMsg) {
            r.error = merr; return r;
        }
        // If the Part class was not found (tileset case), merged is "{}".
        // Still canonicalize any caller params_json override so that (a) the
        // resolved hash changes when overrides change and (b) merged params are
        // forwarded to build() correctly.
        if (!merr.ok && merr.message == kNoPartClassMsg && !params_json.empty()
                     && params_json != "{}") {
            // Use a tiny QuickJS context to parse + sort-key-stringify params_json,
            // matching the merge behavior in merge_params_canonical.
            JSRuntime* prt = JS_NewRuntime();
            JSContext* pctx = JS_NewContext(prt);
            static const char* kSort =
                "(function(o){let keys=Object.keys(o).sort();"
                "let r={};for(let k of keys)r[k]=o[k];"
                "return JSON.stringify(r);})";
            JSValue sortFn  = JS_Eval(pctx, kSort, strlen(kSort), "<sort>", JS_EVAL_TYPE_GLOBAL);
            JSValue pjv     = JS_ParseJSON(pctx, params_json.c_str(), params_json.size(), "<params>");
            if (!JS_IsException(pjv) && !JS_IsException(sortFn)) {
                JSValue sv = JS_Call(pctx, sortFn, JS_UNDEFINED, 1, &pjv);
                if (!JS_IsException(sv)) {
                    const char* s = JS_ToCString(pctx, sv);
                    if (s) { merged = s; JS_FreeCString(pctx, s); }
                }
                JS_FreeValue(pctx, sv);
            }
            JS_FreeValue(pctx, pjv);
            JS_FreeValue(pctx, sortFn);
            JS_FreeContext(pctx);
            JS_FreeRuntime(prt);
        }

        const char* src_bytes = source.data();
        size_t      src_len   = source.size();
        if (!shared_lib_roots_.empty()) {
            std::string ferr;
            if (!fold_sources_cached(source, fold, ferr)) {
                r.error.ok = false;
                r.error.message = "module resolution failed: " + ferr;
                return r;
            }
            src_bytes = fold.folded.data();
            src_len   = fold.folded.size();
        }
        r.resolved_hash = part_asset::compute_resolved_hash(
            src_bytes, src_len,
            merged.data(), merged.size(),
            child_hashes, child_count);
    }
    // `merged` is the canonical params string computed above. Using it directly
    // (not last_merged_params_) avoids the side-effect contamination bug: parallel
    // bake_source/eval_tileset calls on the same host would otherwise overwrite
    // each other's params, and explicit params_json overrides would be silently
    // dropped for tilesets.

    ModuleStore store = store_from_fold(fold);
    const bool use_module = !store.sources.empty();

    rt = JS_NewRuntime();
    if (use_module) JS_SetModuleLoaderFunc(rt, sh_module_normalize, sh_module_loader, &store);

    InterruptCtx ic;
    ic.bounded = opts.time_budget_ms > 0;
    ic.deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(opts.time_budget_ms);
    JS_SetInterruptHandler(rt, interrupt_cb, &ic);

    ctx = new_bake_context(rt, /*want_modules*/ use_module);

    // Enable tileset mode: allocates TilesetState and attaches it to DslState.
    // (state itself is declared before the try so the catch can reach it.)
    state.enable_tileset();
    // Default master seed from merged params; an explicit tile({seed}) overrides it.
    state.tileset()->spec.cfg.seed = derive_seed(merged);
    state.set_rng(derive_seed(merged));

    // Install child-hash placement table (same as bake_source).
    {
        std::map<std::string, uint64_t> name2hash;
        if (child_modules && child_hashes)
            for (size_t i = 0; i < child_count; ++i) {
                name2hash[child_modules[i]] = child_hashes[i];
                if (child_params) {
                    std::string key = child_modules[i];
                    key.push_back('\x1f');
                    key += child_params[i];
                    name2hash[key] = child_hashes[i];
                }
            }
        state.set_child_hashes(std::move(name2hash));
    }
    JS_SetContextOpaque(ctx, &state);
    state.set_budget(ic.deadline, ic.bounded);

    {
    // Inject Part base, then Tileset base (Tileset extends Part), then DSL bindings.
    JSValue base = JS_Eval(ctx, kPartBaseJS, strlen(kPartBaseJS), "<part-base>",
                           JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(base)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx, base); goto ts_done; }
    JS_FreeValue(ctx, base);

    {
    JSValue tsbase = JS_Eval(ctx, kTilesetBaseJS, strlen(kTilesetBaseJS), "<tileset-base>",
                             JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(tsbase)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx, tsbase); goto ts_done; }
    JS_FreeValue(ctx, tsbase);
    }

    dsl::install_bindings(ctx);

    {
        std::string className = find_tileset_class_name(source);
        if (className.empty()) {
            r.error.ok = false; r.error.message = "no class extending Tileset found";
            goto ts_done;
        }
        std::string wrapped = source + "\n;globalThis.__partClass = " + className + ";\n";
        if (use_module) {
            bool threw = false;
            JSValue v = eval_part_as_module(ctx, rt, wrapped, &threw);
            if (threw) { r.error = harvest_exception(ctx); JS_FreeValue(ctx, v); goto ts_done; }
            JS_FreeValue(ctx, v);
        } else {
            JSValue v = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<tileset>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(v)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx, v); goto ts_done; }
            JS_FreeValue(ctx, v);
        }

        JSValue global = JS_GetGlobalObject(ctx);
        JSValue authored = JS_GetPropertyStr(ctx, global, "__partClass");
        JS_FreeValue(ctx, global);
        if (!JS_IsFunction(ctx, authored)) {
            JS_FreeValue(ctx, authored);
            r.error.ok = false; r.error.message = "no class extending Tileset found";
            goto ts_done;
        }
        JSValue inst = JS_CallConstructor(ctx, authored, 0, nullptr);
        JS_FreeValue(ctx, authored);
        if (JS_IsException(inst)) { r.error = harvest_exception(ctx); JS_FreeValue(ctx, inst); goto ts_done; }
        JSValue paramsObj = JS_ParseJSON(ctx, merged.c_str(), merged.size(), "<merged>");
        if (JS_IsException(paramsObj)) { JS_FreeValue(ctx, paramsObj); paramsObj = JS_NewObject(ctx); }
        JSAtom buildAtom = JS_NewAtom(ctx, "build");
        JSValue bret = JS_Invoke(ctx, inst, buildAtom, 1, &paramsObj);
        JS_FreeAtom(ctx, buildAtom);
        if (JS_IsException(bret)) {
            r.error = harvest_exception(ctx);
            if (ic.bounded && std::chrono::steady_clock::now() >= ic.deadline) {
                r.error.ok = false;
                r.error.message = "time budget exceeded (interrupt)";
            }
        }
        JS_FreeValue(ctx, bret);
        JS_FreeValue(ctx, paramsObj);
        JS_FreeValue(ctx, inst);
    }
    }

    // Collect errors from DslState (Part-verb misuse) and TilesetState (tileset-verb
    // misuse) BEFORE the tile()-called check, so a verb-ordering error surfaces with
    // its own message rather than being masked by the "never called tile()" fallback.
    if (r.error.ok && state.has_error()) {
        r.error.ok = false;
        r.error.message = state.error();
    }
    if (r.error.ok && state.tileset()->has_error) {
        r.error.ok = false;
        r.error.message = state.tileset()->error;
    }

    // Post-build check: tile() must have been called (only if no earlier error).
    if (r.error.ok && !state.tileset()->spec.tile_called) {
        r.error.ok = false;
        r.error.message = "tileset build() never called tile()";
    }

    // Task 5: invoke the variant hook 16 times (once per tile in torus order 0..15).
    // Runs only when no earlier error and the hook was registered via variant().
    if (r.error.ok && state.tileset()->variant_fn_set) {
        tileset::TilesetState* ts = state.tileset();
        const tileset::TileConfig& cfg = ts->spec.cfg;

        // Recover the duped JSValue from raw bits storage.
        JSValue variant_fn;
        std::memcpy(&variant_fn, ts->variant_fn_bits, sizeof(variant_fn));

        // Build the `r` helper object for the variant hook rng (same pattern as layer).
        JSValue g_obj = JS_GetGlobalObject(ctx);
        JSValue rng_helper = JS_NewObject(ctx);
        {
            JSValue fn_int   = JS_GetPropertyStr(ctx, g_obj, "__dsl_ts_rng_int");
            JSValue fn_float = JS_GetPropertyStr(ctx, g_obj, "__dsl_ts_rng_float");
            JS_SetPropertyStr(ctx, rng_helper, "int",   fn_int);
            JS_SetPropertyStr(ctx, rng_helper, "float", fn_float);
        }
        JS_FreeValue(ctx, g_obj);

        for (int t = 0; t < 16 && r.error.ok; ++t) {
            int row = t / 4;
            int col = t % 4;
            tileset::EdgeColors ec = tileset::tile_colors(row, col);

            // Seed the per-tile rng: placement_seed(master, 0xFFFF, t).
            dsl::Rng tile_rng(tileset::placement_seed(cfg.seed, 0xFFFF, (uint32_t)t));
            ts->param_rng = &tile_rng;

            // Record stack depth and op/child counts before the call.
            size_t stack_before = state.stack_depth();
            size_t op_begin     = state.op_count();
            size_t child_begin  = state.child_count_ts();

            // Build the argument object: { index, colors: {top,bottom,left,right}, rng }.
            JSValue arg = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, arg, "index", JS_NewInt32(ctx, t));
            {
                JSValue colors = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, colors, "top",    JS_NewInt32(ctx, ec.top));
                JS_SetPropertyStr(ctx, colors, "bottom", JS_NewInt32(ctx, ec.bottom));
                JS_SetPropertyStr(ctx, colors, "left",   JS_NewInt32(ctx, ec.left));
                JS_SetPropertyStr(ctx, colors, "right",  JS_NewInt32(ctx, ec.right));
                JS_SetPropertyStr(ctx, arg, "colors", colors);
            }
            JS_SetPropertyStr(ctx, arg, "rng", JS_DupValue(ctx, rng_helper));

            // Call the variant hook.
            JSValue ret = JS_Call(ctx, variant_fn, JS_UNDEFINED, 1, &arg);
            JS_FreeValue(ctx, arg);

            ts->param_rng = nullptr;

            if (JS_IsException(ret)) {
                r.error = harvest_exception(ctx);
                JS_FreeValue(ctx, ret);
                break;
            }
            JS_FreeValue(ctx, ret);

            // Check for errors set by DSL verbs inside the hook.
            if (state.has_error()) {
                r.error.ok = false; r.error.message = state.error(); break;
            }
            if (ts->has_error) {
                r.error.ok = false; r.error.message = ts->error; break;
            }

            // Transform-stack balance check.
            if (state.stack_depth() != stack_before) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                    "variant(): tile %d hook left transform stack unbalanced "
                    "(depth %zu before, %zu after)",
                    t, stack_before, state.stack_depth());
                r.error.ok = false; r.error.message = buf; break;
            }

            size_t op_end    = state.op_count();
            size_t child_end = state.child_count_ts();

            // Margin check: every op emitted inside the hook must keep its
            // conservative XZ AABB >= edge_strip_width from the tile bounds [0, size).
            if (op_end > op_begin) {
                const float strip = cfg.edge_strip_width;
                const float sz    = cfg.size;
                const auto& ops   = state.buffer().ops;
                bool margin_ok = true;
                for (size_t i = op_begin; i < op_end && margin_ok; ++i) {
                    const dsl::BuildOp& op = ops[i];
                    // Compute world-space conservative AABB of this brush in XZ.
                    float xmin, xmax, zmin, zmax;
                    // Helper: compute XZ extent of a world-space point cloud.
                    auto update_xz = [&](float wx, float wz) {
                        if (wx < xmin) xmin = wx;
                        if (wx > xmax) xmax = wx;
                        if (wz < zmin) zmin = wz;
                        if (wz > zmax) zmax = wz;
                    };
                    // Transform a local point by the op's transform (raylib Matrix,
                    // column-major: m0-m3=col0, m4-m7=col1, m8-m11=col2, m12-m15=col3/translation).
                    auto tx = [&](float lx, float ly, float lz, float& ox, float& oz) {
                        const Matrix& M = op.transform;
                        ox = M.m0*lx + M.m4*ly + M.m8*lz  + M.m12;
                        oz = M.m2*lx + M.m6*ly + M.m10*lz + M.m14;
                    };
                    // Initialize to first point.
                    float fx, fz;
                    tx(op.center.x, op.center.y, op.center.z, fx, fz);
                    xmin = xmax = fx; zmin = zmax = fz;

                    if (op.kind == dsl::BrushKind::Sphere) {
                        // Exact conservative AABB for a sphere under any affine M:
                        //   world_center = M * center  (already captured in fx, fz above)
                        //   world half-extent along world axis i = r * L2_norm(row_i of linear part of M)
                        // For raylib's Matrix layout (row names m0,m4,m8 = X-row; m2,m6,m10 = Z-row):
                        //   hx = r * sqrt(m0^2 + m4^2 + m8^2)
                        //   hz = r * sqrt(m2^2 + m6^2 + m10^2)
                        // This is exact for spheres under rotation + non-uniform scale (no probe samples needed).
                        const Matrix& M = op.transform;
                        float r0 = op.radius;
                        float hx = r0 * std::sqrt(M.m0*M.m0 + M.m4*M.m4 + M.m8*M.m8);
                        float hz = r0 * std::sqrt(M.m2*M.m2 + M.m6*M.m6 + M.m10*M.m10);
                        xmin = fx - hx; xmax = fx + hx;
                        zmin = fz - hz; zmax = fz + hz;
                    } else if (op.kind == dsl::BrushKind::Box) {
                        // 8 corners of the box.
                        float cx = op.center.x, cy = op.center.y, cz = op.center.z;
                        float hx = op.halfExtents.x, hy = op.halfExtents.y, hz = op.halfExtents.z;
                        for (int sx2 = -1; sx2 <= 1; sx2 += 2)
                        for (int sy2 = -1; sy2 <= 1; sy2 += 2)
                        for (int sz2 = -1; sz2 <= 1; sz2 += 2) {
                            float wx, wz;
                            tx(cx + sx2*hx, cy + sy2*hy, cz + sz2*hz, wx, wz);
                            update_xz(wx, wz);
                        }
                    } else {
                        // Capsule / Cylinder / Cone: segment endpoints a=center, b=segB.
                        // The along-axis endpoints are exact transformed points;
                        // the radial margin uses the row-norm bound (same formula as
                        // the sphere case) so it is conservative under any affine M,
                        // including rotation + non-uniform scale.  Local-axis ±r probes
                        // are non-conservative when the transform has off-diagonal terms
                        // that route Y into world X (they under-estimate world extent).
                        const Matrix& M2 = op.transform;
                        float r0 = op.radius;
                        float ax = op.center.x, ay = op.center.y, az = op.center.z;
                        float bx = op.segB.x,   by = op.segB.y,   bz = op.segB.z;
                        float r1 = op.r1;
                        // Row-norm world-space radial margins for each end radius.
                        float row_x = std::sqrt(M2.m0*M2.m0 + M2.m4*M2.m4 + M2.m8*M2.m8);
                        float row_z = std::sqrt(M2.m2*M2.m2 + M2.m6*M2.m6 + M2.m10*M2.m10);
                        // Endpoint a (radius r0): world center + ±row-norm margin.
                        float wax, waz;
                        tx(ax, ay, az, wax, waz);
                        update_xz(wax - r0*row_x, waz - r0*row_z);
                        update_xz(wax + r0*row_x, waz + r0*row_z);
                        // Endpoint b (radius r1; for capsule r1==r0, for cone r1==0).
                        float wbx, wbz;
                        tx(bx, by, bz, wbx, wbz);
                        update_xz(wbx - r1*row_x, wbz - r1*row_z);
                        update_xz(wbx + r1*row_x, wbz + r1*row_z);
                    }

                    if (xmin < strip || xmax > sz - strip ||
                        zmin < strip || zmax > sz - strip) {
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                            "variant(): tile %d content within edgeStripWidth of tile bounds",
                            t);
                        r.error.ok = false; r.error.message = buf;
                        margin_ok = false;
                    }
                }
            }

            // Margin check for child placements inside the hook.
            if (r.error.ok && child_end > child_begin) {
                const float strip = cfg.edge_strip_width;
                const float sz    = cfg.size;
                const auto& ch    = state.children();
                for (size_t i = child_begin; i < child_end && r.error.ok; ++i) {
                    // Row-major transform: X = [3], Z = [11].
                    float px = ch[i].transform[3];
                    float pz = ch[i].transform[11];
                    if (px < strip || px > sz - strip ||
                        pz < strip || pz > sz - strip) {
                        char buf[160];
                        std::snprintf(buf, sizeof(buf),
                            "variant(): tile %d content within edgeStripWidth of tile bounds",
                            t);
                        r.error.ok = false; r.error.message = buf;
                    }
                }
            }

            // Record the VariantRange if anything was emitted (and no error).
            if (r.error.ok && (op_end > op_begin || child_end > child_begin)) {
                tileset::VariantRange vr;
                vr.tile        = t;
                vr.op_begin    = op_begin;
                vr.op_end      = op_end;
                vr.child_begin = child_begin;
                vr.child_end   = child_end;
                ts->spec.variant_ranges.push_back(vr);
            }
        }

        // Free the duped fn value and mark it consumed so ts_done doesn't double-free.
        JS_FreeValue(ctx, variant_fn);
        JS_FreeValue(ctx, rng_helper);
        ts->variant_fn_set = false;
    }

    // Move spec into result (no mesher, no .part write).
    if (r.error.ok) {
        r.spec = std::move(state.tileset()->spec);
    }

ts_done:
    // Release the duped variant fn if it was never consumed by the loop above
    // (early-exit paths: build() exception, DSL/tileset errors, tile() not called).
    // variant_fn_set is cleared to false by the happy path after JS_FreeValue,
    // so this runs only when the loop was skipped entirely.
    if (state.tileset()->variant_fn_set) {
        JSValue vfn_cleanup;
        std::memcpy(&vfn_cleanup, state.tileset()->variant_fn_bits, sizeof(vfn_cleanup));
        JS_FreeValue(ctx, vfn_cleanup);
        state.tileset()->variant_fn_set = false;
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return r;

    } catch (const std::bad_alloc& e) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "OOM in script_host::eval_tileset "
                      "(root=%016llx): %s",
                      (unsigned long long)r.resolved_hash, e.what());
        r.error.ok = false;
        r.error.message = buf;
        // Perform the same cleanup as ts_done. rt/ctx/state were hoisted above
        // the try, so they survive unwinding; all are null/unset when
        // bad_alloc fires before their init.
        tileset::TilesetState* ts = state.tileset();
        if (ctx && ts && ts->variant_fn_set) {
            JSValue vfn_cleanup;
            std::memcpy(&vfn_cleanup, ts->variant_fn_bits, sizeof(vfn_cleanup));
            JS_FreeValue(ctx, vfn_cleanup);
            ts->variant_fn_set = false;
        }
        if (ctx) JS_FreeContext(ctx);
        if (rt)  JS_FreeRuntime(rt);
        return r;
    }
}

// ---------------------------------------------------------------------------
// Fold cache implementation (Task 1, Phase C).
// ---------------------------------------------------------------------------

// FNV-1a 64-bit hash of two strings (source + shared_lib_root).
// Used as cache key for (source, shared_lib_root) pairs.
static uint64_t fold_key_fnv1a64(
    const std::string& source,
    const std::vector<std::string>& ordered_roots) {
    uint64_t h = 1469598103934665603ull;  // FNV offset basis
    auto mix = [&](const std::string& s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;  // FNV prime
        }
        h ^= 0xff;
        h *= 1099511628211ull;  // separator
    };
    mix(source);
    for (const std::string& root : ordered_roots) mix(root);
    return h;
}

bool ScriptHost::fold_sources_cached(const std::string& source,
                                     module_resolver::FoldResult& out,
                                     std::string& err) {
    // If no shared-lib root, skip the cache and just return an empty fold result.
    if (shared_lib_roots_.empty()) {
        out = module_resolver::FoldResult{};
        return true;
    }

    const uint64_t key = fold_key_fnv1a64(source, shared_lib_roots_);

    // Check cache first (with lock).
    {
        std::lock_guard<std::mutex> lk(fold_mu_);
        auto it = fold_cache_.find(key);
        if (it != fold_cache_.end()) {
            ++fold_hits_;
            out = it->second;
            return true;
        }
    }

    // Cache miss: fold the sources.
    module_resolver::FoldResult fresh;
    if (!module_resolver::fold_sources(source, shared_lib_roots_, fresh, err)) {
        return false;
    }

    // Insert into cache (under lock).
    {
        std::lock_guard<std::mutex> lk(fold_mu_);
        auto ins = fold_cache_.emplace(key, std::move(fresh));
        if (ins.second) ++fold_misses_;  // Only count if emplace actually inserted
        out = ins.first->second;
    }

    return true;
}

void ScriptHost::clear_fold_cache() {
    std::lock_guard<std::mutex> lk(fold_mu_);
    fold_cache_.clear();
}

} // namespace script_host
