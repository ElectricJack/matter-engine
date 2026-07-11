// Phase C Task 1 — Seedable terrain noise with valley/mountain bands.
// Evaluates a driver script through QuickJS-ng with shared-lib module loading;
// asserts heightField(seed, worldSize) produces correct band geometry and
// amplitude characteristics. Headless (no GL, no ScriptHost machinery).
//
// Flavor: qjs (CFLAGS + INCLUDE_PATHS + QJS_INC; links module_resolver + QJS C).

#include "module_resolver.h"

extern "C" {
#include "quickjs.h"
}

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "check.h"

// ---------------------------------------------------------------------------
// Driver script: imports heightField from 'shared-lib/terrain_noise' and
// asserts the public contract described in the Task 1 spec. Writes a single
// line "TERRAIN_NOISE_OK" or "TERRAIN_NOISE_FAIL ..." via print().
// ---------------------------------------------------------------------------
static const char* kDriver = R"JS(
import { heightField } from 'shared-lib/terrain_noise';
const A = heightField(1n, 816.0), B = heightField(2n, 816.0), A2 = heightField(1n, 816.0);
let out = [];
// determinism: same seed -> same field
out.push(A.heightAt(100.5, 200.25) === A2.heightAt(100.5, 200.25));
// seed sensitivity: different seed -> different field (sample a few points)
out.push([[10,10],[400,408],[700,90]].some(([x,z]) => A.heightAt(x,z) !== B.heightAt(x,z)));
// bands by radius from center (408,408)
out.push(A.bandAt(408, 408) === 'meadow');
out.push(A.bandAt(408 + 200, 408) === 'foothills');
out.push(A.bandAt(408 + 380, 408) === 'mountains');
// mountain amplitude: max |h| over a coarse mountain-band sweep exceeds meadow max
let mMax = 0, cMax = 0;
for (let i = 0; i < 400; i++) {
  const ang = i * 0.9, rM = 370 + (i % 30), rC = (i % 60);
  mMax = Math.max(mMax, Math.abs(A.heightAt(408 + Math.cos(ang)*rM, 408 + Math.sin(ang)*rM)));
  cMax = Math.max(cMax, Math.abs(A.heightAt(408 + Math.cos(ang)*rC, 408 + Math.sin(ang)*rC)));
}
out.push(mMax > 60 && cMax < 12);
print(out.every(Boolean) ? 'TERRAIN_NOISE_OK' : 'TERRAIN_NOISE_FAIL ' + JSON.stringify(out));
)JS";

// ---------------------------------------------------------------------------
// In-memory module store (canonical specifier -> source).
// ---------------------------------------------------------------------------
struct ModuleStore {
    std::map<std::string, std::string> sources;
};

// Canonicalize specifier: strip trailing ".js".
static std::string canon_spec(const char* s) {
    std::string r(s ? s : "");
    if (r.size() >= 3 && r.compare(r.size() - 3, 3, ".js") == 0) r.resize(r.size() - 3);
    return r;
}

static char* tn_normalize(JSContext* ctx, const char* /*base*/, const char* name,
                           void* /*opaque*/) {
    std::string c = canon_spec(name);
    char* out = static_cast<char*>(js_malloc(ctx, c.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, c.c_str(), c.size() + 1);
    return out;
}

static JSModuleDef* tn_loader(JSContext* ctx, const char* module_name, void* opaque) {
    auto* store = static_cast<ModuleStore*>(opaque);
    std::string key = canon_spec(module_name);
    auto it = store->sources.find(key);
    if (it == store->sources.end()) {
        JS_ThrowReferenceError(ctx, "terrain_noise_tests: module not in store: %s",
                               module_name);
        return nullptr;
    }
    const std::string& src = it->second;
    JSValue func = JS_Eval(ctx, src.c_str(), src.size(), module_name,
                           JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(func)) return nullptr;
    JSModuleDef* m = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(func));
    JS_FreeValue(ctx, func);
    return m;
}

// Drain QJS job queue (top-level module body runs as a job).
static bool drain_jobs(JSRuntime* rt) {
    JSContext* ctx = nullptr;
    for (;;) {
        int r = JS_ExecutePendingJob(rt, &ctx);
        if (r == 0) return true;
        if (r < 0) return false;
    }
}

// ---------------------------------------------------------------------------
// Captured print output.
// ---------------------------------------------------------------------------
static std::string g_print_output;

static JSValue js_print(JSContext* ctx, JSValueConst /*this_val*/, int argc,
                        JSValueConst* argv) {
    for (int i = 0; i < argc; ++i) {
        if (i) g_print_output += ' ';
        size_t len = 0;
        const char* s = JS_ToCStringLen(ctx, &len, argv[i]);
        if (s) { g_print_output.append(s, len); JS_FreeCString(ctx, s); }
    }
    g_print_output += '\n';
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    // Resolve terrain_noise.js via module_resolver (shared-lib root relative to
    // the tests/ working directory).
    const std::string shared_lib_root = "../shared-lib";
    const std::string driver_source(kDriver);

    module_resolver::FoldResult fold;
    std::string ferr;
    if (!module_resolver::fold_sources(driver_source, shared_lib_root, fold, ferr)) {
        printf("FAIL: fold_sources: %s\n", ferr.c_str());
        ++g_failures;
        return 1;
    }

    // Build in-memory module store from fold result.
    ModuleStore store;
    for (auto& m : fold.modules)
        store.sources[m.specifier] = m.source;

    // QuickJS runtime + context.
    JSRuntime* rt = JS_NewRuntime();
    JS_SetModuleLoaderFunc(rt, tn_normalize, tn_loader, &store);

    // Use NewContextRaw + explicit intrinsics (same pattern as ScriptHost).
    JSContext* ctx = JS_NewContextRaw(rt);
    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicEval(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicPromise(ctx);
    JS_AddIntrinsicBigInt(ctx);
    JS_AddIntrinsicMapSet(ctx);
    JS_AddIntrinsicTypedArrays(ctx);
    JS_AddIntrinsicRegExpCompiler(ctx);
    JS_AddIntrinsicRegExp(ctx);

    // Bind print().
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue print_fn = JS_NewCFunction(ctx, js_print, "print", 1);
    JS_SetPropertyStr(ctx, global, "print", print_fn);
    JS_FreeValue(ctx, global);

    // Evaluate driver as ES module.
    g_print_output.clear();
    JSValue v = JS_Eval(ctx, driver_source.c_str(), driver_source.size(), "<driver>",
                        JS_EVAL_TYPE_MODULE);
    bool threw = JS_IsException(v);
    if (!threw && !drain_jobs(rt)) threw = true;

    if (threw) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        printf("FAIL: driver threw: %s\n", msg ? msg : "(unknown)");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        ++g_failures;
    }
    JS_FreeValue(ctx, v);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);

    // Trim trailing newline for comparison.
    while (!g_print_output.empty() && g_print_output.back() == '\n')
        g_print_output.pop_back();

    printf("driver output: '%s'\n", g_print_output.c_str());
    CHECK(g_print_output == "TERRAIN_NOISE_OK", "driver prints TERRAIN_NOISE_OK");

    return check_summary();
}
