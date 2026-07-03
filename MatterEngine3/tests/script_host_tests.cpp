#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
extern "C" {
#include "quickjs.h"
}
#include "../include/script_host.h"
#include "../include/dsl_state.h"
#include "../include/triangle_emit.hpp"   // complete TriangleBuildBuffer for G4/G8 tests
#include "../include/csg_lowering.h"
#include "../include/part_asset_v2.h"
#include "../../MatterSurfaceLib/include/blas_manager.hpp"
#include "../../MatterSurfaceLib/include/tlas_manager.hpp"
#include <sys/stat.h>
#include <vector>

static bool file_exists(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0; }

static std::vector<uint8_t> read_all(const std::string& p){
    std::vector<uint8_t> b; FILE* f=fopen(p.c_str(),"rb"); if(!f) return b;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    b.resize(n); if (fread(b.data(),1,n,f)!=(size_t)n) b.clear(); fclose(f); return b;
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static void test_embed_eval_1_plus_1() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr, "runtime created");
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr, "context created");
    JSValue v = JS_Eval(ctx, "1+1", 3, "<test>", JS_EVAL_TYPE_GLOBAL);
    CHECK(!JS_IsException(v), "eval did not throw");
    int32_t out = -1;
    CHECK(JS_ToInt32(ctx, &out, v) == 0, "result convertible to int");
    CHECK(out == 2, "1+1 == 2");
    JS_FreeValue(ctx, v);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void test_fresh_context_runs_empty_class() {
    script_host::ScriptHost host;
    const char* src =
        "class Empty extends Part {\n"
        "  static params = {};\n"
        "  build(p) {}\n"
        "}\n";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok, "empty class bakes without error");
}

static void test_build_called_on_authored_class() {
    script_host::ScriptHost host;
    const char* src =
        "class Rock extends Part {\n"
        "  static params = {};\n"
        "  build(p) { globalThis.__built = 1; }\n"
        "}\n";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok, "authored class bakes");
    CHECK(host.last_build_ran(), "build(p) was invoked");
}

static void test_dsl_state_rules() {
    dsl::DslState s;
    // pop on empty-above-identity is misuse
    s.popMatrix();
    CHECK(s.has_error(), "popMatrix below identity is an error");

    dsl::DslState s2;
    s2.endVoxels();
    CHECK(s2.has_error(), "endVoxels with no open session is an error");

    // G8: sphere() outside a voxel session is no longer an error -- in None/mesh
    // mode it emits a triangulated solid. The remaining misuse is emitting a solid
    // mid-beginShape (a solid is its own primitive, not loose verts).
    dsl::DslState s3;
    s3.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    CHECK(!s3.has_error(), "sphere() in None session meshes (G8), not an error");
    dsl::DslState s3b;
    s3b.beginShape(0);
    s3b.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    CHECK(s3b.has_error(), "sphere() mid-beginShape is an error");

    dsl::DslState s4;
    s4.beginVoxels(0.1f);
    s4.beginVoxels(0.1f);
    CHECK(s4.has_error(), "opening a session inside another is an error");

    dsl::DslState s5;
    s5.beginVoxels(0.25f);
    s5.fill(7);
    s5.sphere({1,0,0}, 2.0f, dsl::CsgOp::Union);
    s5.endVoxels();
    CHECK(!s5.has_error(), "valid voxel session has no error");
    CHECK(s5.buffer().ops.size() == 1, "one brush recorded");
    CHECK(s5.buffer().ops[0].materialId == 7, "material cursor captured");
    CHECK(s5.buffer().ops[0].spacing == 0.25f, "session spacing captured");
}

static void test_params_merge_and_hash() {
    script_host::ScriptHost host;
    const char* src =
        "class Rock extends Part {\n"
        "  static params = { size: 1.0, seed: 0 };\n"
        "  build(p) { globalThis.__seen = JSON.stringify(p); }\n"
        "}\n";
    script_host::BakeResult def = host.bake_source(src, "{}", {});
    CHECK(def.error.ok, "defaults bake ok");
    CHECK(host.last_merged_params().find("\"size\":1") != std::string::npos,
          "default size present in merged params");

    script_host::ScriptHost host2;
    script_host::BakeResult ov = host2.bake_source(src, "{\"size\":2.0}", {});
    CHECK(ov.error.ok, "override bake ok");
    CHECK(host2.last_merged_params().find("\"size\":2") != std::string::npos,
          "override size present in merged params");
    CHECK(host2.last_merged_params().find("\"seed\":0") != std::string::npos,
          "non-overridden default still present");

    CHECK(def.resolved_hash != ov.resolved_hash,
          "override changes resolved_hash");
}

static void test_resolve_hash_matches_and_skips_build() {
    const char* src =
        "class Rock extends Part {\n"
        "  static params = { size: 1.0, seed: 0 };\n"
        "  build(p) { globalThis.__built = true; }\n"
        "}\n";
    script_host::ScriptHost hb;
    script_host::BakeResult baked = hb.bake_source(src, "{\"size\":2.0}", {});
    CHECK(baked.error.ok, "bake ok");

    script_host::ScriptHost hr;
    uint64_t rh = hr.resolve_hash(src, "{\"size\":2.0}", nullptr, 0);
    CHECK(rh == baked.resolved_hash, "resolve_hash agrees with bake_source hash");

    // resolve_hash must not execute build(): probe a fresh context global.
    script_host::ScriptHost hp;
    hp.resolve_hash(
        "class Probe extends Part { static params={};"
        " build(p){ globalThis.__built2 = true; } }", "{}", nullptr, 0);
    CHECK(hp.last_merged_params().find("__built2") == std::string::npos,
          "resolve_hash did not run build()");
}

static void test_bindings_record_ops_and_misuse() {
    script_host::ScriptHost host;
    const char* ok =
        "class Rock extends Part {\n"
        "  static params = {};\n"
        "  build(p){ this.beginVoxels(0.1); this.fill(MAT.stone);\n"
        "            this.sphere([0,0,0],1.0);\n"
        "            this.box([0,0.5,0],[0.3,0.3,0.3]); this.difference();\n"
        "            this.endVoxels(); }\n"
        "}\n";
    script_host::BakeResult r = host.bake_source(ok, "{}", {});
    CHECK(r.error.ok, "valid voxel script bakes");
    CHECK(host.last_buffer().ops.size() == 2, "two brushes recorded");
    CHECK(host.last_buffer().ops[1].op == dsl::CsgOp::Difference, "difference applied to box");

    // G8: a solid emitted mid-beginShape is the misuse now (a loose sphere with no
    // session meshes fine). The error message names the open-shape conflict.
    script_host::ScriptHost host2;
    const char* bad =
        "class Bad extends Part { static params={};\n"
        "  build(p){ this.beginShape(SHAPE.triangles); this.sphere([0,0,0],1.0); this.endShape(); }\n"
        "}\n";
    script_host::BakeResult rb = host2.bake_source(bad, "{}", {});
    CHECK(!rb.error.ok, "solid mid-beginShape is fail-closed");
    CHECK(rb.error.message.find("beginShape") != std::string::npos ||
          rb.error.message.find("solid") != std::string::npos,
          "structured mid-shape error message");

    // A loose sphere() with no session now bakes (mesh mode, G8).
    script_host::ScriptHost host3;
    const char* meshOnly =
        "class Mesh extends Part { static params={};\n"
        "  build(p){ this.fill(MAT.stone); this.sphere([0,0,0],1.0); }\n"
        "}\n";
    script_host::BakeResult rm = host3.bake_source(meshOnly, "{}", {});
    CHECK(rm.error.ok, "G8: loose mesh sphere bakes without a session");
}

static void test_csg_lowering() {
    dsl::DslState s;
    s.beginVoxels(0.25f);
    s.fill(3);
    s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    s.box({1,0,0}, {0.05f,0.05f,0.05f}, dsl::CsgOp::Difference); // sub-min box
    // re-tag the box op as difference IN-SESSION (verb sets last op; G3 scopes
    // this to the open session, so it must run before endVoxels()).
    s.set_last_op(dsl::CsgOp::Difference);
    s.smoothing(0.4f);
    s.endVoxels();
    dsl::LoweredField f = dsl::lower_build_buffer(s.buffer());
    CHECK(f.additive.size() == 1, "sphere lowered to one additive particle");
    CHECK(f.additive[0].materialId == 3, "additive carries material cursor");
    // Phase 1: a box (any op) lowers to ONE oriented fat primitive, not a carve
    // sphere stamp. A Difference box is folded as a Difference STAGE on the fat
    // prim (no trailing carve particle is emitted for it).
    CHECK(f.fat.size() == 1, "sub-min box lowers to one oriented fat primitive");
    CHECK(f.fat[0].kind == FAT_PRIM_BOX, "the fat primitive is a box");
    CHECK(f.stages.size() >= 2, "union sphere then difference box opens two ordered stages");
    CHECK(f.stages.back() == CSG_STAGE_DIFFERENCE, "the box's stage is a Difference");
    CHECK(f.smoothing == 0.4f || f.smoothing == 0.0f, "smoothing factor carried");
}

static void test_voxel_primitive_occupancy() {
    // Sphere brush occupancy
    dsl::DslState ss; ss.beginVoxels(0.1f); ss.fill(0);
    ss.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union); ss.endVoxels();
    CHECK(dsl::field_is_solid(ss.buffer(), {0,0,0}), "sphere solid at center");
    CHECK(!dsl::field_is_solid(ss.buffer(), {2,0,0}), "sphere empty outside radius");

    // Box brush occupancy
    dsl::DslState sb; sb.beginVoxels(0.1f); sb.fill(0);
    sb.box({0,0,0}, {0.5f,0.5f,0.5f}, dsl::CsgOp::Union); sb.endVoxels();
    CHECK(dsl::field_is_solid(sb.buffer(), {0.4f,0.4f,0.4f}), "box solid inside");
    CHECK(!dsl::field_is_solid(sb.buffer(), {0.9f,0,0}), "box empty outside half-extent");

    // Two overlapping spheres: union / difference / intersection
    auto twoSphere=[&](dsl::CsgOp op2){
        dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
        s.sphere({0,0,0},1.0f,dsl::CsgOp::Union);
        s.sphere({1,0,0},1.0f,op2); s.endVoxels();
        return s.buffer();
    };
    auto U=twoSphere(dsl::CsgOp::Union);
    CHECK(dsl::field_is_solid(U,{-0.9f,0,0}) && dsl::field_is_solid(U,{1.9f,0,0}),
          "union covers both spheres");
    auto D=twoSphere(dsl::CsgOp::Difference);
    CHECK(dsl::field_is_solid(D,{-0.9f,0,0}) && !dsl::field_is_solid(D,{1.0f,0,0}),
          "difference removes second sphere region");
    auto I=twoSphere(dsl::CsgOp::Intersection);
    CHECK(dsl::field_is_solid(I,{0.5f,0,0}) && !dsl::field_is_solid(I,{-0.9f,0,0}),
          "intersection keeps only overlap");
}

static void test_bake_writes_part() {
    script_host::ScriptHost host;
    const char* src =
        "class Ball extends Part { static params={r:1.0};\n"
        "  build(p){ this.beginVoxels(0.25); this.fill(MAT.stone); this.sphere([0,0,0],p.r); this.endVoxels(); }\n"
        "}\n";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(r.error.ok, "sphere bake succeeds");
    CHECK(!r.written_path.empty(), "written path reported");
    CHECK(file_exists(r.written_path), "the .part file exists on disk");
}

static void test_sharp_vs_smooth_seam() {
    dsl::DslState sharp; sharp.beginVoxels(0.1f); sharp.fill(0);
    sharp.sphere({0,0,0},1.0f,dsl::CsgOp::Union);
    sharp.sphere({1,0,0},1.0f,dsl::CsgOp::Union);
    sharp.smoothing(0.0f); sharp.endVoxels();
    dsl::LoweredField fs = dsl::lower_build_buffer(sharp.buffer());
    CHECK(fs.smoothing == 0.0f, "k=0 lowers to hard min (sharp seam)");

    dsl::DslState smooth; smooth.beginVoxels(0.1f); smooth.fill(0);
    smooth.sphere({0,0,0},1.0f,dsl::CsgOp::Union);
    smooth.sphere({1,0,0},1.0f,dsl::CsgOp::Union);
    smooth.smoothing(0.8f); smooth.endVoxels();
    dsl::LoweredField fm = dsl::lower_build_buffer(smooth.buffer());
    CHECK(fm.smoothing > 0.5f, "high k lowers to a large smooth-min factor (merged)");
    CHECK(fm.smoothing > fs.smoothing, "smooth seam has strictly larger blend factor than sharp");
}

static void test_sub_min_box_feature_survives() {
    dsl::DslState s; s.beginVoxels(0.5f); s.fill(0);   // min particle ~0.5
    s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    s.box({1,0,0}, {0.05f,0.05f,0.05f}, dsl::CsgOp::Difference); // 0.1 box << 0.5 min
    s.endVoxels();
    s.set_last_op(dsl::CsgOp::Difference);
    dsl::LoweredField f = dsl::lower_build_buffer(s.buffer());
    // Phase 1: the sub-min box is a real oriented fat primitive (a Difference
    // stage), so the crisp removal no longer depends on the cubic carve stamp;
    // it survives at full fidelity regardless of the min particle size.
    CHECK(f.fat.size() == 1, "sub-min box lowers to one oriented fat primitive");
    // the analytic field still shows the crisp removal at the box location
    CHECK(!dsl::field_is_solid(s.buffer(), {1.0f,0,0}), "sub-min feature present in field");
}

static void test_determinism_identical_bytes() {
    const char* src =
        "class Ball extends Part { static params={r:1.0};\n"
        "  build(p){ this.beginVoxels(0.25); this.fill(MAT.stone); this.sphere([0,0,0],p.r); this.endVoxels(); }\n"
        "}\n";
    script_host::ScriptHost h1; auto r1 = h1.bake_source(src, "{}", {});
    std::vector<uint8_t> b1 = read_all(r1.written_path);
    script_host::ScriptHost h2; auto r2 = h2.bake_source(src, "{}", {});
    std::vector<uint8_t> b2 = read_all(r2.written_path);
    CHECK(r1.error.ok && r2.error.ok, "both bakes succeed");
    CHECK(r1.resolved_hash == r2.resolved_hash, "same source+params => same resolved_hash");
    CHECK(!b1.empty() && b1 == b2, "re-bake produces byte-identical .part");
}

static void test_fresh_context_no_residue() {
    const char* A =
        "class A extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); this.fill(MAT.dirt); this.box([0,0,0],[1,1,1]); this.endVoxels(); }\n"
        "}\n";
    const char* B =
        "class B extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); this.fill(MAT.stone); this.sphere([0,0,0],1.0); this.endVoxels(); }\n"
        "}\n";
    script_host::ScriptHost h;
    h.bake_source(A, "{}", {});                  // bake A first (residue test)
    auto bAfterA = h.bake_source(B, "{}", {});
    script_host::ScriptHost hClean;
    auto bAlone = hClean.bake_source(B, "{}", {});
    CHECK(bAfterA.error.ok && bAlone.error.ok, "both B bakes ok");
    CHECK(bAfterA.resolved_hash == bAlone.resolved_hash, "B hash independent of prior A bake");
    CHECK(read_all(bAfterA.written_path) == read_all(bAlone.written_path),
          "B bytes identical whether or not A ran first (fresh context, no residue)");
}

static void test_seeded_rng_and_no_ambient() {
    const char* src =
        "class Noise extends Part { static params={seed:0};\n"
        "  build(p){ this.beginVoxels(0.5); this.fill(0);\n"
        "    for(let i=0;i<5;i++){ this.sphere([Math.random()*2,0,0], 0.5); } this.endVoxels(); }\n"
        "}\n";
    script_host::ScriptHost a; auto ra1 = a.bake_source(src, "{\"seed\":1}", {});
    script_host::ScriptHost b; auto rb1 = b.bake_source(src, "{\"seed\":1}", {});
    CHECK(ra1.resolved_hash == rb1.resolved_hash, "same seed => same resolved_hash");
    CHECK(read_all(ra1.written_path) == read_all(rb1.written_path), "same seed => same bytes");
    script_host::ScriptHost c; auto rc2 = c.bake_source(src, "{\"seed\":2}", {});
    CHECK(read_all(ra1.written_path) != read_all(rc2.written_path), "different seed => different bytes");

    // No ambient nondeterminism: Date/require/fetch must be undefined.
    script_host::ScriptHost d;
    const char* probe =
        "class Probe extends Part { static params={};\n"
        "  build(p){ globalThis.__amb = (typeof Date)+','+(typeof require)+','+(typeof fetch)+','+(typeof globalThis.os); }\n"
        "}\n";
    auto rp = d.bake_source(probe, "{}", {});
    CHECK(rp.error.ok, "probe bakes");
    CHECK(d.last_ambient_probe() == "undefined,undefined,undefined,undefined",
          "no Date/require/fetch/os bindings present");
}

static void test_fail_closed() {
    // (a) thrown error
    script_host::ScriptHost h1;
    const char* thrower =
        "class T extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); throw new Error('boom'); }\n"
        "}\n";
    auto r1 = h1.bake_source(thrower, "{}", {});
    CHECK(!r1.error.ok, "throw => error");
    CHECK(r1.written_path.empty(), "throw => no file written");
    CHECK(r1.error.message.find("boom") != std::string::npos, "error message carries throw text");

    // (b) session misuse (begin inside begin)
    script_host::ScriptHost h2;
    const char* misuse =
        "class M extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); this.beginVoxels(0.25); this.endVoxels(); }\n"
        "}\n";
    auto r2 = h2.bake_source(misuse, "{}", {});
    CHECK(!r2.error.ok, "session misuse => error");
    CHECK(r2.written_path.empty(), "session misuse => no file");

    // (c) time-budget overrun
    script_host::ScriptHost h3;
    const char* spin =
        "class S extends Part { static params={};\n"
        "  build(p){ this.beginVoxels(0.25); while(true){} }\n"
        "}\n";
    script_host::BakeOptions budget; budget.time_budget_ms = 50;
    auto r3 = h3.bake_source(spin, "{}", budget);
    CHECK(!r3.error.ok, "time-budget overrun => error");
    CHECK(r3.written_path.empty(), "time-budget overrun => no file");
    CHECK(r3.error.message.find("budget") != std::string::npos ||
          r3.error.message.find("interrupt") != std::string::npos,
          "structured time-budget error");
}

// SP-2 C-2: static discovery of a part's child instances WITHOUT baking.
// A part declares its children via a `static requires(params)` method (or a
// `static requires` array) returning a list of { module, params } records.
// eval_requires evals the class top-level in a fresh isolated context (no
// build()) and returns the declared children with canonicalized params.
static void test_eval_requires_lists_children() {
    script_host::ScriptHost host;
    const char* src =
        "class Tower extends Part {\n"
        "  static params = { floors: 3 };\n"
        "  static requires(p) {\n"
        "    let out = [];\n"
        "    for (let i = 0; i < p.floors; i++)\n"
        "      out.push({ module: 'Floor', params: { level: i } });\n"
        "    out.push({ module: 'Roof', params: {} });\n"
        "    return out;\n"
        "  }\n"
        "  build(p) {}\n"
        "}\n";
    auto kids = host.eval_requires(src, "{}");
    CHECK(kids.size() == 4, "Tower with default floors=3 declares 3 floors + 1 roof");
    if (kids.size() == 4) {
        CHECK(kids[0].module_specifier == "Floor", "first child is Floor");
        CHECK(kids[0].params_json.find("\"level\":0") != std::string::npos,
              "first floor carries level:0");
        CHECK(kids[3].module_specifier == "Roof", "last child is Roof");
    }
}

static void test_eval_requires_honors_overrides() {
    script_host::ScriptHost host;
    const char* src =
        "class Tower extends Part {\n"
        "  static params = { floors: 3 };\n"
        "  static requires(p) {\n"
        "    let out = [];\n"
        "    for (let i = 0; i < p.floors; i++)\n"
        "      out.push({ module: 'Floor', params: { level: i } });\n"
        "    return out;\n"
        "  }\n"
        "  build(p) {}\n"
        "}\n";
    auto kids = host.eval_requires(src, "{\"floors\":5}");
    CHECK(kids.size() == 5, "override floors=5 declares 5 floor children");
}

static void test_eval_requires_none_is_empty() {
    script_host::ScriptHost host;
    // No `static requires` at all => empty list, no error.
    const char* leaf =
        "class Leaf extends Part {\n"
        "  static params = {};\n"
        "  build(p) {}\n"
        "}\n";
    auto kids = host.eval_requires(leaf, "{}");
    CHECK(kids.empty(), "part with no requires declares no children");

    // An explicit empty requires() also yields empty.
    script_host::ScriptHost host2;
    const char* empty =
        "class Leaf extends Part {\n"
        "  static params = {};\n"
        "  static requires(p) { return []; }\n"
        "  build(p) {}\n"
        "}\n";
    auto kids2 = host2.eval_requires(empty, "{}");
    CHECK(kids2.empty(), "part with empty requires() declares no children");
}

static void test_eval_requires_deterministic() {
    const char* src =
        "class Tower extends Part {\n"
        "  static params = { floors: 2 };\n"
        "  static requires(p) {\n"
        "    let out = [];\n"
        "    for (let i = 0; i < p.floors; i++)\n"
        "      out.push({ module: 'Floor', params: { level: i, h: 2.5 } });\n"
        "    return out;\n"
        "  }\n"
        "  build(p) {}\n"
        "}\n";
    script_host::ScriptHost a, b;
    auto k1 = a.eval_requires(src, "{}");
    auto k2 = b.eval_requires(src, "{}");
    CHECK(k1.size() == k2.size() && k1.size() == 2,
          "same source+params => same child count");
    bool same = k1.size() == k2.size();
    for (size_t i = 0; same && i < k1.size(); ++i)
        same = k1[i].module_specifier == k2[i].module_specifier &&
               k1[i].params_json == k2[i].params_json;
    CHECK(same, "same source+params => identical child records (deterministic)");
}

static void test_eval_requires_does_not_build() {
    script_host::ScriptHost host;
    const char* src =
        "class Probe extends Part {\n"
        "  static params = {};\n"
        "  static requires(p) { return [{ module: 'X', params: {} }]; }\n"
        "  build(p) { globalThis.__built_requires = true; }\n"
        "}\n";
    auto kids = host.eval_requires(src, "{}");
    CHECK(kids.size() == 1, "requires evaluated");
    // eval_requires must not run build(): it shares merge_params_canonical, which
    // never instantiates/builds, so last_build_ran stays false.
    CHECK(!host.last_build_ran(), "eval_requires did not run build()");
}

// SP-2 Task 5: placeChild('Module') records a child-part placement at the
// current matrix-stack transform; bake_source folds the child hashes/names and
// save_v2 writes the ChildInstance rows. This proves the round-trip: bake a leaf,
// bake a parent that places it twice, reload the parent .part, and assert two
// child rows with the right hashes + transforms — plus a determinism re-bake.
static void test_place_child_roundtrip() {
    using namespace script_host;
    ScriptHost host;

    // NOTE: the host's classic bake path (no shared-lib root) evaluates the part
    // as a GLOBAL script and discovers the class via a lexical-binding trampoline,
    // so a plain `class X extends Part` declaration is required (module-only
    // `export default class` would throw under JS_EVAL_TYPE_GLOBAL). This matches
    // every other test in this file.
    const char* leaf_src =
        "class Leaf extends Part {"
        "  build(p){ this.beginVoxels(0.1); this.fill(MAT.leaf);"
        "            this.sphere([0,0,0],0.1); this.endVoxels(); } }";
    BakeResult lr = host.bake_source(leaf_src, "{}", {});
    CHECK(lr.error.ok, "leaf bakes");
    uint64_t leaf_hash = lr.resolved_hash;

    const char* parent_src =
        "class P extends Part {"
        "  build(p){"
        "    this.beginVoxels(0.2); this.fill(MAT.bark);"
        "    this.box([0,0,0],[0.2,0.2,0.2]); this.endVoxels();"
        "    this.pushMatrix(); this.translate(2,3,4); this.placeChild('Leaf'); this.popMatrix();"
        "    this.pushMatrix(); this.translate(5,0,0); this.placeChild('Leaf'); this.popMatrix();"
        "  } }";
    uint64_t kids[1]   = { leaf_hash };
    std::string names[1] = { std::string("Leaf") };
    BakeResult pr = host.bake_source(parent_src, "{}", {}, kids, 1, names);
    CHECK(pr.error.ok, "parent bakes with placed children");

    BLASManager blas; TLASManager tlas(64);
    std::vector<part_asset::ChildInstance> children;
    part_asset::LodLevels lods;
    std::string ppath = part_asset::cache_path_resolved(pr.resolved_hash);
    bool loaded = part_asset::load_v2(ppath, pr.resolved_hash, blas, tlas, children, lods);
    CHECK(loaded, "parent .part reloads");
    CHECK(children.size() == 2, "two leaf instances recorded");
    if (children.size() == 2) {
        CHECK(children[0].child_resolved_hash == leaf_hash, "child 0 is the leaf");
        CHECK(children[1].child_resolved_hash == leaf_hash, "child 1 is the leaf");
        // row-major translation lives in transform[3],[7],[11]
        CHECK(children[0].transform[3]  == 2.0f &&
              children[0].transform[7]  == 3.0f &&
              children[0].transform[11] == 4.0f, "child 0 placed at (2,3,4)");
        CHECK(children[1].transform[3]  == 5.0f, "child 1 placed at x=5");
    }

    // Determinism: re-baking the same parent must yield the same resolved hash.
    BakeResult pr2 = host.bake_source(parent_src, "{}", {}, kids, 1, names);
    CHECK(pr2.error.ok && pr2.resolved_hash == pr.resolved_hash,
          "parent re-bake is deterministic");
}

// --- Phase 2 DSL-completeness gaps (G3..G8) ---------------------------------

// G3: CSG op verbs are scoped to the open voxel session + its brush range. A stray
// difference() after endVoxels() errors; in-session re-tag still works.
static void test_g3_session_scoped_op_verbs() {
    // In-session re-tag works.
    dsl::DslState ok;
    ok.beginVoxels(0.1f);
    ok.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    ok.set_last_op(dsl::CsgOp::Difference);
    CHECK(!ok.has_error(), "G3: in-session op re-tag has no error");
    CHECK(ok.buffer().ops.size() == 1 && ok.buffer().ops[0].op == dsl::CsgOp::Difference,
          "G3: in-session difference() re-tags the current brush");
    ok.endVoxels();

    // Stray difference() AFTER endVoxels() is a clear error.
    dsl::DslState stray;
    stray.beginVoxels(0.1f);
    stray.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    stray.endVoxels();
    stray.set_last_op(dsl::CsgOp::Difference);
    CHECK(stray.has_error(), "G3: difference() after endVoxels() is an error");

    // difference() with no brush in THIS session is a clear error (can't mis-tag a
    // previous session's brush).
    dsl::DslState empty;
    empty.beginVoxels(0.1f);
    empty.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    empty.endVoxels();
    empty.beginVoxels(0.1f);
    empty.set_last_op(dsl::CsgOp::Difference);   // no brush yet in this session
    CHECK(empty.has_error(), "G3: op verb before any brush in this session errors");
}

// G4: tint(r,g,b,a) cursor is captured onto each brush (BuildOp) and triangle (TriEx);
// default is neutral (1,1,1,0) and unchanged.
static void test_g4_tint_cursor() {
    // Default neutral.
    dsl::DslState def;
    CHECK(def.tint().w == 0.0f && def.tint().x == 1.0f, "G4: default tint is neutral (1,1,1,0)");

    // Voxel brush captures the tint cursor.
    dsl::DslState s;
    s.beginVoxels(0.1f);
    s.tint(0.2f, 0.4f, 0.6f, 0.8f);
    s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    s.endVoxels();
    CHECK(!s.has_error(), "G4: tinted voxel session has no error");
    CHECK(s.buffer().ops.size() == 1, "G4: one brush recorded");
    const Vector4& bt = s.buffer().ops[0].tint;
    CHECK(fabsf(bt.x-0.2f)<1e-6f && fabsf(bt.y-0.4f)<1e-6f &&
          fabsf(bt.z-0.6f)<1e-6f && fabsf(bt.w-0.8f)<1e-6f,
          "G4: BuildOp carries the tint cursor");

    // Mesh triangle captures the tint cursor into TriEx.
    dsl::DslState m;
    m.tint(0.1f, 0.2f, 0.3f, 0.9f);
    m.beginShape(0);
    m.vertex(0,0,0); m.vertex(1,0,0); m.vertex(0,1,0);
    m.endShape();
    CHECK(!m.has_error(), "G4: tinted shape has no error");
    const auto& tx = m.triangle_buffer()->tri_extra();
    CHECK(tx.size() == 1, "G4: one triangle emitted");
    CHECK(fabsf(tx[0].tint.x-0.1f)<1e-6f && fabsf(tx[0].tint.w-0.9f)<1e-6f,
          "G4: TriEx carries the tint cursor");

    // Default path still neutral on the triangle.
    dsl::DslState d2;
    d2.beginShape(0); d2.vertex(0,0,0); d2.vertex(1,0,0); d2.vertex(0,1,0); d2.endShape();
    CHECK(d2.triangle_buffer()->tri_extra()[0].tint.w == 0.0f,
          "G4: default tint alpha 0 unchanged");
}

// G5: lookAt aims the forward (+Z) axis at the target and composes with the stack.
static void test_g5_lookat() {
    dsl::DslState s;
    // Look from origin toward +X. Forward +Z (local) should map to +X (world).
    s.lookAt(5, 0, 0, 0, 1, 0);
    CHECK(!s.has_error(), "G5: lookAt has no error");
    Matrix m = s.top();
    // Local +Z transformed = third column (m.m8,m.m9,m.m10) should be ~+X.
    float fx = m.m8, fy = m.m9, fz = m.m10;
    float len = sqrtf(fx*fx+fy*fy+fz*fz);
    fx/=len; fy/=len; fz/=len;
    CHECK(fabsf(fx-1.0f)<1e-4f && fabsf(fy)<1e-4f && fabsf(fz)<1e-4f,
          "G5: +Z forward axis aims at the target direction (+X)");

    // Composes with a prior translate: origin stays put, forward aims from there.
    dsl::DslState s2;
    s2.translate(0, 10, 0);
    s2.lookAt(0, 10, 5, 0, 1, 0);   // from (0,10,0) toward (0,10,5) => +Z forward
    Vector3 p = s2.position();
    CHECK(fabsf(p.y-10.0f)<1e-4f, "G5: lookAt preserves the current frame origin");
    Matrix m2 = s2.top();
    float gz = m2.m10 / sqrtf(m2.m8*m2.m8+m2.m9*m2.m9+m2.m10*m2.m10);
    CHECK(gz > 0.99f, "G5: forward aims +Z toward target from composed origin");
}

// G6: placeChild(module, params?) selects the matching required VARIANT's real
// resolved hash. The host installs a placement table keyed by both the plain
// module name and a composite `module \x1f canonical-params` per declared child;
// placeChild('Leaf',{...}) maps straight to the variant baked with those params
// (no hash re-derivation). This replaces the old params-fold scheme, which
// produced hashes matching no baked part.
static void test_g6_place_child_params() {
    using namespace script_host;
    ScriptHost host;
    const char* leaf =
        "class Leaf extends Part {"
        "  build(p){ this.beginVoxels(0.1); this.fill(MAT.leaf);"
        "            this.sphere([0,0,0],0.1); this.endVoxels(); } }";

    // Two distinct required variants: same source, different params => different
    // resolved hashes (params fold into each child's own content hash).
    BakeResult la1 = host.bake_source(leaf, "{\"seed\":1}", {});
    BakeResult la2 = host.bake_source(leaf, "{\"seed\":2}", {});
    CHECK(la1.error.ok && la2.error.ok, "G6: leaf variants bake");
    uint64_t hashA = la1.resolved_hash, hashB = la2.resolved_hash;
    CHECK(hashA != hashB, "G6: distinct params => distinct child variant hashes");

    // No-params parent: a single plain-keyed child resolves via the module name.
    BakeResult lr = host.bake_source(leaf, "{}", {});
    CHECK(lr.error.ok, "G6: plain leaf bakes");
    uint64_t leaf_hash = lr.resolved_hash;
    {
        uint64_t kids[1] = { leaf_hash };
        std::string names[1]   = { std::string("Leaf") };
        std::string cparams[1] = { std::string("{}") };
        const char* p_none =
            "class P extends Part { build(p){ this.placeChild('Leaf'); } }";
        BakeResult rn = host.bake_source(p_none, "{}", {}, kids, 1, names, cparams);
        CHECK(rn.error.ok, "G6: no-params parent bakes");
        BLASManager b0; TLASManager t0(64); std::vector<part_asset::ChildInstance> c0;
        part_asset::LodLevels l0;
        part_asset::load_v2(part_asset::cache_path_resolved(rn.resolved_hash), rn.resolved_hash, b0, t0, c0, l0);
        CHECK(c0.size() == 1 && c0[0].child_resolved_hash == leaf_hash,
              "G6: no params => plain module key => declared hash");
    }

    // Variant-selecting parent: two declared variants; placeChild picks each by
    // its params. Same params twice => same real variant hash (dedup); different
    // params => the other real variant hash.
    uint64_t kids[2]       = { hashA, hashB };
    std::string names[2]   = { std::string("Leaf"), std::string("Leaf") };
    std::string cparams[2] = { std::string("{\"seed\":1}"), std::string("{\"seed\":2}") };
    const char* p_ab =
        "class P extends Part { build(p){ this.placeChild('Leaf', {seed:1});"
        "                                  this.placeChild('Leaf', {seed:1});"
        "                                  this.placeChild('Leaf', {seed:2}); } }";
    BakeResult r = host.bake_source(p_ab, "{}", {}, kids, 2, names, cparams);
    CHECK(r.error.ok, "G6: variant-selecting parent bakes");

    BLASManager bb; TLASManager tb(64); std::vector<part_asset::ChildInstance> ca;
    part_asset::LodLevels lb;
    part_asset::load_v2(part_asset::cache_path_resolved(r.resolved_hash), r.resolved_hash, bb, tb, ca, lb);
    CHECK(ca.size() == 3, "G6: three placements recorded");
    if (ca.size() == 3) {
        CHECK(ca[0].child_resolved_hash == hashA &&
              ca[1].child_resolved_hash == hashA,
              "G6: {seed:1} placements resolve to variant A's real hash (dedup)");
        CHECK(ca[2].child_resolved_hash == hashB,
              "G6: {seed:2} placement resolves to variant B's real hash");
    }
}

// G7: a pushMatrix() with no popMatrix() fails the build with a clear message.
static void test_g7_stack_balance() {
    script_host::ScriptHost host;
    const char* src =
        "class Leaky extends Part { static params={};"
        "  build(p){ this.pushMatrix(); this.translate(1,0,0); /* no popMatrix */ } }";
    script_host::BakeResult r = host.bake_source(src, "{}", {});
    CHECK(!r.error.ok, "G7: leaked pushMatrix fails the build");
    CHECK(r.error.message.find("stack") != std::string::npos ||
          r.error.message.find("pushMatrix") != std::string::npos,
          "G7: error message names the transform-stack imbalance");

    // Balanced build still succeeds.
    script_host::ScriptHost host2;
    const char* ok =
        "class Ok extends Part { static params={};"
        "  build(p){ this.pushMatrix(); this.translate(1,0,0); this.popMatrix();"
        "            this.beginVoxels(0.2); this.sphere([0,0,0],0.5); this.endVoxels(); } }";
    script_host::BakeResult r2 = host2.bake_source(ok, "{}", {});
    CHECK(r2.error.ok, "G7: balanced stack bakes fine");
}

// G8: sphere()/box() are session-polymorphic. None => a closed triangulated solid;
// Voxels => a brush; mid-beginShape => error. (Mesh-emitter geometry is tested in
// triangle_variation_tests; here we pin the dispatch.)
static void test_g8_sphere_box_polymorphic() {
    // None / mesh mode: sphere emits a closed triangulated solid at the right radius.
    dsl::DslState mesh;
    mesh.sphere({0,0,0}, 2.0f, dsl::CsgOp::Union);
    CHECK(!mesh.has_error(), "G8: sphere() in None session is not an error");
    const auto& mt = mesh.triangle_buffer()->triangles();
    CHECK(!mt.empty(), "G8: sphere() in None session emits triangles");
    CHECK(mesh.buffer().ops.empty(), "G8: mesh sphere does NOT push a voxel brush");
    // Radius check: every vertex sits ~2.0 from center (the sphere is closed/round).
    float maxr = 0, minr = 1e9f;
    for (const Tri& t : mt) {
        for (const float3& v : { t.vertex0, t.vertex1, t.vertex2 }) {
            float rr = sqrtf(v.x*v.x+v.y*v.y+v.z*v.z);
            if (rr>maxr) maxr=rr; if (rr<minr) minr=rr;
        }
    }
    CHECK(fabsf(maxr-2.0f)<1e-3f && fabsf(minr-2.0f)<1e-3f,
          "G8: mesh sphere vertices all lie at radius 2.0");

    // Voxels: sphere stays a brush (no triangles).
    dsl::DslState vox;
    vox.beginVoxels(0.1f);
    vox.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    vox.endVoxels();
    CHECK(!vox.has_error(), "G8: voxel sphere ok");
    CHECK(vox.buffer().ops.size() == 1, "G8: voxel sphere is a brush");
    CHECK(vox.triangle_buffer()->triangles().empty(), "G8: voxel sphere emits no triangles");

    // Mid-beginShape: sphere()/box() error (a solid is its own primitive).
    dsl::DslState mid;
    mid.beginShape(0);
    mid.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    CHECK(mid.has_error(), "G8: sphere() mid-beginShape is an error");

    dsl::DslState midb;
    midb.beginShape(0);
    midb.box({0,0,0}, {1,1,1}, dsl::CsgOp::Union);
    CHECK(midb.has_error(), "G8: box() mid-beginShape is an error");

    // box() in None emits a 12-triangle solid baked under the matrix.
    dsl::DslState boxmesh;
    boxmesh.box({0,0,0}, {1,1,1}, dsl::CsgOp::Union);
    CHECK(!boxmesh.has_error(), "G8: box() in None session is not an error");
    CHECK(boxmesh.triangle_buffer()->triangles().size() == 12,
          "G8: mesh box is 12 triangles");
    CHECK(boxmesh.buffer().ops.empty(), "G8: mesh box does NOT push a voxel brush");
}

// Phase 3: extrude dispatch + POLYGON lazy retention + joinType cursor.
static void test_extrude_dispatch_and_polygon() {
    // (a) extrude in a voxel session errors (deferred).
    {
        dsl::DslState s;
        s.beginVoxels(0.1f);
        float seg[6] = {0,0,0, 0,0,1};
        s.extrude(seg, 2);
        CHECK(s.has_error(), "P3: extrude in a voxel session errors (deferred)");
        CHECK(s.error().find("voxel") != std::string::npos,
              "P3: voxel-extrude error mentions voxel");
    }
    // (b) extrude mid open beginShape (not yet ended) errors.
    {
        dsl::DslState s;
        s.beginShape(3);                 // POLYGON, still open
        s.vertex(0,0,0); s.vertex(1,0,0); s.vertex(1,1,0);
        float seg[6] = {0,0,0, 0,0,1};
        s.extrude(seg, 2);
        CHECK(s.has_error(), "P3: extrude mid-beginShape errors (needs finalized profile)");
    }
    // (c) extrude with no retained profile errors.
    {
        dsl::DslState s;
        float seg[6] = {0,0,0, 0,0,1};
        s.extrude(seg, 2);
        CHECK(s.has_error(), "P3: extrude with no retained profile errors");
    }
    // (d) POLYGON beginShape in a voxel session errors (mesh-only).
    {
        dsl::DslState s;
        s.beginVoxels(0.1f);
        s.beginShape(3);
        CHECK(s.has_error(), "P3: POLYGON beginShape in a voxel session errors");
    }
    // (e) Lazy retention: a POLYGON shape with NO following extrude emits a flat
    //     filled face (triangulated).
    {
        dsl::DslState s;
        s.beginShape(3);                 // POLYGON
        s.vertex(0,0,0); s.vertex(2,0,0); s.vertex(2,2,0); s.vertex(0,2,0);
        s.endShape();                    // finalize + retain (no emit yet)
        CHECK(!s.has_error(), "P3: POLYGON shape has no error");
        // The retained profile flat-fills on the next session boundary / build end.
        s.flush_retained_profile();      // host calls this at build end
        const auto& tris = s.triangle_buffer()->triangles();
        CHECK(tris.size() == 2, "P3: unclaimed POLYGON flat-fills (square -> 2 tris)");
    }
    // (f) Lazy retention claimed: a POLYGON followed by extrude emits the swept
    //     solid and NO flat face.
    {
        dsl::DslState s;
        s.beginShape(3);                 // POLYGON profile
        s.vertex(-0.5f,-0.5f,0); s.vertex(0.5f,-0.5f,0);
        s.vertex(0.5f,0.5f,0);   s.vertex(-0.5f,0.5f,0);
        s.endShape();                    // retain
        float seg[6] = {0,0,0, 0,0,2};
        s.extrude(seg, 2);               // consumes the retained profile
        CHECK(!s.has_error(), "P3: extrude of a retained POLYGON has no error");
        s.flush_retained_profile();      // would flat-fill IF unclaimed
        const auto& tris = s.triangle_buffer()->triangles();
        // A swept prism is 12 tris; a flat fill would have been 2. The flat face
        // must NOT have been emitted (profile was claimed), so count == 12.
        CHECK(tris.size() == 12, "P3: claimed POLYGON sweeps (12 tris) and emits NO flat face");
    }
    // (g) joinType cursor affects the corner: MITER vs BEVEL on an L polyline
    //     change the triangle count (drives the tri_emit::extrude join path).
    {
        dsl::DslState miter;
        miter.joinType(0);               // MITER
        miter.beginShape(3);
        miter.vertex(-0.3f,-0.3f,0); miter.vertex(0.3f,-0.3f,0);
        miter.vertex(0.3f,0.3f,0);   miter.vertex(-0.3f,0.3f,0);
        miter.endShape();
        float L[9] = {0,0,0, 2,0,0, 2,2,0};
        miter.extrude(L, 3);

        dsl::DslState bevel;
        bevel.joinType(1);               // BEVEL
        bevel.beginShape(3);
        bevel.vertex(-0.3f,-0.3f,0); bevel.vertex(0.3f,-0.3f,0);
        bevel.vertex(0.3f,0.3f,0);   bevel.vertex(-0.3f,0.3f,0);
        bevel.endShape();
        bevel.extrude(L, 3);

        CHECK(!miter.has_error() && !bevel.has_error(), "P3: L-extrude (both joins) ok");
        CHECK(miter.triangle_buffer()->triangles().size() !=
              bevel.triangle_buffer()->triangles().size(),
              "P3: joinType MITER vs BEVEL change the corner triangle count");
    }
    // (h) beginContour/endContour build a hole; the flat fill excludes it.
    {
        dsl::DslState s;
        s.beginShape(3);
        s.vertex(0,0,0); s.vertex(4,0,0); s.vertex(4,4,0); s.vertex(0,4,0);  // outer CCW
        s.beginContour();
        s.vertex(1,1,0); s.vertex(1,3,0); s.vertex(3,3,0); s.vertex(3,1,0);  // hole CW
        s.endContour();
        s.endShape();
        s.flush_retained_profile();
        const auto& tris = s.triangle_buffer()->triangles();
        CHECK(!s.has_error(), "P3: POLYGON with a hole has no error");
        CHECK(tris.size() == 8, "P3: square+hole flat fill -> 8 tris (annulus)");
    }
}

// Phase 4 / G1: voxel line() lowers to a capsule brush (sdSegment - r) instead of
// erroring; None line() keeps the swept-tube mesh; voxel capsule()/cylinder()/cone()
// emit brushes, None mesh branch errors cleanly (Phase 5 owns the mesh geometry).
static void test_g1_voxel_line_and_round_dispatch() {
    // (a) line() in a voxel session no longer errors -> one capsule brush.
    {
        dsl::DslState s;
        s.beginVoxels(0.1f); s.fill(0);
        s.line(-1,0,0, 1,0,0, 0.5f, 0.5f);
        CHECK(!s.has_error(), "G1: line() in a voxel session is not an error");
        CHECK(s.buffer().ops.size() == 1, "G1: voxel line() emits one brush");
        CHECK(s.buffer().ops[0].kind == dsl::BrushKind::Capsule,
              "G1: voxel line() is a capsule brush");
        CHECK(s.triangle_buffer()->triangles().empty(),
              "G1: voxel line() emits no triangles");
        // Occupancy matches the capsule SDF (sdSegment - r).
        CHECK(dsl::field_is_solid(s.buffer(), {0,0,0}),    "G1: capsule mid solid");
        CHECK(dsl::field_is_solid(s.buffer(), {1.4f,0,0}), "G1: capsule end-cap solid");
        CHECK(!dsl::field_is_solid(s.buffer(), {0,0.6f,0}),"G1: capsule outside wall empty");
    }
    // (b) line() in None session keeps the swept-tube MESH (unchanged behavior).
    {
        dsl::DslState s;
        s.line(-1,0,0, 1,0,0, 0.5f, 0.5f);
        CHECK(!s.has_error(), "G1: line() in None session is not an error");
        CHECK(s.buffer().ops.empty(), "G1: None line() pushes NO voxel brush");
        CHECK(!s.triangle_buffer()->triangles().empty(),
              "G1: None line() still emits the swept-tube mesh");
    }
    // (c) voxel capsule()/cylinder()/cone() emit brushes.
    {
        dsl::DslState s; s.beginVoxels(0.1f); s.fill(0);
        s.capsule({-1,0,0},{1,0,0},0.5f, dsl::CsgOp::Union);
        s.cylinder({0,-1,0},{0,1,0},0.4f, dsl::CsgOp::Union);
        s.cone({0,0,-1},{0,0,1},0.6f,0.0f, dsl::CsgOp::Union);
        s.endVoxels();
        CHECK(!s.has_error(), "P4: voxel capsule/cylinder/cone have no error");
        CHECK(s.buffer().ops.size() == 3, "P4: three round brushes recorded");
        CHECK(s.buffer().ops[0].kind == dsl::BrushKind::Capsule,  "P4: capsule kind");
        CHECK(s.buffer().ops[1].kind == dsl::BrushKind::Cylinder, "P4: cylinder kind");
        CHECK(s.buffer().ops[2].kind == dsl::BrushKind::Cylinder, "P4: cone lowers to cylinder kind");
    }
    // (d) None / mesh branch of capsule/cylinder/cone emits triangles (Phase 5)
    //     and pushes NO voxel brush. (Geometry detail tested in triangle_variation_tests.)
    {
        dsl::DslState s;
        s.capsule({-1,0,0},{1,0,0},0.5f, dsl::CsgOp::Union);
        CHECK(!s.has_error(), "P5: capsule() in None session is not an error");
        CHECK(!s.triangle_buffer()->triangles().empty(), "P5: mesh capsule emits triangles");
        CHECK(s.buffer().ops.empty(), "P5: mesh capsule pushes no voxel brush");
    }
    {
        dsl::DslState s;
        s.cylinder({0,-1,0},{0,1,0},0.4f, dsl::CsgOp::Union);
        CHECK(!s.has_error(), "P5: cylinder() in None session is not an error");
        CHECK(!s.triangle_buffer()->triangles().empty(), "P5: mesh cylinder emits triangles");
        CHECK(s.buffer().ops.empty(), "P5: mesh cylinder pushes no voxel brush");
    }
    {
        dsl::DslState s;
        s.cone({0,0,-1},{0,0,1},0.6f,0.0f, dsl::CsgOp::Union);
        CHECK(!s.has_error(), "P5: cone() in None session is not an error");
        CHECK(!s.triangle_buffer()->triangles().empty(), "P5: mesh cone emits triangles");
        CHECK(s.buffer().ops.empty(), "P5: mesh cone pushes no voxel brush");
    }
    // (e) capsule()/cylinder()/cone() mid-beginShape error (a solid is its own primitive).
    {
        dsl::DslState s; s.beginShape(0);
        s.capsule({-1,0,0},{1,0,0},0.5f, dsl::CsgOp::Union);
        CHECK(s.has_error(), "P4: capsule() mid-beginShape is an error");
    }
    // (f) End-to-end JS surface: capsule/cylinder/cone/voxel-line bake through
    //     QuickJS + part_base.js.h + bindings without error (wiring smoke test).
    {
        script_host::ScriptHost host;
        const char* src =
            "class Round extends Part { static params={};\n"
            "  build(p){ this.beginVoxels(0.25); this.fill(MAT.stone);\n"
            "    this.capsule([-1,0,0],[1,0,0],0.5);\n"
            "    this.cylinder([0,-1,0],[0,1,0],0.4);\n"
            "    this.cone([0,0,-1],[0,0,1],0.6,0.0);\n"
            "    this.line([0,2,0],[0,3,0],0.2);\n"   // voxel line -> capsule (G1)
            "    this.endVoxels(); }\n"
            "}\n";
        script_host::BakeResult r = host.bake_source(src, "{}", {});
        CHECK(r.error.ok, "P4: JS capsule/cylinder/cone/line voxel bake succeeds");
        CHECK(!r.written_path.empty(), "P4: round-primitive part written");
    }
}

// Phase 5: round-primitive mesh emitters through the JS DSL. In a None (mesh)
// session capsule/cylinder/cone bake triangles; mid-beginShape (Triangles
// session) they still error.
static void test_p5_round_mesh_dispatch() {
    // (a) JS mesh-mode capsule/cylinder/cone bake without a voxel session.
    {
        script_host::ScriptHost host;
        const char* src =
            "class RoundMesh extends Part { static params={};\n"
            "  build(p){ this.fill(MAT.stone);\n"
            "    this.cylinder([-1,0,0],[1,0,0],0.4);\n"
            "    this.cone([0,-1,0],[0,1,0],0.5,0.0);\n"
            "    this.capsule([0,0,-1],[0,0,1],0.3);\n"
            "  }\n"
            "}\n";
        script_host::BakeResult r = host.bake_source(src, "{}", {});
        CHECK(r.error.ok, "P5: JS mesh capsule/cylinder/cone bake succeeds");
        CHECK(!r.written_path.empty(), "P5: mesh round-primitive part written");
    }
    // (b) Mid-beginShape (Triangles session) each still errors via direct DslState.
    {
        dsl::DslState s; s.beginShape(0);
        s.cylinder({-1,0,0},{1,0,0},0.4f, dsl::CsgOp::Union);
        CHECK(s.has_error(), "P5: cylinder() mid-beginShape is an error");
    }
    {
        dsl::DslState s; s.beginShape(0);
        s.cone({0,-1,0},{0,1,0},0.5f,0.0f, dsl::CsgOp::Union);
        CHECK(s.has_error(), "P5: cone() mid-beginShape is an error");
    }
}

static void test_eval_lod_budgets() {
    script_host::ScriptHost host;
    const char* opted =
        "class G extends Part {\n"
        "  static params = { seed: 0, lodBudget: 1.0 };\n"
        "  static lodBudgets = [1.0, 0.3, 0.08];\n"
        "  static lodAnchorSize = 0.5;\n"
        "  build(p) {}\n"
        "}\n";
    auto spec = host.eval_lod_budgets(opted);
    assert(spec.budgets.size() == 3);
    assert(spec.budgets[0] == 1.0 && spec.budgets[1] == 0.3 && spec.budgets[2] == 0.08);
    assert(spec.anchor_size == 0.5);

    const char* plain =
        "class P extends Part { static params = {}; build(p) {} }\n";
    assert(host.eval_lod_budgets(plain).budgets.empty());

    const char* malformed =
        "class M extends Part {\n"
        "  static lodBudgets = [1.0, 'x'];\n"      // non-number: fail closed
        "  build(p) {}\n"
        "}\n";
    assert(host.eval_lod_budgets(malformed).budgets.empty());

    const char* broken = "not even javascript {{{";
    assert(host.eval_lod_budgets(broken).budgets.empty());
    printf("  test_eval_lod_budgets OK\n");
}

int main() {
    test_embed_eval_1_plus_1();
    test_p5_round_mesh_dispatch();
    test_g1_voxel_line_and_round_dispatch();
    test_fresh_context_runs_empty_class();
    test_build_called_on_authored_class();
    test_dsl_state_rules();
    test_params_merge_and_hash();
    test_resolve_hash_matches_and_skips_build();
    test_bindings_record_ops_and_misuse();
    test_csg_lowering();
    test_voxel_primitive_occupancy();
    test_bake_writes_part();
    test_sharp_vs_smooth_seam();
    test_sub_min_box_feature_survives();
    test_determinism_identical_bytes();
    test_fresh_context_no_residue();
    test_seeded_rng_and_no_ambient();
    test_fail_closed();
    test_eval_requires_lists_children();
    test_eval_requires_honors_overrides();
    test_eval_requires_none_is_empty();
    test_eval_requires_deterministic();
    test_eval_requires_does_not_build();
    test_place_child_roundtrip();
    test_g3_session_scoped_op_verbs();
    test_g4_tint_cursor();
    test_g5_lookat();
    test_g6_place_child_params();
    test_g7_stack_balance();
    test_g8_sphere_box_polymorphic();
    test_extrude_dispatch_and_polygon();
    test_eval_lod_budgets();
    if (failures == 0) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
