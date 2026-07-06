// tileset_dsl_tests: ScriptHost::eval_tileset recording (tile/base/layer/dropChild verbs).
#include "script_host.h"
#include "tileset_spec.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace script_host;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

static const char* kMinimalTileset = R"JS(
class Floor extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 256, seed: 77 });
    this.base((x, z) => 0.01 * x + 0.02 * z, 3);
  }
}
)JS";

static void test_eval_records_tile_and_base(ScriptHost& host) {
    auto r = host.eval_tileset(kMinimalTileset, "{}", BakeOptions{});
    CHECK(r.error.ok, "dsl: minimal tileset evals clean");
    CHECK(r.spec.tile_called, "dsl: tile() recorded");
    CHECK(std::fabs(r.spec.cfg.size - 2.0f) < 1e-6f, "dsl: tile size recorded");
    CHECK(r.spec.cfg.texels_per_meter == 256, "dsl: texelsPerMeter recorded");
    CHECK(r.spec.cfg.seed == 77, "dsl: explicit seed recorded");
    CHECK(std::fabs(r.spec.cfg.edge_strip_width - 0.15f) < 1e-6f, "dsl: default edgeStripWidth");
    const auto& b = r.spec.base;
    CHECK(b.set && b.n == 64, "dsl: base field sampled 64x64");
    CHECK(std::fabs(b.cell - 2.0f / 64.0f) < 1e-6f, "dsl: base cell size");
    CHECK(b.material == 3, "dsl: base material recorded");
    // fn = 0.01x + 0.02z, sample (x=5, z=9):
    float expect = 0.01f * (5 * b.cell) + 0.02f * (9 * b.cell);
    CHECK(std::fabs(b.heights[9 * 64 + 5] - expect) < 1e-5f, "dsl: base heights match fn");
}

static void test_eval_errors(ScriptHost& host) {
    // base() before tile()
    auto r1 = host.eval_tileset(R"JS(
class F extends Tileset {
  build() { this.base((x,z)=>0, 1); }
}
)JS", "{}", BakeOptions{});
    CHECK(!r1.error.ok && r1.error.message.find("before tile") != std::string::npos,
          "dsl: base() before tile() is a structured error");

    // build() that never calls tile()
    auto r2 = host.eval_tileset(R"JS(
class F extends Tileset {
  build() { this.fill(1); }
}
)JS", "{}", BakeOptions{});
    CHECK(!r2.error.ok && r2.error.message.find("tile()") != std::string::npos,
          "dsl: missing tile() is a structured error");

    // Part verbs still work inside a tileset build (inherited API)
    auto r3 = host.eval_tileset(R"JS(
class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 1 });
    this.fill(2);
    this.beginVoxels(0.05);
    this.sphere([1.0, 0.0, 1.0], 0.1);
    this.endVoxels();
  }
}
)JS", "{}", BakeOptions{});
    CHECK(r3.error.ok, "dsl: inherited Part voxel verbs eval clean in tileset");
}

static void test_eval_deterministic(ScriptHost& host) {
    auto a = host.eval_tileset(kMinimalTileset, "{}", BakeOptions{});
    auto b = host.eval_tileset(kMinimalTileset, "{}", BakeOptions{});
    CHECK(a.error.ok && b.error.ok, "dsl: double eval clean");
    CHECK(a.resolved_hash == b.resolved_hash, "dsl: resolved hash stable");
    CHECK(a.spec.base.heights == b.spec.base.heights, "dsl: base samples identical across evals");
}

// C1 regression: params_json override must be folded into the resolved hash;
// two eval_tileset calls with different params_json must produce different hashes.
static void test_params_override_changes_hash(ScriptHost& host) {
    // A tileset with no static params — class extends Tileset (not Part directly),
    // so merge_params_canonical returns "{}" for the static-params step. The
    // params_json override must still be canonicalized and included in the hash.
    static const char* kSrc = R"JS(
class MyFloor extends Tileset {
  build(p) {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 1 });
  }
}
)JS";
    auto r_no_override   = host.eval_tileset(kSrc, "{}",              BakeOptions{});
    auto r_with_override = host.eval_tileset(kSrc, "{\"variant\":1}", BakeOptions{});
    CHECK(r_no_override.error.ok,   "dsl: params-override: no-override eval clean");
    CHECK(r_with_override.error.ok, "dsl: params-override: with-override eval clean");
    CHECK(r_no_override.resolved_hash != r_with_override.resolved_hash,
          "dsl: params-override: different params_json → different resolved_hash (C1 fix)");
}

// M2: error-path tests for tile() argument validation.
static void test_tile_validation_errors(ScriptHost& host) {
    // tile({size:-1}) — negative size must error with a recognizable message.
    auto r1 = host.eval_tileset(R"JS(
class F extends Tileset {
  build() { this.tile({ size: -1.0, texelsPerMeter: 128 }); }
}
)JS", "{}", BakeOptions{});
    CHECK(!r1.error.ok, "dsl: tile({size:-1}) is an error");
    CHECK(r1.error.message.find("size") != std::string::npos,
          "dsl: tile({size:-1}) message mentions 'size'");

    // tile() called twice — second call must error.
    auto r2 = host.eval_tileset(R"JS(
class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 1 });
    this.tile({ size: 3.0, texelsPerMeter: 128, seed: 2 });
  }
}
)JS", "{}", BakeOptions{});
    CHECK(!r2.error.ok, "dsl: tile() called twice is an error");
    CHECK(r2.error.message.find("twice") != std::string::npos ||
          r2.error.message.find("tile") != std::string::npos,
          "dsl: tile() called twice message is recognizable");

    // tile({edgeStripWidth:0.05, cornerClearRadius:0.08}) —
    // edgeStripWidth (0.05) must exceed cornerClearRadius (0.08): should error.
    auto r3 = host.eval_tileset(R"JS(
class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 1,
                edgeStripWidth: 0.05, cornerClearRadius: 0.08 });
  }
}
)JS", "{}", BakeOptions{});
    CHECK(!r3.error.ok, "dsl: tile({edgeStripWidth:0.05, cornerClearRadius:0.08}) is an error");
    CHECK(r3.error.message.find("edgeStrip") != std::string::npos ||
          r3.error.message.find("cornerClear") != std::string::npos,
          "dsl: edgeStripWidth<=cornerClearRadius message is recognizable");
}

// NOTE: eval_tileset in non-module mode (no shared-lib imports) requires plain
// `class X extends Tileset` syntax — not `export default class`. The brief's
// fixtures use `export default` but that requires module mode which is only
// activated by shared-lib imports. All tests here use the classic-script pattern
// matching the other tileset_dsl_tests fixtures.
static const char* kLayeredTileset = R"JS(
class Floor extends Tileset {
  static requires = [
    { module: 'Pebble', params: { seed: 0 } },
    { module: 'Pebble', params: { seed: 1 } },
    { module: 'Twig' },
  ];
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 42 });
    this.base((x, z) => 0.0, 1);

    this.pushMatrix();
    this.translate(1.0, 0.2, 0.6);
    this.dropChild('Twig');
    this.popMatrix();

    this.layer('Pebble', { density: 30, physics: false, embed: 0.3,
                           params: r => ({ seed: r.int(2) }) });
    this.layer('Twig',   { density: 8, physics: true, dropHeight: [0.1, 0.3],
                           scale: [0.7, 1.3], placement: 'poisson' });
  }
}
)JS";

// Child tables: hashes are arbitrary but distinct; modules/params match `requires`.
static const uint64_t kChildHashes[] = { 0x1111, 0x2222, 0x3333 };
static const std::string kChildModules[] = { "Pebble", "Pebble", "Twig" };
static const std::string kChildParams[]  = { R"({"seed":0})", R"({"seed":1})", "{}" };

static void test_layer_recording(ScriptHost& host) {
    auto r = host.eval_tileset(kLayeredTileset, "{}", BakeOptions{},
                               kChildHashes, 3, kChildModules, kChildParams);
    CHECK(r.error.ok, "layer: layered tileset evals clean");
    if (!r.error.ok) return;  // guard: remaining checks dereference into spec
    CHECK(r.spec.layers.size() == 2, "layer: two layers in call order");
    CHECK(r.spec.drops.size() == 1 && r.spec.drops[0].child_hash == 0x3333,
          "layer: dropChild recorded with Twig hash");
    if (!r.spec.drops.empty())
    CHECK(std::fabs(r.spec.drops[0].transform[3] - 1.0f) < 1e-6f ||
          std::fabs(r.spec.drops[0].transform[12] - 1.0f) < 1e-6f,
          "layer: dropChild transform carries translation (row- or col-major slot)");
    if (r.spec.layers.size() < 2) return;  // guard: L0/L1 accesses below

    const auto& L0 = r.spec.layers[0];
    CHECK(L0.module == "Pebble" && !L0.physics && std::fabs(L0.embed - 0.3f) < 1e-6f,
          "layer: pebble opts recorded");
    // 4 strip lists + 16 interiors all populated
    size_t strip_total = 0, interior_total = 0;
    for (int o = 0; o < 2; ++o) for (int c = 0; c < 2; ++c) strip_total += L0.strip[o][c].size();
    for (int t = 0; t < 16; ++t) interior_total += L0.interior[t].size();
    CHECK(strip_total > 0,    "layer: strip placements generated");
    CHECK(interior_total > 0, "layer: interior placements generated");
    // interior expected ~ density*(size-2w)^2 = 30*1.7^2 = 86.7 per tile
    CHECK(L0.interior[0].size() >= 70 && L0.interior[0].size() <= 100,
          "layer: interior count near density*area");

    // params fn resolved to declared variants only
    bool hashes_ok = true;
    for (const auto& p : L0.interior[0])
        if (p.child_hash != 0x1111 && p.child_hash != 0x2222) hashes_ok = false;
    CHECK(hashes_ok, "layer: params fn resolves to declared variant hashes");
    // both variants appear (density is high enough that P(all-same) ~ 2^-86)
    bool saw0 = false, saw1 = false;
    for (const auto& p : L0.interior[0]) { saw0 |= p.child_hash == 0x1111; saw1 |= p.child_hash == 0x2222; }
    CHECK(saw0 && saw1, "layer: both param variants used");

    // physics:false -> yaw-only quats (x,z components zero)
    bool yaw_only = true;
    for (const auto& p : L0.interior[0])
        if (std::fabs(p.quat[0]) > 1e-6f || std::fabs(p.quat[2]) > 1e-6f) yaw_only = false;
    CHECK(yaw_only, "layer: non-physics placements are yaw-only");

    const auto& L1 = r.spec.layers[1];
    CHECK(L1.physics && L1.placement_kind == 1, "layer: twig physics+poisson recorded");
    bool y_in_range = true, scale_in_range = true;
    for (const auto& p : L1.interior[5]) {
        if (p.pos[1] < 0.1f || p.pos[1] > 0.3f) y_in_range = false;
        if (p.scale < 0.7f || p.scale > 1.3f) scale_in_range = false;
    }
    CHECK(y_in_range,     "layer: drop heights within range");
    CHECK(scale_in_range, "layer: scales within range");
    // strip placements stay within the strip domain
    bool strip_in = true;
    for (const auto& p : L1.strip[0][0])
        if (p.pos[0] < -0.15f || p.pos[0] >= 0.15f || p.pos[2] < 0.0f || p.pos[2] >= 2.0f)
            strip_in = false;
    CHECK(strip_in, "layer: vertical strip placements inside strip domain");
}

static void test_layer_determinism(ScriptHost& host) {
    auto a = host.eval_tileset(kLayeredTileset, "{}", BakeOptions{},
                               kChildHashes, 3, kChildModules, kChildParams);
    auto b = host.eval_tileset(kLayeredTileset, "{}", BakeOptions{},
                               kChildHashes, 3, kChildModules, kChildParams);
    CHECK(a.error.ok && b.error.ok, "layer: double eval clean");
    bool same = a.spec.layers.size() == b.spec.layers.size();
    for (size_t l = 0; same && l < a.spec.layers.size(); ++l) {
        const auto& x = a.spec.layers[l]; const auto& y = b.spec.layers[l];
        // Compare all four strip tables (orient 0/1, color 0/1) bit-identically.
        for (int o = 0; same && o < 2; ++o) {
            for (int c = 0; same && c < 2; ++c) {
                same = x.strip[o][c].size() == y.strip[o][c].size();
                for (size_t i = 0; same && i < x.strip[o][c].size(); ++i)
                    same = std::memcmp(&x.strip[o][c][i], &y.strip[o][c][i], sizeof(tileset::Placement)) == 0;
            }
        }
        // Compare all 16 interior tile tables bit-identically.
        for (int t = 0; same && t < 16; ++t) {
            same = x.interior[t].size() == y.interior[t].size();
            for (size_t i = 0; same && i < x.interior[t].size(); ++i)
                same = std::memcmp(&x.interior[t][i], &y.interior[t][i], sizeof(tileset::Placement)) == 0;
        }
    }
    CHECK(same, "layer: placements bit-identical across evals (strips + interior)");
}

static void test_layer_errors(ScriptHost& host) {
    // density missing
    auto r1 = host.eval_tileset(R"JS(
class F extends Tileset {
  static requires = [ { module: 'Twig' } ];
  build() { this.tile({size:2.0, texelsPerMeter:128, seed:1}); this.layer('Twig', {}); }
}
)JS", "{}", BakeOptions{}, &kChildHashes[2], 1, &kChildModules[2], &kChildParams[2]);
    CHECK(!r1.error.ok && r1.error.message.find("density") != std::string::npos,
          "layer: missing density is a structured error");
    // undeclared params variant (static object path)
    auto r2 = host.eval_tileset(R"JS(
class F extends Tileset {
  static requires = [ { module: 'Twig' } ];
  build() { this.tile({size:2.0, texelsPerMeter:128, seed:1});
            this.layer('Twig', { density: 5, params: { seed: 9 } }); }
}
)JS", "{}", BakeOptions{}, &kChildHashes[2], 1, &kChildModules[2], &kChildParams[2]);
    CHECK(!r2.error.ok && r2.error.message.find("variant") != std::string::npos,
          "layer: undeclared params variant is a structured error");

    // params fn returning an undeclared variant must be fail-closed (not silently
    // fall back to the plain-module hash).  'Twig' is declared plain (no params);
    // a fn that returns {seed:9} names a variant that was never declared.
    auto r3 = host.eval_tileset(R"JS(
class F extends Tileset {
  static requires = [ { module: 'Twig' } ];
  build() { this.tile({size:2.0, texelsPerMeter:128, seed:1});
            this.layer('Twig', { density: 5,
                                 params: r => ({ seed: 9 }) }); }
}
)JS", "{}", BakeOptions{}, &kChildHashes[2], 1, &kChildModules[2], &kChildParams[2]);
    CHECK(!r3.error.ok, "layer: params fn undeclared variant is a structured error");
    CHECK(r3.error.message.find("Twig") != std::string::npos &&
          r3.error.message.find("variant") != std::string::npos,
          "layer: params fn undeclared variant error names the module");

    // F8: placement opt present but not a string must fail closed (not silently
    // map to uniform).  placement:42 must produce a structured error.
    auto r4 = host.eval_tileset(R"JS(
class F extends Tileset {
  static requires = [ { module: 'Twig' } ];
  build() { this.tile({size:2.0, texelsPerMeter:128, seed:1});
            this.layer('Twig', { density: 5, placement: 42 }); }
}
)JS", "{}", BakeOptions{}, &kChildHashes[2], 1, &kChildModules[2], &kChildParams[2]);
    CHECK(!r4.error.ok, "layer: non-string placement is a structured error");
    CHECK(!r4.error.message.empty(), "layer: non-string placement error message is non-empty");
}

// ---------------------------------------------------------------------------
// Task 5: variant() verb tests
// ---------------------------------------------------------------------------

static void test_variant(ScriptHost& host) {
    auto r = host.eval_tileset(R"JS(
class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });
    this.variant(t => {
      if (t.index === 5) {
        this.fill(2);
        this.beginVoxels(0.05);
        this.sphere([1.0, 0.0, 1.0], 0.05);   // center tile, well inside margin
        this.endVoxels();
      }
    });
  }
}
)JS", "{}", BakeOptions{});
    CHECK(r.error.ok, "variant: hook evals clean");
    CHECK(r.spec.variant_ranges.size() == 1 && !r.spec.variant_ranges.empty() && r.spec.variant_ranges[0].tile == 5,
          "variant: only tile 5 emitted content");
    if (!r.spec.variant_ranges.empty())
        CHECK(r.spec.variant_ranges[0].op_end > r.spec.variant_ranges[0].op_begin,
              "variant: op range non-empty");
}

static void test_variant_margin(ScriptHost& host) {
    auto r = host.eval_tileset(R"JS(
class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });
    this.variant(t => {
      if (t.index === 2) {
        this.beginVoxels(0.05);
        this.sphere([0.05, 0.0, 1.0], 0.04);   // 0.05 from x=0 bound < 0.15 margin
        this.endVoxels();
      }
    });
  }
}
)JS", "{}", BakeOptions{});
    CHECK(!r.error.ok, "variant: margin violation is an error");
    CHECK(r.error.message.find("tile 2") != std::string::npos,
          "variant: error names the tile");
}

static void test_variant_colors(ScriptHost& host) {
    // Hook observes de Bruijn colors: record them via heights hack? No — throw on mismatch.
    auto r = host.eval_tileset(R"JS(
class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });
    this.variant(t => {
      const ok = [0,1].includes(t.colors.top) && [0,1].includes(t.colors.left);
      if (!ok) throw new Error('bad colors at tile ' + t.index);
      if (t.index === 0 && (t.colors.top !== 0 || t.colors.left !== 0))
        throw new Error('tile 0 colors wrong');
    });
  }
}
)JS", "{}", BakeOptions{});
    CHECK(r.error.ok, "variant: de Bruijn colors passed to hook");
}

// ---------------------------------------------------------------------------
// Review findings: conservative sphere AABB under non-uniform scale (Fix 2)
// ---------------------------------------------------------------------------
// The old diagonal-probe approach for spheres could underestimate the world-space
// extent when the transform has off-diagonal components that route Y into world X.
// The row-norm formula is exact for spheres under any affine M.
//
// Scenario: rotateZ(PI/4) * scale(SQRT2, SQRT2, 1) gives M where
//   M.m0 = 1, M.m4 = -1, M.m8 = 0  (world X row)
//   M.m2 = 0, M.m6 = 0,  M.m10 = 1 (world Z row)
// For a sphere of radius 0.1 at world center (0.26, 0.2, 1.0):
//   true world hx = 0.1 * sqrt(1^2 + (-1)^2 + 0^2) = 0.1 * sqrt(2) ~= 0.1414
//   world xmin = 0.26 - 0.1414 = 0.1186 < 0.15 (strip) => MARGIN VIOLATION
// Old probes sampled (±r, 0, ±r) and (0, ±r, 0): old hx = max(1+0, 1)*0.1 = 0.1
//   old xmin = 0.26 - 0.1 = 0.16 >= 0.15 => PASSES (missed violation).
static void test_variant_sphere_nonuniform_scale_margin(ScriptHost& host) {
    // This tileset should produce a margin violation with the fixed row-norm AABB,
    // but the old diagonal-probe code would have passed it silently.
    // DSL order: translate -> rotateZ(PI/4) -> scale(sqrt(2), sqrt(2), 1) ->
    //   sphere at local origin with r=0.1.
    // M = RotZ(PI/4) * Scale(sqrt2, sqrt2, 1):  m0=1, m4=-1, m8=0 (X-row)
    //   => true hx = 0.1*sqrt(2) ~= 0.1414; world center X = 0.26
    //   => world xmin = 0.1186 < 0.15 (strip) => error expected.
    auto r = host.eval_tileset(R"JS(
class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });
    this.variant(t => {
      if (t.index === 0) {
        this.pushMatrix();
        this.translate(0.26, 0.2, 1.0);
        this.rotateZ(Math.PI / 4);
        this.scale(Math.SQRT2, Math.SQRT2, 1.0);
        this.beginVoxels(0.05);
        this.sphere([0, 0, 0], 0.1);
        this.endVoxels();
        this.popMatrix();
      }
    });
  }
}
)JS", "{}", BakeOptions{});
    CHECK(!r.error.ok, "variant: non-uniform scale sphere margin violation detected (row-norm fix)");
    CHECK(r.error.message.find("tile 0") != std::string::npos,
          "variant: non-uniform scale sphere margin error names tile 0");
}

// ---------------------------------------------------------------------------
// Review findings: variant fn lifetime on early-exit paths (Fix 1)
// ---------------------------------------------------------------------------
// When build() errors AFTER calling variant() (e.g., a DSL verb error after
// tile() and variant()), the duped JSValue should be freed at ts_done and not
// leak. The test verifies the error path is structurally correct (returns an
// error, doesn't crash on context teardown).
static void test_variant_fn_freed_on_build_error(ScriptHost& host) {
    // variant() is registered, then a DSL verb throws an error inside build() itself.
    // The eval should return a structured error, not crash.
    auto r1 = host.eval_tileset(R"JS(
class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });
    this.variant(t => { /* hook registered */ });
    throw new Error('build error after variant');
  }
}
)JS", "{}", BakeOptions{});
    CHECK(!r1.error.ok, "variant fn leak fix: build() throw after variant() returns error");
    CHECK(r1.error.message.find("build error after variant") != std::string::npos ||
          !r1.error.message.empty(),
          "variant fn leak fix: error message is non-empty");

    // variant() registered but tile() DSL error before the loop: variant_fn_set is
    // true but r.error.ok is false when entering the if-block.
    auto r2 = host.eval_tileset(R"JS(
class F extends Tileset {
  build() {
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });
    this.variant(t => { /* hook registered */ });
    this.tile({ size: 2.0, texelsPerMeter: 128, seed: 3 });  /* tile() twice → error */
  }
}
)JS", "{}", BakeOptions{});
    CHECK(!r2.error.ok, "variant fn leak fix: tile() twice after variant() returns error");
    CHECK(r2.error.message.find("twice") != std::string::npos ||
          r2.error.message.find("tile") != std::string::npos,
          "variant fn leak fix: tile-twice error message is recognizable");
}

int main() {
    printf("== tileset_dsl_tests ==\n");
    ScriptHost host;
    test_eval_records_tile_and_base(host);
    test_eval_errors(host);
    test_eval_deterministic(host);
    test_params_override_changes_hash(host);
    test_tile_validation_errors(host);
    test_layer_recording(host);
    test_layer_determinism(host);
    test_layer_errors(host);
    test_variant(host);
    test_variant_margin(host);
    test_variant_colors(host);
    test_variant_sphere_nonuniform_scale_margin(host);
    test_variant_fn_freed_on_build_error(host);
    if (g_failures == 0) printf("PASSED (0 failures)\n");
    else                 printf("FAILED (%d failures)\n", g_failures);
    return g_failures;
}
