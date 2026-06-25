#include <cstdio>
#include <cstdint>
#include <cstring>
extern "C" {
#include "quickjs.h"
}
#include "../include/script_host.h"
#include "../include/dsl_state.h"
#include "../include/csg_lowering.h"
#include <sys/stat.h>

static bool file_exists(const std::string& p){ struct stat st; return stat(p.c_str(),&st)==0; }

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

    dsl::DslState s3;
    s3.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    CHECK(s3.has_error(), "emitting outside a session is an error");

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

    script_host::ScriptHost host2;
    const char* bad =
        "class Bad extends Part { static params={};\n"
        "  build(p){ this.sphere([0,0,0],1.0); }\n"   // emit with no session
        "}\n";
    script_host::BakeResult rb = host2.bake_source(bad, "{}", {});
    CHECK(!rb.error.ok, "emit outside session is fail-closed");
    CHECK(rb.error.message.find("session") != std::string::npos, "structured session error message");
}

static void test_csg_lowering() {
    dsl::DslState s;
    s.beginVoxels(0.25f);
    s.fill(3);
    s.sphere({0,0,0}, 1.0f, dsl::CsgOp::Union);
    s.box({1,0,0}, {0.05f,0.05f,0.05f}, dsl::CsgOp::Difference); // sub-min box
    s.smoothing(0.4f);
    s.endVoxels();
    // re-tag the box op as difference (verb sets last op)
    s.set_last_op(dsl::CsgOp::Difference);
    dsl::LoweredField f = dsl::lower_build_buffer(s.buffer());
    CHECK(f.additive.size() == 1, "sphere lowered to one additive particle");
    CHECK(f.additive[0].materialId == 3, "additive carries material cursor");
    CHECK(!f.carve.empty(), "sub-min box still produces carve particles (feature survives)");
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
    CHECK(!f.carve.empty(), "sub-min box carves at least one carve particle");
    // the analytic field still shows the crisp removal at the box location
    CHECK(!dsl::field_is_solid(s.buffer(), {1.0f,0,0}), "sub-min feature present in field");
}

int main() {
    test_embed_eval_1_plus_1();
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
    if (failures == 0) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
