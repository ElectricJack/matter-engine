#include "dsl_state.h"
#include "dsl_bindings.h"
#include "pf_bindings.h"
#include "tileset_spec.h"
#include "tileset_placement.h"
#include "tileset_layout.h"
#include "part_graph.h"   // params_from_json, params_to_json — canonical JSON normalizer
#include "terrain_mesher.h"
#include "triangle_emit.hpp"
#include <cmath>
#include <cstring>
#include <vector>
extern "C" {
#include "quickjs.h"
}

// Per-part SCHEMA-level configuration (lodBudgets, lodAnchorSize) is
// discovered via `static` class properties read by ScriptHost — NOT via the
// runtime verb bindings below. If you're looking for the lodBudgets binding,
// see: MatterEngine3/src/script_host.cpp :: eval_lod_budgets
// This file only owns the runtime-verb bindings the Part class methods forward
// to (translate, box, sphere, placeChild, tileset verbs, etc.).

namespace dsl {

// Normalize raw JS_JSONStringify bytes to the canonical params_to_json format
// (sorted keys, %.17g numbers, no whitespace) so that composite child-hash
// lookups (`module \x1f params_json`) match the keys the host writes into
// name2hash (part_graph.cpp: params_to_json(kid.params)).
// An empty input returns an empty string so callers can distinguish "no params"
// from "params that happen to parse to an empty map".
static std::string normalize_params_json(const char* s, size_t len) {
    if (!s || len == 0) return {};
    part_graph::Params p = part_graph::params_from_json(std::string(s, len));
    return part_graph::params_to_json(p);
}

static DslState* state_of(JSContext* ctx) {
    return static_cast<DslState*>(JS_GetContextOpaque(ctx));
}
static double argd(JSContext* ctx, JSValueConst v) { double d=0; JS_ToFloat64(ctx,&d,v); return d; }

static JSValue j_pushMatrix(JSContext* c, JSValueConst, int, JSValueConst*) { state_of(c)->pushMatrix(); return JS_UNDEFINED; }
static JSValue j_popMatrix (JSContext* c, JSValueConst, int, JSValueConst*) { state_of(c)->popMatrix();  return JS_UNDEFINED; }
static JSValue j_translate(JSContext* c, JSValueConst, int n, JSValueConst* a){ state_of(c)->translate(argd(c,a[0]),argd(c,a[1]),argd(c,a[2])); return JS_UNDEFINED; }
static JSValue j_rotateX(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->rotateX(argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_rotateY(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->rotateY(argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_rotateZ(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->rotateZ(argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_scale(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->scale(argd(c,a[0]),argd(c,a[1]),argd(c,a[2])); return JS_UNDEFINED; }
static JSValue j_applyMatrix(JSContext* c, JSValueConst, int, JSValueConst* a){
    float m[16]; for (int i=0;i<16;++i){ JSValue e=JS_GetPropertyUint32(c,a[0],i); m[i]=(float)argd(c,e); JS_FreeValue(c,e);} state_of(c)->applyMatrix(m); return JS_UNDEFINED; }
static JSValue j_fill(JSContext* c, JSValueConst, int, JSValueConst* a){ int32_t id=0; JS_ToInt32(c,&id,a[0]); state_of(c)->fill((uint32_t)id); return JS_UNDEFINED; }
static JSValue j_tint(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->tint((float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2]),(float)argd(c,a[3])); return JS_UNDEFINED; }
static JSValue j_lookAt(JSContext* c, JSValueConst, int n, JSValueConst* a){
    // lookAt(tx,ty,tz, [upx,upy,upz]); up defaults to +Y when omitted.
    float ux = (n>3)?(float)argd(c,a[3]):0.0f;
    float uy = (n>3)?(float)argd(c,a[4]):1.0f;
    float uz = (n>3)?(float)argd(c,a[5]):0.0f;
    state_of(c)->lookAt((float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2]),ux,uy,uz);
    return JS_UNDEFINED; }
static JSValue j_beginVoxels(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->beginVoxels((float)argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_endVoxels(JSContext* c, JSValueConst, int, JSValueConst*){ state_of(c)->endVoxels(); return JS_UNDEFINED; }
static JSValue j_sphere(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->sphere({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},(float)argd(c,a[3]),CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_box(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->box({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
                     {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_op(JSContext* c, JSValueConst, int, JSValueConst* a){
    int32_t k=0; JS_ToInt32(c,&k,a[0]); state_of(c)->set_last_op((CsgOp)k); return JS_UNDEFINED; }
static JSValue j_smoothing(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->smoothing((float)argd(c,a[0])); return JS_UNDEFINED; }

static JSValue j_raycast(JSContext* c, JSValueConst, int, JSValueConst* a){
    Vector3 hit{}, nrm{};
    bool ok = state_of(c)->raycast(
        {(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
        {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},
        hit, nrm);
    if (!ok) return JS_NULL;   // miss OR fail-closed error (bake fails anyway)
    JSValue pt = JS_NewArray(c);
    JS_SetPropertyUint32(c, pt, 0, JS_NewFloat64(c, hit.x));
    JS_SetPropertyUint32(c, pt, 1, JS_NewFloat64(c, hit.y));
    JS_SetPropertyUint32(c, pt, 2, JS_NewFloat64(c, hit.z));
    JSValue nm = JS_NewArray(c);
    JS_SetPropertyUint32(c, nm, 0, JS_NewFloat64(c, nrm.x));
    JS_SetPropertyUint32(c, nm, 1, JS_NewFloat64(c, nrm.y));
    JS_SetPropertyUint32(c, nm, 2, JS_NewFloat64(c, nrm.z));
    JSValue obj = JS_NewObject(c);
    JS_SetPropertyStr(c, obj, "point", pt);
    JS_SetPropertyStr(c, obj, "normal", nm);
    return obj;
}

static JSValue j_beginModifier(JSContext* c, JSValueConst, int, JSValueConst*) {
    state_of(c)->begin_modifier_region(); return JS_UNDEFINED; }

// Optional numeric field: returns true and fills `out` iff present (non-null).
static bool opt_num(JSContext* c, JSValueConst obj, const char* k, double& out) {
    JSValue v = JS_GetPropertyStr(c, obj, k);
    const bool has = !JS_IsUndefined(v) && !JS_IsNull(v);
    if (has) JS_ToFloat64(c, &out, v);
    JS_FreeValue(c, v);
    return has;
}

// endModifier(list): an Array of one-key objects in execution order, e.g.
//   [{ smooth: { iterations: 2 } }, { retopo: {...} }, { simplify: 0.3 }]
// Shorthand: { simplify: 0.3 } (bare number).
static JSValue j_endModifier(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    if (n < 1 || !JS_IsArray(a[0])) {
        st->set_error("endModifier: expected an array of modifier entries");
        return JS_UNDEFINED;
    }
    JSValue lenv = JS_GetPropertyStr(c, a[0], "length");
    uint32_t len = 0; JS_ToUint32(c, &len, lenv); JS_FreeValue(c, lenv);

    std::vector<ModifierSpec> stack;
    for (uint32_t i = 0; i < len && !st->has_error(); ++i) {
        JSValue entry = JS_GetPropertyUint32(c, a[0], i);
        JSPropertyEnum* props = nullptr;
        uint32_t pcount = 0;
        if (!JS_IsObject(entry) ||
            JS_GetOwnPropertyNames(c, &props, &pcount, entry,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            st->set_error("endModifier: each entry must be an object like { smooth: {...} }");
            JS_FreeValue(c, entry);
            break;
        }
        if (pcount != 1) {
            st->set_error("endModifier: each entry must have exactly one key (the modifier name)");
        } else {
            const char* key = JS_AtomToCString(c, props[0].atom);
            JSValue val = JS_GetProperty(c, entry, props[0].atom);
            ModifierSpec spec{};
            double d = 0.0;
            if (key && !std::strcmp(key, "simplify")) {
                spec.kind = ModifierKind::Simplify;
                if (JS_IsNumber(val)) d = argd(c, val);          // shorthand { simplify: 0.3 }
                else if (!opt_num(c, val, "ratio", d)) d = 0.0;  // { simplify: { ratio: 0.3 } }
                if (!(d > 0.0 && d <= 1.0))
                    st->set_error("endModifier: simplify ratio must be in (0, 1]");
                spec.ratio = (float)d;
            } else if (key && !std::strcmp(key, "smooth")) {
                spec.kind = ModifierKind::Smooth;
                if (!JS_IsObject(val)) {
                    st->set_error("endModifier: smooth params must be an object");
                } else {
                    if (opt_num(c, val, "iterations", d)) spec.iterations = (int)d;
                    if (opt_num(c, val, "lambda", d))     spec.lambda = (float)d;
                    if (opt_num(c, val, "mu", d))         spec.mu = (float)d;
                    if (spec.iterations < 1 || !(spec.lambda > 0.0f) || !(spec.mu < 0.0f))
                        st->set_error("endModifier: smooth params out of range "
                                      "(iterations>=1, lambda>0, mu<0)");
                }
            } else if (key && !std::strcmp(key, "retopo")) {
                spec.kind = ModifierKind::Retopo;
                if (!JS_IsObject(val)) {
                    st->set_error("endModifier: retopo params must be an object");
                } else {
                    if (opt_num(c, val, "target_ratio", d))    spec.target_ratio = (float)d;
                    if (opt_num(c, val, "iterations", d))      spec.retopo_iterations = (int)d;
                    if (opt_num(c, val, "seed", d))            spec.seed = (uint32_t)d;
                    if (opt_num(c, val, "timeout_seconds", d)) spec.timeout_seconds = (int)d;
                    if (!(spec.target_ratio > 0.0f) || spec.retopo_iterations < 1 ||
                        spec.timeout_seconds < 1)
                        st->set_error("endModifier: retopo params out of range");
                }
            } else {
                st->set_error(std::string("endModifier: unknown modifier '") +
                              (key ? key : "?") + "'");
            }
            if (key) JS_FreeCString(c, key);
            JS_FreeValue(c, val);
            if (!st->has_error()) stack.push_back(spec);
        }
        for (uint32_t p = 0; p < pcount; ++p) JS_FreeAtom(c, props[p].atom);
        js_free(c, props);
        JS_FreeValue(c, entry);
    }
    if (!st->has_error()) st->end_modifier_region(std::move(stack));
    return JS_UNDEFINED;
}

static JSValue j_placeChild(JSContext* c, JSValueConst, int n, JSValueConst* a){
    const char* m = JS_ToCString(c, a[0]);
    if (!m) return JS_UNDEFINED;
    // G6: optional params (a plain JS object/array) -> canonical JSON bytes folded
    // into the child's resolved hash so parametric children dedup. Normalize via
    // params_from_json -> params_to_json (sorted keys, %.17g numbers) so the lookup
    // key matches the name2hash keys the host builds with params_to_json(kid.params).
    // Without normalization, JS_JSONStringify's ES key order and shortest-repr numbers
    // differ from the canonical format, causing composite key misses and a silent
    // bare-module fallback that collapsed all parametric placements to one hash.
    // No params (undefined/null) = bare-module lookup (unchanged behavior).
    if (n > 1 && !JS_IsUndefined(a[1]) && !JS_IsNull(a[1])) {
        JSValue js = JS_JSONStringify(c, a[1], JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(js)) {
            size_t len = 0;
            const char* s = JS_ToCStringLen(c, &len, js);
            if (s) {
                std::string normalized = normalize_params_json(s, len);
                JS_FreeCString(c, s);
                // "{}" (empty params object) and empty string both mean "no params"
                // — use the bare-module lookup so a placeChild('Foo', {}) works for
                // a module declared without params in static requires.
                if (!normalized.empty() && normalized != "{}") {
                    state_of(c)->placeChild(m, normalized.c_str(), normalized.size());
                } else {
                    state_of(c)->placeChild(m);
                }
            } else {
                state_of(c)->placeChild(m);
            }
        } else {
            state_of(c)->placeChild(m);
        }
        JS_FreeValue(c, js);
    } else {
        state_of(c)->placeChild(m);
    }
    JS_FreeCString(c, m);
    return JS_UNDEFINED; }

static JSValue j_beginShape(JSContext* c, JSValueConst, int, JSValueConst* a){
    int32_t mode=0; JS_ToInt32(c,&mode,a[0]); state_of(c)->beginShape(mode); return JS_UNDEFINED; }
static JSValue j_vertex(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->vertex((float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])); return JS_UNDEFINED; }
static JSValue j_endShape(JSContext* c, JSValueConst, int, JSValueConst*){ state_of(c)->endShape(); return JS_UNDEFINED; }
static JSValue j_beginContour(JSContext* c, JSValueConst, int, JSValueConst*){ state_of(c)->beginContour(); return JS_UNDEFINED; }
static JSValue j_endContour(JSContext* c, JSValueConst, int, JSValueConst*){ state_of(c)->endContour(); return JS_UNDEFINED; }
static JSValue j_joinType(JSContext* c, JSValueConst, int, JSValueConst* a){
    int32_t k=0; JS_ToInt32(c,&k,a[0]); state_of(c)->joinType((int)k); return JS_UNDEFINED; }
static JSValue j_extrude(JSContext* c, JSValueConst, int, JSValueConst* a){
    // extrude(path): path is a JS array of points, each point a 3-element
    // [x,y,z] array. Flatten into 3*n floats for DslState::extrude.
    JSValue lenv = JS_GetPropertyStr(c, a[0], "length");
    int32_t n = 0; JS_ToInt32(c, &n, lenv); JS_FreeValue(c, lenv);
    if (n < 2) { state_of(c)->extrude(nullptr, 0); return JS_UNDEFINED; }
    std::vector<float> flat; flat.reserve((size_t)n * 3);
    for (int32_t i = 0; i < n; ++i) {
        JSValue pt = JS_GetPropertyUint32(c, a[0], (uint32_t)i);
        for (int j = 0; j < 3; ++j) {
            JSValue e = JS_GetPropertyUint32(c, pt, (uint32_t)j);
            flat.push_back((float)argd(c, e));
            JS_FreeValue(c, e);
        }
        JS_FreeValue(c, pt);
    }
    state_of(c)->extrude(flat.data(), n);
    return JS_UNDEFINED; }
static JSValue j_position(JSContext* c, JSValueConst, int, JSValueConst*){
    Vector3 p = state_of(c)->position();
    JSValue arr = JS_NewArray(c);
    JS_SetPropertyUint32(c, arr, 0, JS_NewFloat64(c, p.x));
    JS_SetPropertyUint32(c, arr, 1, JS_NewFloat64(c, p.y));
    JS_SetPropertyUint32(c, arr, 2, JS_NewFloat64(c, p.z));
    return arr; }
static JSValue j_line(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->line((float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2]),
                      (float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5]),
                      (float)argd(c,a[6]),(float)argd(c,a[7])); return JS_UNDEFINED; }
// Round primitives (Phase 4). a,b are segment endpoints; r0/r1 end radii.
// Voxel-session => SDF brush; None => clean error (mesh emitters land in Phase 5).
static JSValue j_capsule(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->capsule({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
                         {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},
                         (float)argd(c,a[6]), CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_cylinder(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->cylinder({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
                          {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},
                          (float)argd(c,a[6]), CsgOp::Union); return JS_UNDEFINED; }
static JSValue j_cone(JSContext* c, JSValueConst, int, JSValueConst* a){
    state_of(c)->cone({(float)argd(c,a[0]),(float)argd(c,a[1]),(float)argd(c,a[2])},
                      {(float)argd(c,a[3]),(float)argd(c,a[4]),(float)argd(c,a[5])},
                      (float)argd(c,a[6]), (float)argd(c,a[7]), CsgOp::Union); return JS_UNDEFINED; }

// Seeded Math.random: draws from the bake's DslState Rng (seeded by the host
// before build()). Deterministic and process-entropy-free so the resolved-hash
// <-> bytes contract holds. Falls back to 0.0 if no Rng is installed (which
// keeps the bake deterministic rather than reaching for engine entropy).
static JSValue j_random(JSContext* c, JSValueConst, int, JSValueConst*) {
    DslState* st = state_of(c);
    double d = (st && st->rng()) ? st->rng()->next_unit() : 0.0;
    return JS_NewFloat64(c, d);
}

// ---------------------------------------------------------------------------
// Tileset bindings (__dsl_ts_*)
// ---------------------------------------------------------------------------
static tileset::TilesetState* ts_of(JSContext* c) {
    dsl::DslState* s = state_of(c);
    return s ? s->tileset() : nullptr;
}

// Uniform random unit quaternion (Box-Muller). Draws 4 normal floats, normalizes.
// Guard: retry if norm is degenerate (astronomically rare).
static void random_unit_quat(dsl::Rng& r, float q[4]) {
    for (;;) {
        float g[4];
        for (int i = 0; i < 4; i += 2) {
            float u1 = (float)r.next_unit(); if (u1 < 1e-7f) u1 = 1e-7f;
            float u2 = (float)r.next_unit();
            float m = std::sqrt(-2.0f * std::log(u1));
            g[i]     = m * std::cos(6.2831853f * u2);
            g[i + 1] = m * std::sin(6.2831853f * u2);
        }
        float n = std::sqrt(g[0]*g[0] + g[1]*g[1] + g[2]*g[2] + g[3]*g[3]);
        if (n > 1e-6f) { q[0]=g[0]/n; q[1]=g[1]/n; q[2]=g[2]/n; q[3]=g[3]/n; return; }
    }
}

// Params-fn `r` helper: int(n) and float(a,b) drawing from ts->param_rng.
static JSValue j_ts_rng_int(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts || !ts->param_rng) return JS_NewInt32(c, 0);
    int32_t nn = 1; if (n > 0) JS_ToInt32(c, &nn, a[0]);
    if (nn <= 1) return JS_NewInt32(c, 0);
    uint64_t v = ts->param_rng->next_u64();
    return JS_NewInt32(c, (int32_t)(v % (uint64_t)nn));
}
static JSValue j_ts_rng_float(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts || !ts->param_rng) return JS_NewFloat64(c, 0.0);
    double lo = 0.0, hi = 1.0;
    if (n > 0) JS_ToFloat64(c, &lo, a[0]);
    if (n > 1) JS_ToFloat64(c, &hi, a[1]);
    double u = ts->param_rng->next_unit();
    return JS_NewFloat64(c, lo + u * (hi - lo));
}

static JSValue j_ts_tile(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (ts->spec.tile_called) { ts->set_error("tile() called twice"); return JS_UNDEFINED; }
    tileset::TileConfig& cfg = ts->spec.cfg;
    double v;
    if (n > 0 && !JS_IsUndefined(a[0]) && !JS_ToFloat64(c, &v, a[0])) cfg.size = (float)v;
    if (n > 1 && !JS_IsUndefined(a[1]) && !JS_ToFloat64(c, &v, a[1])) cfg.texels_per_meter = (int)v;
    if (n > 2 && !JS_IsUndefined(a[2]) && !JS_ToFloat64(c, &v, a[2])) cfg.seed = (uint64_t)(double)v;
    if (n > 3 && !JS_IsUndefined(a[3]) && !JS_ToFloat64(c, &v, a[3])) cfg.edge_strip_width = (float)v;
    if (n > 4 && !JS_IsUndefined(a[4]) && !JS_ToFloat64(c, &v, a[4])) cfg.corner_clear_radius = (float)v;
    if (cfg.size <= 0.0f) { ts->set_error("tile: size must be positive"); return JS_UNDEFINED; }
    if (cfg.texels_per_meter <= 0) { ts->set_error("tile: texelsPerMeter must be positive"); return JS_UNDEFINED; }
    if (cfg.edge_strip_width <= cfg.corner_clear_radius) { ts->set_error("tile(): edgeStripWidth must exceed cornerClearRadius"); return JS_UNDEFINED; }
    ts->spec.tile_called = true;
    return JS_UNDEFINED;
}

static JSValue j_ts_base(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (!ts->spec.tile_called) { ts->set_error("base() before tile()"); return JS_UNDEFINED; }
    if (n < 2 || !JS_IsFunction(c, a[0])) { ts->set_error("base(fn, material): fn required"); return JS_UNDEFINED; }
    uint32_t mat = 0; JS_ToUint32(c, &mat, a[1]);

    tileset::BaseField& b = ts->spec.base;
    b.n = tileset::BaseField::kSamplesPerTile;
    b.cell = ts->spec.cfg.size / (float)b.n;
    b.material = mat;
    b.heights.assign((size_t)b.n * b.n, 0.0f);
    for (int z = 0; z < b.n; ++z) {
        for (int x = 0; x < b.n; ++x) {
            JSValue args[2] = { JS_NewFloat64(c, x * b.cell), JS_NewFloat64(c, z * b.cell) };
            JSValue rv = JS_Call(c, a[0], JS_UNDEFINED, 2, args);
            JS_FreeValue(c, args[0]); JS_FreeValue(c, args[1]);
            if (JS_IsException(rv)) { ts->set_error("base(): heightfield fn threw"); return JS_EXCEPTION; }
            double h = 0.0; JS_ToFloat64(c, &h, rv); JS_FreeValue(c, rv);
            b.heights[(size_t)z * b.n + x] = (float)h;
        }
    }
    b.set = true;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// place_one_instance — shared per-point placement logic for j_ts_layer.
//
// Fills `out` with scale, pos[1], quat, and child_hash drawn from attr_rng.
// The x/z coordinates are NOT set here; the caller maps them after this call
// (strip orientation swap vs. interior pass-through differ by call site).
//
// orient: 0 = vertical strip (x=across, z=along),
//         1 = horizontal strip (x=along, z=across),
//        -1 = interior (x=pt.x, z=pt.z, no swap).
// The x/z assignment IS performed here so both call sites are unified.
//
// Does NOT free params_val or r_helper on error (caller owns them).
// Returns false and sets ts->has_error on any error; out is unmodified.
// ---------------------------------------------------------------------------
static bool place_one_instance(
    JSContext* c,
    tileset::TilesetState* ts,
    dsl::DslState* state,
    const std::string& module,
    const tileset::LayerSpec& layer,
    bool params_is_fn,
    uint64_t fixed_hash,
    JSValueConst params_val,
    JSValueConst r_helper,
    dsl::Rng& attr_rng,
    const tileset::Point2& pt,
    int orient,
    tileset::Placement& out)
{
    tileset::Placement p{};

    // scale drawn first.
    // NOTE: if scale_range transitions between degenerate ([a,a]) and
    // non-degenerate, the RNG draw is added or removed here, shifting
    // every subsequent placement attribute (y, quat, params) in the stream.
    if (layer.scale_range[0] == layer.scale_range[1]) {
        p.scale = layer.scale_range[0];
    } else {
        p.scale = layer.scale_range[0] +
                  (float)attr_rng.next_unit() * (layer.scale_range[1] - layer.scale_range[0]);
    }

    // y / quat
    if (layer.physics) {
        p.pos[1] = layer.drop_h[0] +
                   (float)attr_rng.next_unit() * (layer.drop_h[1] - layer.drop_h[0]);
        random_unit_quat(attr_rng, p.quat);
    } else {
        p.pos[1] = 0.0f;
        float angle = (float)attr_rng.next_unit() * 6.2831853f;
        float half = angle * 0.5f;
        p.quat[0] = 0.0f;
        p.quat[1] = std::sin(half);
        p.quat[2] = 0.0f;
        p.quat[3] = std::cos(half);
    }

    // params
    if (params_is_fn) {
        ts->param_rng = &attr_rng;
        JSValue ret = JS_Call(c, params_val, JS_UNDEFINED, 1, &r_helper);
        ts->param_rng = nullptr;
        if (JS_IsException(ret)) {
            JS_FreeValue(c, ret);
            ts->set_error("layer('" + module + "'): params fn threw");
            return false;
        }
        JSValue js = JS_JSONStringify(c, ret, JS_UNDEFINED, JS_UNDEFINED);
        JS_FreeValue(c, ret);
        std::string pjson;
        if (!JS_IsException(js)) {
            size_t len = 0;
            const char* s = JS_ToCStringLen(c, &len, js);
            if (s) { pjson.assign(s, len); JS_FreeCString(c, s); }
        }
        JS_FreeValue(c, js);
        // Normalize: round-trip through params_from_json -> params_to_json so the
        // composite key matches name2hash's canonical bytes (sorted keys, %.17g).
        if (!pjson.empty() && pjson != "{}") {
            pjson = normalize_params_json(pjson.c_str(), pjson.size());
        }
        uint64_t h = 0;
        // Fail-closed: if fn returned a non-trivial params object, the composite
        // key (module\x1f<params>) must be explicitly declared in static requires.
        // Trivial (empty/"{}"): fall through to plain-module lookup so that a fn
        // that returns {} for a plain-declared module still resolves correctly.
        bool has_real_params = !pjson.empty() && pjson != "{}";
        if (has_real_params &&
            !state->has_composite_child_key(module, pjson.c_str(), pjson.size())) {
            ts->set_error("layer('" + module + "'): params variant not declared in static requires");
            return false;
        }
        if (!state->lookup_child_hash(module, has_real_params ? pjson.c_str() : nullptr,
                                      has_real_params ? pjson.size() : 0, h)) {
            ts->set_error("layer('" + module + "'): params variant not declared in static requires");
            return false;
        }
        p.child_hash = h;
    } else {
        p.child_hash = fixed_hash;
    }

    // Map coordinates: strips swap x/z based on orientation; interior is direct.
    if (orient == 0) {
        p.pos[0] = pt.x; p.pos[2] = pt.z;  // vertical: x=across, z=along
    } else if (orient == 1) {
        p.pos[0] = pt.z; p.pos[2] = pt.x;  // horizontal: x=along, z=across
    } else {
        p.pos[0] = pt.x; p.pos[2] = pt.z;  // interior: direct
    }

    out = p;
    return true;
}

static JSValue j_ts_layer(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (ts->has_error) return JS_UNDEFINED;
    dsl::DslState* state = state_of(c);

    // -- Argument 0: module name --
    if (n < 1 || JS_IsUndefined(a[0])) {
        ts->set_error("layer: module name required"); return JS_UNDEFINED;
    }
    const char* mstr = JS_ToCString(c, a[0]);
    if (!mstr) { ts->set_error("layer: module name required"); return JS_UNDEFINED; }
    std::string module(mstr);
    JS_FreeCString(c, mstr);

    // -- tile() must have been called first --
    if (!ts->spec.tile_called) {
        ts->set_error("layer('" + module + "'): tile() must be called before layer()");
        return JS_UNDEFINED;
    }

    // -- Argument 1: opts object --
    JSValue opts = (n > 1 && !JS_IsUndefined(a[1])) ? a[1] : JS_UNDEFINED;

    // Helper to read a named float property from opts.
    auto get_float = [&](const char* key, bool* found) -> double {
        if (JS_IsUndefined(opts)) { if (found) *found=false; return 0.0; }
        JSValue v = JS_GetPropertyStr(c, opts, key);
        if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(c,v); if (found) *found=false; return 0.0; }
        double d = 0.0; JS_ToFloat64(c, &d, v); JS_FreeValue(c,v);
        if (found) *found=true; return d;
    };
    auto get_bool = [&](const char* key, bool def) -> bool {
        if (JS_IsUndefined(opts)) return def;
        JSValue v = JS_GetPropertyStr(c, opts, key);
        if (JS_IsUndefined(v) || JS_IsNull(v)) { JS_FreeValue(c,v); return def; }
        bool b = JS_ToBool(c, v) != 0; JS_FreeValue(c,v); return b;
    };
    auto get_str = [&](const char* key) -> std::string {
        if (JS_IsUndefined(opts)) return "";
        JSValue v = JS_GetPropertyStr(c, opts, key);
        if (!JS_IsString(v)) { JS_FreeValue(c,v); return ""; }
        const char* s = JS_ToCString(c, v); JS_FreeValue(c,v);
        if (!s) return "";
        std::string r(s); JS_FreeCString(c, s); return r;
    };
    // Read [min,max] array property.
    auto get_range = [&](const char* key, float def0, float def1, float out[2]) {
        out[0]=def0; out[1]=def1;
        if (JS_IsUndefined(opts)) return;
        JSValue v = JS_GetPropertyStr(c, opts, key);
        if (!JS_IsArray(v)) { JS_FreeValue(c,v); return; }
        JSValue e0=JS_GetPropertyUint32(c,v,0); JSValue e1=JS_GetPropertyUint32(c,v,1);
        double d0=def0, d1=def1;
        JS_ToFloat64(c,&d0,e0); JS_ToFloat64(c,&d1,e1);
        out[0]=(float)d0; out[1]=(float)d1;
        JS_FreeValue(c,e0); JS_FreeValue(c,e1); JS_FreeValue(c,v);
    };

    // -- Parse density (required) --
    bool density_found = false;
    double density_val = get_float("density", &density_found);
    if (!density_found || density_val <= 0.0) {
        ts->set_error("layer('" + module + "'): density required");
        return JS_UNDEFINED;
    }

    // -- Parse remaining opts with defaults --
    tileset::LayerSpec layer;
    layer.module = module;
    layer.density = (float)density_val;

    // placement: 'uniform'(0) / 'poisson'(1) / 'cluster'(2).
    // Fail-closed: if the property is present and not a string, error rather than
    // silently falling back to uniform (e.g. placement:42 must not compile).
    {
        layer.placement_kind = 0;   // default: uniform
        if (!JS_IsUndefined(opts)) {
            JSValue pv = JS_GetPropertyStr(c, opts, "placement");
            bool has_placement = !JS_IsUndefined(pv) && !JS_IsNull(pv);
            if (has_placement && !JS_IsString(pv)) {
                JS_FreeValue(c, pv);
                ts->set_error("layer('" + module + "'): placement must be a string");
                return JS_UNDEFINED;
            }
            if (has_placement) {
                const char* ps = JS_ToCString(c, pv);
                std::string pk(ps ? ps : "");
                if (ps) JS_FreeCString(c, ps);
                JS_FreeValue(c, pv);
                if (pk.empty() || pk == "uniform") layer.placement_kind = 0;
                else if (pk == "poisson")          layer.placement_kind = 1;
                else if (pk == "cluster")          layer.placement_kind = 2;
                else {
                    ts->set_error("layer('" + module + "'): unknown placement '" + pk + "'");
                    return JS_UNDEFINED;
                }
            } else {
                JS_FreeValue(c, pv);
            }
        }
    }

    layer.physics = get_bool("physics", true);
    layer.embed   = (float)get_float("embed", nullptr);
    get_range("dropHeight", 0.15f, 0.35f, layer.drop_h);
    get_range("scale",      1.0f,  1.0f,  layer.scale_range);
    layer.collider_override = get_str("collider");

    // -- Params: object, function, or absent --
    JSValue params_val = JS_UNDEFINED;
    bool params_is_fn = false;
    bool params_is_obj = false;
    if (!JS_IsUndefined(opts)) {
        params_val = JS_GetPropertyStr(c, opts, "params");
        if (JS_IsFunction(c, params_val))           params_is_fn  = true;
        else if (JS_IsObject(params_val))            params_is_obj = true;
        else { JS_FreeValue(c, params_val); params_val = JS_UNDEFINED; }
    }

    // If params is a static object, stringify and normalize once now.
    std::string static_params_json;
    if (params_is_obj) {
        JSValue js = JS_JSONStringify(c, params_val, JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(js)) {
            size_t len=0; const char* s = JS_ToCStringLen(c, &len, js);
            if (s) {
                // Normalize: sorted keys + %.17g numbers so the composite key
                // matches name2hash's params_to_json bytes.
                static_params_json = normalize_params_json(s, len);
                JS_FreeCString(c,s);
            }
        }
        JS_FreeValue(c, js);
        JS_FreeValue(c, params_val); params_val = JS_UNDEFINED;
        // Validate: the composite key module\x1f params_json must be an explicit entry
        // (no plain-module fallback — an explicit params object must name a declared variant).
        if (!state->has_composite_child_key(module, static_params_json.c_str(),
                                            static_params_json.size())) {
            ts->set_error("layer('" + module + "'): params variant not declared in static requires");
            return JS_UNDEFINED;
        }
    } else if (!params_is_fn) {
        // No params: use empty string to fall back to plain module key.
        static_params_json = "";
    }

    // If no params, pre-resolve the hash once (plain module key).
    uint64_t fixed_hash = 0;
    bool has_fixed_hash = false;
    if (!params_is_fn) {
        has_fixed_hash = state->lookup_child_hash(
            module,
            static_params_json.empty() ? nullptr : static_params_json.c_str(),
            static_params_json.size(), fixed_hash);
        if (!has_fixed_hash) {
            ts->set_error("layer('" + module + "'): undeclared module (add to static requires)");
            JS_FreeValue(c, params_val);
            return JS_UNDEFINED;
        }
    }

    // -- Build the params-fn `r` helper object (shared across all placements) --
    JSValue r_helper = JS_UNDEFINED;
    if (params_is_fn) {
        r_helper = JS_NewObject(c);
        JSValue g = JS_GetGlobalObject(c);
        JSValue fn_int   = JS_GetPropertyStr(c, g, "__dsl_ts_rng_int");
        JSValue fn_float = JS_GetPropertyStr(c, g, "__dsl_ts_rng_float");
        JS_SetPropertyStr(c, r_helper, "int",   fn_int);
        JS_SetPropertyStr(c, r_helper, "float", fn_float);
        JS_FreeValue(c, g);
    }

    // -- Domain generation: 20 domains in fixed order --
    const tileset::TileConfig& cfg = ts->spec.cfg;
    const float w    = cfg.edge_strip_width;
    const float size = cfg.size;
    const float ccr  = cfg.corner_clear_radius;
    const uint64_t master_seed = cfg.seed;
    const uint32_t layer_index = (uint32_t)ts->spec.layers.size();  // index at entry
    tileset::PlacementKind pk = (tileset::PlacementKind)layer.placement_kind;

    // Domain ids: vStrip c0->0, c1->1, hStrip c0->2, c1->3, interior->4+tile
    // orientation 0 = vertical strips, orientation 1 = horizontal strips
    // Vertical strip: across = [-w, +w), along = [0, size). Map: pos={across, y, along}
    // Horizontal strip: across = [-w, +w), along = [0, size). Map: pos={along, y, across}
    // Corner clear disks: at along=0 and along=size (wrap), radius=ccr.

    for (int orient = 0; orient < 2; ++orient) {
        for (int color = 0; color < 2; ++color) {
            uint32_t domain_id = (uint32_t)(orient * 2 + color);
            uint64_t dom_seed = tileset::placement_seed(master_seed, layer_index, domain_id);
            uint64_t attr_seed = dom_seed ^ 0xA5A5A5A5A5A5A5A5ull;

            tileset::PlacementDomain dom;
            dom.x0 = -w; dom.x1 = w;
            dom.z0 = 0.0f; dom.z1 = size;
            dom.clear_disks = { {0.0f, 0.0f}, {0.0f, size} };
            dom.clear_radius = ccr;

            std::vector<tileset::Point2> pts = tileset::scatter(pk, dom, layer.density, dom_seed);

            dsl::Rng attr_rng(attr_seed);
            std::vector<tileset::Placement>& dest = layer.strip[orient][color];
            dest.reserve(pts.size());

            for (const auto& pt : pts) {
                tileset::Placement p{};
                if (!place_one_instance(c, ts, state, module, layer,
                                        params_is_fn, fixed_hash,
                                        params_val, r_helper,
                                        attr_rng, pt, orient, p)) {
                    JS_FreeValue(c, params_val);
                    JS_FreeValue(c, r_helper);
                    return JS_UNDEFINED;
                }
                dest.push_back(p);
            }
        }
    }

    // Interior tiles 0..15 (row*4+col)
    for (int tile = 0; tile < 16; ++tile) {
        uint32_t domain_id = 4u + (uint32_t)tile;
        uint64_t dom_seed = tileset::placement_seed(master_seed, layer_index, domain_id);
        uint64_t attr_seed = dom_seed ^ 0xA5A5A5A5A5A5A5A5ull;

        tileset::PlacementDomain dom;
        dom.x0 = w; dom.x1 = size - w;
        dom.z0 = w; dom.z1 = size - w;
        // No corner disks for interior (edgeStripWidth > cornerClearRadius enforced by tile())
        dom.clear_radius = 0.0f;

        std::vector<tileset::Point2> pts = tileset::scatter(pk, dom, layer.density, dom_seed);

        dsl::Rng attr_rng(attr_seed);
        std::vector<tileset::Placement>& dest = layer.interior[tile];
        dest.reserve(pts.size());

        for (const auto& pt : pts) {
            tileset::Placement p{};
            if (!place_one_instance(c, ts, state, module, layer,
                                    params_is_fn, fixed_hash,
                                    params_val, r_helper,
                                    attr_rng, pt, -1, p)) {
                JS_FreeValue(c, params_val);
                JS_FreeValue(c, r_helper);
                return JS_UNDEFINED;
            }
            dest.push_back(p);
        }
    }

    JS_FreeValue(c, params_val);
    JS_FreeValue(c, r_helper);

    // Push the completed LayerSpec (errors mid-generation leave no partial layer).
    ts->spec.layers.push_back(std::move(layer));
    return JS_UNDEFINED;
}

static JSValue j_ts_dropChild(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (ts->has_error) return JS_UNDEFINED;
    dsl::DslState* state = state_of(c);

    // Get module name (arg 0).
    if (n < 1 || JS_IsUndefined(a[0])) {
        ts->set_error("dropChild: module name required"); return JS_UNDEFINED;
    }
    const char* m = JS_ToCString(c, a[0]);
    if (!m) { ts->set_error("dropChild: module name required"); return JS_UNDEFINED; }
    std::string module(m);
    JS_FreeCString(c, m);

    // Optional params (arg 1) — stringify and normalize like placeChild does.
    std::string params_str;
    if (n > 1 && !JS_IsUndefined(a[1]) && !JS_IsNull(a[1])) {
        JSValue js = JS_JSONStringify(c, a[1], JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(js)) {
            size_t len = 0;
            const char* s = JS_ToCStringLen(c, &len, js);
            if (s) {
                params_str = normalize_params_json(s, len);
                JS_FreeCString(c, s);
            }
        }
        JS_FreeValue(c, js);
    }

    uint64_t hash = 0;
    if (!state->lookup_child_hash(module,
                                  params_str.empty() ? nullptr : params_str.c_str(),
                                  params_str.size(), hash)) {
        ts->set_error("dropChild('" + module + "'): undeclared module (add to static requires)");
        return JS_UNDEFINED;
    }

    tileset::DropChildRec rec{};
    rec.child_hash = hash;
    // Capture current transform stack top as row-major float[16].
    // Use the same matrix_to_row16 logic DslState::placeChild uses by accessing top().
    // We need the row-major layout; replicate the conversion inline here.
    Matrix mm = state->top();
    rec.transform[0]=mm.m0;  rec.transform[1]=mm.m4;  rec.transform[2]=mm.m8;  rec.transform[3]=mm.m12;
    rec.transform[4]=mm.m1;  rec.transform[5]=mm.m5;  rec.transform[6]=mm.m9;  rec.transform[7]=mm.m13;
    rec.transform[8]=mm.m2;  rec.transform[9]=mm.m6;  rec.transform[10]=mm.m10; rec.transform[11]=mm.m14;
    rec.transform[12]=mm.m3; rec.transform[13]=mm.m7; rec.transform[14]=mm.m11; rec.transform[15]=mm.m15;
    ts->spec.drops.push_back(rec);
    return JS_UNDEFINED;
}

static JSValue j_ts_variant(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    tileset::TilesetState* ts = ts_of(c);
    if (!ts) { state_of(c)->set_error("tileset verb outside Tileset root"); return JS_UNDEFINED; }
    if (ts->has_error) return JS_UNDEFINED;
    // variant() may only be called after tile().
    if (!ts->spec.tile_called) {
        ts->set_error("variant(): must be called after tile()");
        return JS_UNDEFINED;
    }
    // variant() may only be called once.
    if (ts->variant_called) {
        ts->set_error("variant(): called more than once");
        return JS_UNDEFINED;
    }
    ts->variant_called = true;
    // Register the hook fn: dup it and store the JSValue as raw bits.
    if (n < 1 || !JS_IsFunction(c, a[0])) {
        ts->set_error("variant(): argument must be a function");
        return JS_UNDEFINED;
    }
    JSValue fn = JS_DupValue(c, a[0]);
    static_assert(sizeof(fn) <= sizeof(ts->variant_fn_bits),
                  "JSValue too large for variant_fn_bits storage");
    std::memcpy(ts->variant_fn_bits, &fn, sizeof(fn));
    ts->variant_fn_set = true;
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// World query verbs: heightAt / slopeAt / moistureAt / biomeAt
// All fail loudly when no world field is bound.
// ---------------------------------------------------------------------------
static JSValue j_heightAt(JSContext* c, JSValueConst, int, JSValueConst* a) {
    DslState* st = state_of(c);
    const WorldBinding& w = st->world();
    if (!w.field) { st->set_error("heightAt: no world field bound"); return JS_UNDEFINED; }
    return JS_NewFloat64(c, w.field->height_at((float)argd(c, a[0]), (float)argd(c, a[1])));
}
static JSValue j_slopeAt(JSContext* c, JSValueConst, int, JSValueConst* a) {
    DslState* st = state_of(c);
    const WorldBinding& w = st->world();
    if (!w.field) { st->set_error("slopeAt: no world field bound"); return JS_UNDEFINED; }
    return JS_NewFloat64(c, w.field->slope_at((float)argd(c, a[0]), (float)argd(c, a[1])));
}
static JSValue j_moistureAt(JSContext* c, JSValueConst, int, JSValueConst* a) {
    DslState* st = state_of(c);
    const WorldBinding& w = st->world();
    if (!w.field) { st->set_error("moistureAt: no world field bound"); return JS_UNDEFINED; }
    return JS_NewFloat64(c, w.field->moisture_at((float)argd(c, a[0]), (float)argd(c, a[1])));
}
static JSValue j_biomeAt(JSContext* c, JSValueConst, int, JSValueConst* a) {
    DslState* st = state_of(c);
    const WorldBinding& w = st->world();
    if (!w.field) { st->set_error("biomeAt: no world field bound"); return JS_UNDEFINED; }
    using B = terrain_field::FieldRuntime;
    const char* s = "meadow";
    switch (w.field->biome_at((float)argd(c, a[0]), (float)argd(c, a[1]))) {
        case B::Ocean:     s = "ocean";     break;
        case B::Meadow:    s = "meadow";    break;
        case B::Foothills: s = "foothills"; break;
        case B::Mountains: s = "mountains"; break;
    }
    return JS_NewString(c, s);
}

// terrainVolume(tx, tz, rung, matArray)
// Meshes one sector of the bound terrain field using native surface-nets and
// pushes the result directly into the triangle buffer. matArray is an array of
// four material IDs [grass, dirt, rock, snow] indexed by the field's material_at.
// Fails loudly if no world binding is installed.
static JSValue j_terrainVolume(JSContext* c, JSValueConst, int n, JSValueConst* a) {
    DslState* st = state_of(c);
    const WorldBinding& w = st->world();
    if (!w.field) {
        st->set_error("terrainVolume: no world bound — set BakeOptions.world before baking a terrain sector");
        return JS_UNDEFINED;
    }
    if (n < 3) { st->set_error("terrainVolume: requires (tx, tz, rung[, mats])"); return JS_UNDEFINED; }

    int64_t tx = 0, tz = 0;
    JS_ToInt64(c, &tx, a[0]);
    JS_ToInt64(c, &tz, a[1]);
    int32_t rung = 0;
    JS_ToInt32(c, &rung, a[2]);

    // Optional material array: up to 4 entries [grass, dirt, rock, snow].
    // Defaults to 0..3 if not supplied.
    uint32_t mat[4] = {0, 1, 2, 3};
    if (n >= 4 && JS_IsArray(a[3])) {
        for (int i = 0; i < 4; ++i) {
            JSValue v = JS_GetPropertyUint32(c, a[3], (uint32_t)i);
            if (!JS_IsUndefined(v)) {
                int32_t m = 0; JS_ToInt32(c, &m, v);
                mat[i] = (uint32_t)m;
            }
            JS_FreeValue(c, v);
        }
    }

    terrain_mesher::SectorMesh mesh;
    std::string err;
    if (!terrain_mesher::mesh_sector(*w.field, tx, tz, rung,
                                      w.sector_size, w.y_min, w.y_max, mesh, err)) {
        st->set_error("terrainVolume: " + err);
        return JS_UNDEFINED;
    }

    // Emit each material bucket via the standard fill→beginShape→vertex→endShape
    // path so the downstream mesh pipeline (AO bake, normal blending) computes
    // shading normals the same way it does for Terrain.js today. The mesher's
    // gradient normals live in SectorMesh for test validation only — no new
    // normal plumbing enters the verb emission path.
    for (const auto& bkt : mesh.buckets) {
        // Map mesher material (0..3 = grass/dirt/rock/snow) to the caller's IDs.
        uint32_t mat_id = bkt.material < 4 ? mat[bkt.material] : mat[0];
        const size_t n_tris = bkt.positions.size() / 9;
        st->fill(mat_id);
        st->beginShape(0 /* triangles */);
        for (size_t t = 0; t < n_tris; ++t) {
            for (int v = 0; v < 3; ++v) {
                st->vertex(bkt.positions[t*9+v*3+0],
                           bkt.positions[t*9+v*3+1],
                           bkt.positions[t*9+v*3+2]);
            }
        }
        st->endShape();
    }
    return JS_UNDEFINED;
}

void install_bindings(JSContext* ctx) {
    JSValue g = JS_GetGlobalObject(ctx);
    auto bind=[&](const char* n, JSCFunction* f, int argc){ JS_SetPropertyStr(ctx,g,n,JS_NewCFunction(ctx,f,n,argc)); };
    bind("__dsl_pushMatrix",j_pushMatrix,0); bind("__dsl_popMatrix",j_popMatrix,0);
    bind("__dsl_translate",j_translate,3);
    bind("__dsl_rotateX",j_rotateX,1); bind("__dsl_rotateY",j_rotateY,1); bind("__dsl_rotateZ",j_rotateZ,1);
    bind("__dsl_scale",j_scale,3); bind("__dsl_applyMatrix",j_applyMatrix,1);
    bind("__dsl_lookAt",j_lookAt,6);
    bind("__dsl_fill",j_fill,1); bind("__dsl_tint",j_tint,4);
    bind("__dsl_beginVoxels",j_beginVoxels,1); bind("__dsl_endVoxels",j_endVoxels,0);
    bind("__dsl_sphere",j_sphere,4); bind("__dsl_box",j_box,6);
    bind("__dsl_op",j_op,1); bind("__dsl_smoothing",j_smoothing,1);
    bind("__dsl_raycast",j_raycast,6);
    bind("__dsl_beginModifier",j_beginModifier,0); bind("__dsl_endModifier",j_endModifier,1);
    bind("__dsl_placeChild",j_placeChild,2);
    bind("__dsl_beginShape",j_beginShape,1); bind("__dsl_vertex",j_vertex,3);
    bind("__dsl_endShape",j_endShape,0); bind("__dsl_line",j_line,8);
    bind("__dsl_capsule",j_capsule,7); bind("__dsl_cylinder",j_cylinder,7);
    bind("__dsl_cone",j_cone,8);
    bind("__dsl_beginContour",j_beginContour,0); bind("__dsl_endContour",j_endContour,0);
    bind("__dsl_joinType",j_joinType,1); bind("__dsl_extrude",j_extrude,1);
    bind("__dsl_position",j_position,0);
    // Terrain verb binding (Task 5: terrainVolume).
    bind("__terrainVolume",j_terrainVolume,4);
    // World query verbs (Task 7: heightAt/slopeAt/moistureAt/biomeAt).
    bind("__heightAt",j_heightAt,2);
    bind("__slopeAt",j_slopeAt,2);
    bind("__moistureAt",j_moistureAt,2);
    bind("__biomeAt",j_biomeAt,2);
    // Tileset verb bindings.
    bind("__dsl_ts_tile",j_ts_tile,5); bind("__dsl_ts_base",j_ts_base,2);
    bind("__dsl_ts_layer",j_ts_layer,2); bind("__dsl_ts_dropChild",j_ts_dropChild,2);
    bind("__dsl_ts_variant",j_ts_variant,1);
    // Params-fn `r` helper natives (draw from ts->param_rng during layer()).
    bind("__dsl_ts_rng_int",j_ts_rng_int,1); bind("__dsl_ts_rng_float",j_ts_rng_float,2);
    // Override Math.random with the seeded draw so authored parts are reproducible.
    JSValue math = JS_GetPropertyStr(ctx, g, "Math");
    if (JS_IsObject(math)) {
        JS_SetPropertyStr(ctx, math, "random", JS_NewCFunction(ctx, j_random, "random", 0));
    }
    JS_FreeValue(ctx, math);
    install_pf_bindings(ctx);
    JS_FreeValue(ctx,g);
}

} // namespace dsl
