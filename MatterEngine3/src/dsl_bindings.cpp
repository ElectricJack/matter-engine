#include "../include/dsl_state.h"
#include "../include/dsl_bindings.h"
#include <vector>
extern "C" {
#include "quickjs.h"
}

namespace dsl {

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
static JSValue j_simplify(JSContext* c, JSValueConst, int, JSValueConst* a){ state_of(c)->set_simplify((float)argd(c,a[0])); return JS_UNDEFINED; }
static JSValue j_placeChild(JSContext* c, JSValueConst, int n, JSValueConst* a){
    const char* m = JS_ToCString(c, a[0]);
    if (!m) return JS_UNDEFINED;
    // G6: optional params (a plain JS object/array) -> canonical JSON bytes folded
    // into the child's resolved hash so parametric children dedup. JSON.stringify
    // gives a deterministic byte stream for the content-addressed key. No params
    // (undefined/null) = today's behavior (declared hash unchanged).
    if (n > 1 && !JS_IsUndefined(a[1]) && !JS_IsNull(a[1])) {
        JSValue js = JS_JSONStringify(c, a[1], JS_UNDEFINED, JS_UNDEFINED);
        if (!JS_IsException(js)) {
            size_t len = 0;
            const char* s = JS_ToCStringLen(c, &len, js);
            if (s) { state_of(c)->placeChild(m, s, len); JS_FreeCString(c, s); }
            else   { state_of(c)->placeChild(m); }
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
    bind("__dsl_simplify",j_simplify,1);
    bind("__dsl_placeChild",j_placeChild,2);
    bind("__dsl_beginShape",j_beginShape,1); bind("__dsl_vertex",j_vertex,3);
    bind("__dsl_endShape",j_endShape,0); bind("__dsl_line",j_line,8);
    bind("__dsl_capsule",j_capsule,7); bind("__dsl_cylinder",j_cylinder,7);
    bind("__dsl_cone",j_cone,8);
    bind("__dsl_beginContour",j_beginContour,0); bind("__dsl_endContour",j_endContour,0);
    bind("__dsl_joinType",j_joinType,1); bind("__dsl_extrude",j_extrude,1);
    bind("__dsl_position",j_position,0);
    // Override Math.random with the seeded draw so authored parts are reproducible.
    JSValue math = JS_GetPropertyStr(ctx, g, "Math");
    if (JS_IsObject(math)) {
        JS_SetPropertyStr(ctx, math, "random", JS_NewCFunction(ctx, j_random, "random", 0));
    }
    JS_FreeValue(ctx, math);
    JS_FreeValue(ctx,g);
}

} // namespace dsl
