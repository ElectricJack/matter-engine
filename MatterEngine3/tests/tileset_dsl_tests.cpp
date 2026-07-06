// tileset_dsl_tests: ScriptHost::eval_tileset recording (tile/base verbs).
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

int main() {
    printf("== tileset_dsl_tests ==\n");
    ScriptHost host;
    test_eval_records_tile_and_base(host);
    test_eval_errors(host);
    test_eval_deterministic(host);
    if (g_failures == 0) printf("PASSED (0 failures)\n");
    else                 printf("FAILED (%d failures)\n", g_failures);
    return g_failures;
}
