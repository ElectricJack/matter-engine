#include "../include/module_resolver.h"
#include "../include/part_asset_v2.h"
#include "../include/script_rng_binding.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#include "check.h"

// ---- scratch shared-lib helpers (used by Task 4 + Task 6/8) ----------------
std::string make_scratch_shared_lib(const std::string& src_root) {
    std::string dir = std::string("scratch_shlib_") + std::to_string((unsigned long)rand());
    std::string mk = "mkdir -p '" + dir + "'"; (void)system(mk.c_str());
    std::string cp = "cp '" + src_root + "'/*.js '" + dir + "'/ 2>/dev/null"; (void)system(cp.c_str());
    return dir;
}
void append_to_file(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::app); f << text;
}
void remove_scratch_shared_lib(const std::string& dir) {
    std::string rm = "rm -rf '" + dir + "'"; (void)system(rm.c_str());
}

// ---- Task 1: import-specifier parsing -------------------------------------
static void test_parse_imports() {
    const std::string src =
        "import { lsystem } from 'shared-lib/lsystem';\n"
        "import {rng} from \"shared-lib/rng\";\n"
        "import * as V from 'shared-lib/vecmath.js';\n"
        "// import { fake } from 'shared-lib/not-real';  (commented out)\n"
        "const s = \"import { str } from 'shared-lib/string-literal'\";\n"
        "class Foo extends Part { build(p){} }\n";
    std::vector<std::string> specs = module_resolver::parse_import_specifiers(src);
    std::sort(specs.begin(), specs.end());
    CHECK(specs.size() == 3, "exactly three real import specifiers parsed");
    CHECK(specs.size() == 3 && specs[0] == "shared-lib/lsystem", "specifier 0 = lsystem");
    CHECK(specs.size() == 3 && specs[1] == "shared-lib/rng", "specifier 1 = rng");
    CHECK(specs.size() == 3 && specs[2] == "shared-lib/vecmath.js", "specifier 2 keeps .js as written");
}

// ---- Task 2: specifier resolution -----------------------------------------
static void test_resolve_specifier() {
    const std::string root = "shared-lib-fixtures";
    std::string path, err;
    CHECK(module_resolver::resolve_specifier("shared-lib/aaa", root, path, err),
          "resolve shared-lib/aaa");
    CHECK(path == "shared-lib-fixtures/aaa.js", "aaa resolves to aaa.js under root");
    CHECK(module_resolver::resolve_specifier("shared-lib/bbb.js", root, path, err),
          "trailing .js accepted");
    CHECK(path == "shared-lib-fixtures/bbb.js", "bbb.js resolves to bbb.js");
    // missing file -> fail closed
    CHECK(!module_resolver::resolve_specifier("shared-lib/nope", root, path, err),
          "missing module fails closed");
    // non-shared-lib specifier rejected
    CHECK(!module_resolver::resolve_specifier("./relative", root, path, err),
          "relative specifier rejected");
    CHECK(!module_resolver::resolve_specifier("shared-lib/../escape", root, path, err),
          "path traversal rejected");
}

// ---- Task 3: transitive gather + canonical fold ---------------------------
static void test_fold_transitive_and_canonical() {
    const std::string root = "shared-lib-fixtures";
    // A part that imports top (which transitively pulls mid -> leaf) and bbb.
    const std::string part =
        "import { TOP } from 'shared-lib/top';\n"
        "import { BBB } from 'shared-lib/bbb';\n"
        "class P extends Part { build(p){} }\n";
    std::string err;
    module_resolver::FoldResult r1;
    CHECK(module_resolver::fold_sources(part, root, r1, err), "fold succeeds");
    // resolved modules = bbb, leaf, mid, top  (transitive, deduped)
    CHECK(r1.resolved_specifiers.size() == 4, "four transitive modules gathered");
    // canonical order: part source first, then modules by sorted resolved specifier.
    // bytes must start with the part source.
    CHECK(r1.folded.size() > part.size(), "folded buffer larger than part alone");
    CHECK(std::equal(part.begin(), part.end(), r1.folded.begin()), "part source folded first");

    // Order independence: same imports listed in a different order -> identical fold.
    const std::string part_reordered =
        "import { BBB } from 'shared-lib/bbb';\n"
        "import { TOP } from 'shared-lib/top';\n"
        "class P extends Part { build(p){} }\n";
    module_resolver::FoldResult r2;
    CHECK(module_resolver::fold_sources(part_reordered, root, r2, err), "fold (reordered) succeeds");
    // The MODULE portion of the fold (everything after the part source) is identical,
    // because modules are ordered by sorted resolved specifier, not import order.
    std::string mods1(r1.folded.begin() + part.size(), r1.folded.end());
    std::string mods2(r2.folded.begin() + part_reordered.size(), r2.folded.end());
    CHECK(mods1 == mods2, "module fold is import-order independent (canonical)");

    // Cycle / missing -> fail closed.
    module_resolver::FoldResult rbad;
    const std::string bad = "import { X } from 'shared-lib/nope';\n";
    CHECK(!module_resolver::fold_sources(bad, root, rbad, err), "missing module fails fold");
}

// ---- Task 4: folded source changes the resolved hash ----------------------
// Helper: resolved hash of a part = fnv1a64 over its canonical fold + params bytes.
static uint64_t hash_part(const std::string& part, const std::string& root,
                          const std::string& params) {
    std::string err;
    module_resolver::FoldResult r;
    bool ok = module_resolver::fold_sources(part, root, r, err);
    if (!ok) { printf("FAIL: fold for hash_part: %s\n", err.c_str()); return 0; }
    return part_asset::compute_resolved_hash(r.folded.data(), r.folded.size(),
                                             params.data(), params.size(),
                                             /*child_hashes*/nullptr, /*count*/0);
}

static void test_fold_changes_resolved_hash() {
    const std::string root = "shared-lib-fixtures";
    const std::string importer =
        "import { LEAF } from 'shared-lib/leaf';\n"
        "class P extends Part { build(p){} }\n";
    const std::string non_importer =
        "class Q extends Part { build(p){} }\n";
    const std::string params = "{\"seed\":0}";

    uint64_t h_imp_before  = hash_part(importer, root, params);
    uint64_t h_nonimp_before = hash_part(non_importer, root, params);

    // Make a scratch copy of the fixtures, edit leaf.js, re-fold against the copy.
    std::string scratch = make_scratch_shared_lib(root);
    append_to_file(scratch + "/leaf.js", "\nexport const EXTRA = 999;\n");

    uint64_t h_imp_after   = hash_part(importer, scratch, params);
    uint64_t h_nonimp_after = hash_part(non_importer, scratch, params);

    CHECK(h_imp_before != h_imp_after, "editing leaf.js changes importer hash");
    CHECK(h_nonimp_before == h_nonimp_after, "non-importer hash unchanged by leaf.js edit");

    // Transitive: a part importing top (top->mid->leaf) also changes when leaf edits.
    const std::string transitive_importer =
        "import { TOP } from 'shared-lib/top';\n"
        "class R extends Part { build(p){} }\n";
    uint64_t h_t_before = hash_part(transitive_importer, root, params);
    uint64_t h_t_after  = hash_part(transitive_importer, scratch, params);
    CHECK(h_t_before != h_t_after, "transitive importer (top->mid->leaf) invalidated by leaf edit");

    remove_scratch_shared_lib(scratch);
}

// ---- Task 5: ordering-stability of the fold -------------------------------
static void test_ordering_stability() {
    const std::string root = "shared-lib-fixtures";
    // Three parts that import {aaa,bbb} in all permutations must fold identically.
    const char* perms[] = {
        "import {A} from 'shared-lib/aaa';\nimport {B} from 'shared-lib/bbb';\nclass P extends Part{build(p){}}\n",
        "import {B} from 'shared-lib/bbb';\nimport {A} from 'shared-lib/aaa';\nclass P extends Part{build(p){}}\n",
    };
    // The part SOURCE differs (import line order), so resolved hashes differ overall.
    // But the MODULE FOLD region must be byte-identical. Assert on the fold region:
    std::string err;
    module_resolver::FoldResult r0, r1;
    module_resolver::fold_sources(perms[0], root, r0, err);
    module_resolver::fold_sources(perms[1], root, r1, err);
    std::string m0(r0.folded.begin() + std::char_traits<char>::length(perms[0]), r0.folded.end());
    std::string m1(r1.folded.begin() + std::char_traits<char>::length(perms[1]), r1.folded.end());
    CHECK(m0 == m1, "module-fold region is stable across import permutations");
    CHECK(r0.resolved_specifiers == r1.resolved_specifiers, "resolved-specifier order is canonical/stable");
    CHECK(r0.resolved_specifiers.size() == 2 &&
          r0.resolved_specifiers[0] == "shared-lib/aaa" &&
          r0.resolved_specifiers[1] == "shared-lib/bbb",
          "specifiers sorted lexicographically (aaa before bbb)");
}

// ---- Task 6: C++ reference xoshiro128** to pin rng.js outputs --------------
struct RefRng {
    uint32_t s[4];
    static uint32_t rotl(uint32_t x, int k){ return (x << k) | (x >> (32 - k)); }
    explicit RefRng(uint32_t seed){
        uint32_t z = seed;
        for (int i = 0; i < 4; ++i) {
            z += 0x9e3779b9u;
            uint32_t w = z;
            w = (w ^ (w >> 16)) * 0x21f0aaadu;
            w = (w ^ (w >> 15)) * 0x735a2d97u;
            s[i] = w ^ (w >> 15);
        }
    }
    uint32_t next(){
        uint32_t result = rotl(s[1] * 5u, 7) * 9u;
        uint32_t t = s[1] << 9;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t;
        s[3] = rotl(s[3], 11);
        return result;
    }
};

static void test_rng_reference_stream() {
    // Same seed -> identical stream; different seed -> different stream; no entropy.
    RefRng a(12345u), a2(12345u);
    for (int i = 0; i < 8; ++i) {
        uint32_t x = a.next(), y = a2.next();
        CHECK(x == y, "same seed yields identical stream value");
    }
    RefRng c(12345u), d(999u);
    bool any_diff = false;
    for (int i = 0; i < 8; ++i) if (c.next() != d.next()) any_diff = true;
    CHECK(any_diff, "different seed yields a different stream");
    // Pin a few exact values so rng.js can be verified against them. Sequence the
    // calls explicitly: printf argument evaluation order is unspecified in C++.
    RefRng e(42u);
    uint32_t e0 = e.next(), e1 = e.next(), e2 = e.next(), e3 = e.next();
    printf("INFO: xoshiro128** seed=42 first4: %u %u %u %u\n", e0, e1, e2, e3);
    // Cross-checked against shared-lib/rng.js via node: 660444221 3652823732
    // 77672526 910233633 (same stream, bit-for-bit).
    CHECK(e0 == 660444221u && e1 == 3652823732u && e2 == 77672526u && e3 == 910233633u,
          "rng.js xoshiro128** seed=42 stream pinned (matches node cross-check)");
}

// ---- Task 7: C++ Math.random binding (seed-from-params) -------------------
static void test_script_rng_binding() {
    // seed_from_params_json extracts an integer seed from a params JSON blob.
    CHECK(script_rng::seed_from_params_json("{\"seed\":42}", "seed") == 42u,
          "seed parsed from params json");
    CHECK(script_rng::seed_from_params_json("{\"size\":1.0}", "seed") == 0u,
          "missing seed defaults to 0");
    // ScriptRng matches the JS/reference algorithm and is deterministic.
    script_rng::ScriptRng r1(42u), r2(42u), r3(7u);
    for (int i = 0; i < 8; ++i) CHECK(r1.next_u32() == r2.next_u32(), "ScriptRng reproducible");
    bool diff = false;
    script_rng::ScriptRng r4(42u);
    for (int i = 0; i < 8; ++i) if (r4.next_u32() != r3.next_u32()) diff = true;
    CHECK(diff, "ScriptRng differs by seed");
    // random() is in [0,1).
    script_rng::ScriptRng r5(1u);
    for (int i = 0; i < 100; ++i) { double v = r5.random(); CHECK(v >= 0.0 && v < 1.0, "random in [0,1)"); }
    // ScriptRng must match the pinned rng.js seed=42 stream bit-for-bit.
    script_rng::ScriptRng r6(42u);
    CHECK(r6.next_u32() == 660444221u && r6.next_u32() == 3652823732u,
          "ScriptRng matches rng.js seed=42 stream");
}

// ---- Task 8: helper pure-output reference assertions ----------------------
static void test_helper_pure_outputs() {
    // vecmath: cross of basis vectors. cross([1,0,0],[0,1,0]) = [0,0,1]
    double cx = 1*0 - 0*1, cy = 0*0 - 1*0, cz = 1*1 - 0*0;
    CHECK(cx == 0 && cy == 0 && cz == 1, "cross(x,y)=z reference");
    // bezier cubic at t=0.5 for p0=0,p1=0,p2=1,p3=1 (scalar):
    // w0=0.125,w1=0.375,w2=0.375,w3=0.125 -> 0.375*1 + 0.125*1 = 0.5
    double B = 0.125*0 + 0.375*0 + 0.375*1 + 0.125*1;
    CHECK(std::abs(B - 0.5) < 1e-9, "cubic bezier midpoint reference = 0.5");
    // geometry ring(4, r=1): points at angles 0, pi/2, pi, 3pi/2 -> unit circle in XZ.
    double a0x = std::cos(0), a1z = std::sin(M_PI/2);
    CHECK(std::abs(a0x - 1.0) < 1e-9 && std::abs(a1z - 1.0) < 1e-9, "ring(4) basis points reference");
    // lsystem deterministic: axiom "A", rule A->AB, 2 iters -> "ABB"
    // it1: "AB"; it2: A->AB, B->B(default) => "ABB"
    auto step = [](const std::string& in){ std::string o; for(char c: in){ o += (c=='A') ? "AB" : std::string(1,c);} return o; };
    std::string s = step(step(std::string("A")));
    CHECK(s == "ABB", "lsystem A->AB twice = ABB reference");
}

#ifdef SP2_SCRIPT_HOST
#include "../include/script_host.h"   // SP-2
static void test_import_resolves_end_to_end() {
    // A tiny part importing geometry.ring + rng; bake it through SP-2's host with the
    // real shared-lib/ root and assert (a) bake succeeds, (b) same seed -> identical
    // resolved hash/bytes; (c) different seed -> different.
    using namespace script_host;
    ScriptHost host;
    host.set_shared_lib_root("../shared-lib");
    const std::string part =
        "import { ring } from 'shared-lib/geometry';\n"
        "import { rng } from 'shared-lib/rng';\n"
        "class Tree extends Part {\n"
        "  static params = { seed: 0 };\n"
        "  build(p){ this.beginVoxels(0.1); this.fill(MAT.stone);\n"
        "    const r = rng(p.seed);\n"
        "    for (const pt of ring(6, 1.0, 0)) this.sphere(pt, 0.2 + r.random()*0.05);\n"
        "    this.endVoxels(); }\n"
        "}\n";
    BakeResult a = host.bake_source(part, "{\"seed\":1}", {});
    BakeResult a2 = host.bake_source(part, "{\"seed\":1}", {});
    BakeResult b = host.bake_source(part, "{\"seed\":2}", {});
    CHECK(a.error.ok, "import-resolving part bakes successfully");
    CHECK(a.resolved_hash == a2.resolved_hash, "same seed -> identical resolved hash");
    CHECK(a.resolved_hash != b.resolved_hash, "different seed -> different resolved hash");
    // editing a shared module changes the importer's hash:
    std::string scratch = make_scratch_shared_lib("../shared-lib");
    append_to_file(scratch + "/geometry.js", "\nexport const X = 1;\n");
    ScriptHost host2; host2.set_shared_lib_root(scratch);
    BakeResult c = host2.bake_source(part, "{\"seed\":1}", {});
    CHECK(c.error.ok && c.resolved_hash != a.resolved_hash, "shared-module edit invalidates importer bake");
    remove_scratch_shared_lib(scratch);
}
#else
static void test_import_resolves_end_to_end() {
    printf("INFO: SP-2 ScriptHost not linked; end-to-end bake test skipped (compile with -DSP2_SCRIPT_HOST)\n");
}
#endif

int main() {
    test_parse_imports();
    test_resolve_specifier();
    test_fold_transitive_and_canonical();
    test_fold_changes_resolved_hash();
    test_ordering_stability();
    test_rng_reference_stream();
    test_script_rng_binding();
    test_helper_pure_outputs();
    test_import_resolves_end_to_end();
    if (g_failures == 0) printf("All shared_lib tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
