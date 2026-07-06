# Ground Tileset Physics Core (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The deterministic physics core for the ground tileset bake: vendored box3d, de Bruijn torus layout math, collider auto-fit, and a toroidal settle world with portal-synchronized shared bodies and layered orchestration — validated entirely by headless CPU tests (no rendering).

**Architecture:** Three new MatterEngine3 modules: `tileset_layout` (pure functions: 4×4 de Bruijn torus, edge colors, strip occurrences), `tileset_collider` (PCA/OBB auto-fit of part meshes to box3d shape descriptors), `tileset_settle` (box3d world wrapper: static base heightfield, toroidal XZ wrap, portal-sync groups, layered settle loop, snap + micro-relax). Spec: `docs/superpowers/specs/2026-07-05-ground-tileset-bake-design.md` (sections "Wang Tiling & the De Bruijn Torus" and "Physics: box3d Integration").

**Tech Stack:** C++17 (g++, MatterEngine3 conventions), box3d v0.1.0 vendored at `Libraries/box3d` (C17, MIT), SplitMix64 rng (`MatterEngine3/include/dsl_rng.h`), plain-Makefile test suites run by `build-all.sh test`.

## Global Constraints

- box3d pinned at tag **v0.1.0** (github.com/erincatto/box3d); compile with `-std=c17 -O2 -ffp-contract=off` (the fp-contract flag is REQUIRED for box3d's cross-platform determinism); link `-lm`.
- Every box3d def struct MUST come from its `b3DefaultXxxDef()` function (zero-initialized defs fail an internal cookie assert).
- NEVER call `b3Body_EnableSleep` at runtime (upstream issue #4: world lock leak). Set `enableSleep` on the body def instead.
- NEVER call `b3Body_SetMassData` (upstream issue #35). Mass comes from `b3ShapeDef.density` (+ `updateBodyMass = true`, the default).
- There is no box-shape function: boxes are hulls — `b3BoxHull bh = b3MakeBoxHull(hx, hy, hz);` then `b3CreateHullShape(body, &sdef, &bh.base);`. Do NOT call `b3DestroyHull` on a `b3BoxHull`.
- `b3Quat` layout is `{ b3Vec3 v; float s; }` (vector part + scalar), NOT flat xyzw. `b3Pos` == `b3Vec3` in the default float build.
- Friction/restitution live on `b3ShapeDef.baseMaterial` (`b3SurfaceMaterial`), not on the shape def directly.
- Fixed timestep `1/120.0f`, `subStepCount = 4`. Determinism gate everywhere: run twice, byte-hash all final poses, assert equal.
- MatterEngine3 code style: C++17, `g++ -Wall -Wno-missing-braces -Wno-unused-variable -O2`; new engine sources are added to the explicit `ME3_CPP` list in `MatterEngine3/Makefile`.
- Test style: standalone binary per suite in `MatterEngine3/tests/`, `CHECK(cond, msg)` macro incrementing `g_failures`, `main()` returns `g_failures ? 1 : 0`, Makefile `run-<name>` target, registered in the `build-all.sh` test loop.
- Commit style: conventional commits (`feat:`, `test:`, `fix:`), matching recent history.

## File Structure

- `Libraries/box3d/` — vendored box3d v0.1.0 + new `Libraries/box3d/Makefile` producing `libbox3d.a`.
- `MatterEngine3/include/tileset_layout.h` + `MatterEngine3/src/tileset_layout.cpp` — torus/edge-color math (no box3d dependency).
- `MatterEngine3/include/tileset_collider.h` + `MatterEngine3/src/tileset_collider.cpp` — mesh → collider descriptor auto-fit (no box3d dependency; descriptors are converted to box3d shapes inside tileset_settle).
- `MatterEngine3/include/tileset_settle.h` + `MatterEngine3/src/tileset_settle.cpp` — box3d world wrapper, portal sync, layered settle.
- `MatterEngine3/tests/tileset_core_tests.cpp` — layout + collider unit tests (`run-tilesetcore`).
- `MatterEngine3/tests/tileset_physics_tests.cpp` — box3d smoke, settle, wrap, portal-sync, layering, determinism (`run-tilesetphysics`).
- Modify: `MatterEngine3/Makefile` (ME3_CPP list + `-I../Libraries/box3d/include`), `MatterEngine3/tests/Makefile` (two new targets), `build-all.sh` (register the two run targets).

---

### Task 1: Vendor box3d and prove it settles a falling box

**Files:**
- Create: `Libraries/box3d/` (vendored source tree), `Libraries/box3d/Makefile`
- Create: `MatterEngine3/tests/tileset_physics_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`, `build-all.sh`

**Interfaces:**
- Consumes: nothing (first task).
- Produces: `Libraries/box3d/libbox3d.a` + headers under `Libraries/box3d/include/box3d/`; the `tileset_physics_tests.cpp` harness (CHECK macro, `run_all` pattern) that Tasks 4–6 append test functions to; Makefile target `run-tilesetphysics`.

- [ ] **Step 1: Vendor box3d at v0.1.0**

```bash
cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp"
git clone --depth 1 --branch v0.1.0 https://github.com/erincatto/box3d.git Libraries/box3d
rm -rf Libraries/box3d/.git
ls Libraries/box3d/include/box3d/ Libraries/box3d/src/ | head -30
```

Expected: `include/box3d/` contains `box3d.h`, `types.h`, `collision.h`, `math_functions.h`, `id.h`, `base.h`, `constants.h`, `config.h`; `src/` contains ~50 `.c` files (`physics_world.c`, `solver.c`, `hull.c`, ...).

- [ ] **Step 2: Write `Libraries/box3d/Makefile`**

```makefile
# Static-lib build of box3d (v0.1.0). -ffp-contract=off is REQUIRED for
# box3d's cross-platform determinism (upstream sets it via CMake).
CC ?= gcc
CFLAGS = -std=c17 -O2 -ffp-contract=off -Iinclude -Isrc -Wall -Wno-unused-parameter

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)

libbox3d.a: $(OBJ)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) libbox3d.a

.PHONY: clean
```

- [ ] **Step 3: Build the static lib**

Run: `make -C Libraries/box3d -j8`
Expected: `Libraries/box3d/libbox3d.a` exists, no errors. (Warnings from alpha code are acceptable; errors are not. If a source file fails on a missing GNU extension, add `-D_GNU_SOURCE` to CFLAGS — do not edit vendored sources.)

- [ ] **Step 4: Write the failing smoke test**

Create `MatterEngine3/tests/tileset_physics_tests.cpp`:

```cpp
// Headless tests for the tileset settle core (box3d).
// Suite convention: CHECK macro + g_failures, exit code = failure count.
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#include "box3d/box3d.h"
#include "box3d/collision.h"
#include "box3d/math_functions.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

// ---------------------------------------------------------------------------
// test_smoke_drop: a unit cube dropped on a static slab settles and sleeps.
// Proves the vendored lib links and basic world/body/shape lifecycle works.
// ---------------------------------------------------------------------------
static void test_smoke_drop() {
    b3WorldDef wdef = b3DefaultWorldDef();
    wdef.gravity = (b3Vec3){ 0.0f, -10.0f, 0.0f };
    b3WorldId world = b3CreateWorld(&wdef);

    // Static ground slab: top surface at y = 0.
    b3BodyDef gdef = b3DefaultBodyDef();
    gdef.type = b3_staticBody;
    gdef.position = (b3Pos){ 0.0f, -1.0f, 0.0f };
    b3BodyId ground = b3CreateBody(world, &gdef);
    b3ShapeDef gsdef = b3DefaultShapeDef();
    b3BoxHull gbox = b3MakeBoxHull(50.0f, 1.0f, 50.0f);
    b3CreateHullShape(ground, &gsdef, &gbox.base);

    // Dynamic unit cube dropped from y = 3.
    b3BodyDef bdef = b3DefaultBodyDef();
    bdef.type = b3_dynamicBody;
    bdef.position = (b3Pos){ 0.0f, 3.0f, 0.0f };
    b3BodyId cube = b3CreateBody(world, &bdef);
    b3ShapeDef sdef = b3DefaultShapeDef();
    sdef.density = 1.0f;
    b3BoxHull cbox = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
    b3CreateHullShape(cube, &sdef, &cbox.base);

    for (int i = 0; i < 600; ++i) b3World_Step(world, 1.0f / 120.0f, 4);

    b3Pos p = b3Body_GetPosition(cube);
    CHECK(std::fabs(p.y - 0.5f) < 0.02f, "smoke: cube rests on slab (y ~= 0.5)");
    CHECK(!b3Body_IsAwake(cube), "smoke: cube fell asleep");

    b3DestroyWorld(world);
}

int main() {
    printf("== tileset_physics_tests ==\n");
    test_smoke_drop();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
```

- [ ] **Step 5: Add the Makefile target**

In `MatterEngine3/tests/Makefile`, following the existing per-suite target style, add:

```makefile
BOX3D_DIR = ../../Libraries/box3d

tileset_physics_tests: tileset_physics_tests.cpp $(BOX3D_DIR)/libbox3d.a
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I$(BOX3D_DIR)/include \
	    -o $@ tileset_physics_tests.cpp $(BOX3D_DIR)/libbox3d.a -lm

$(BOX3D_DIR)/libbox3d.a:
	$(MAKE) -C $(BOX3D_DIR)

run-tilesetphysics: tileset_physics_tests
	./tileset_physics_tests
```

- [ ] **Step 6: Run the suite**

Run: `make -C MatterEngine3/tests run-tilesetphysics`
Expected: `ok:` for both checks, `PASSED (0 failures)`, exit 0.

- [ ] **Step 7: Register in build-all.sh**

In `build-all.sh`, find the MatterEngine3 test loop (the `for tgt in run-partv2 run-script ...` list) and append `run-tilesetphysics` to the list.

- [ ] **Step 8: Commit**

```bash
git add Libraries/box3d MatterEngine3/tests/tileset_physics_tests.cpp MatterEngine3/tests/Makefile build-all.sh
git commit -m "feat: vendor box3d v0.1.0 with static-lib build + smoke settle test"
```

---

### Task 2: De Bruijn torus layout module

**Files:**
- Create: `MatterEngine3/include/tileset_layout.h`, `MatterEngine3/src/tileset_layout.cpp`
- Create: `MatterEngine3/tests/tileset_core_tests.cpp`
- Modify: `MatterEngine3/Makefile` (add to ME3_CPP), `MatterEngine3/tests/Makefile`, `build-all.sh`

**Interfaces:**
- Consumes: nothing.
- Produces (used by Tasks 5–6 and by Phases 2–4):
  - `tileset::kTorusN` (= 4), `tileset::kBoundaryColors[4]` (= {0,0,1,1})
  - `tileset::EdgeColors { int top, bottom, left, right; }`
  - `tileset::EdgeColors tileset::tile_colors(int row, int col)`
  - `int tileset::atlas_row(int top, int bottom)` / `int tileset::atlas_col(int left, int right)` (return -1 for an impossible pair)
  - `tileset::StripOccurrence { int boundary; int lane; }` and `std::vector<tileset::StripOccurrence> tileset::strip_occurrences(int color, bool vertical)`

Coordinate/naming convention (used verbatim through Phase 3): torus cell (row `i`, col `j`), tile side `S` meters; a tile's world rect is `x ∈ [j*S, (j+1)*S)`, `z ∈ [i*S, (i+1)*S)`. Vertical boundary `k` is the line `x = k*S` (color `kBoundaryColors[k]`); horizontal boundary `k` is `z = k*S`. `tile_colors(i,j) = { top: C[i], bottom: C[(i+1)%4], left: C[j], right: C[(j+1)%4] }` where `C = kBoundaryColors`.

- [ ] **Step 1: Write the failing tests**

Create `MatterEngine3/tests/tileset_core_tests.cpp`:

```cpp
// Unit tests for tileset layout math (and, from Task 3, collider auto-fit).
#include <cstdio>
#include <set>
#include <tuple>

#include "tileset_layout.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++g_failures; } \
    else         { printf("ok:   %s\n", (msg)); } \
} while (0)

static void test_layout_complete_set() {
    // All 16 (top,bottom,left,right) combinations occur exactly once.
    std::set<std::tuple<int,int,int,int>> seen;
    for (int i = 0; i < tileset::kTorusN; ++i)
        for (int j = 0; j < tileset::kTorusN; ++j) {
            tileset::EdgeColors c = tileset::tile_colors(i, j);
            seen.insert({c.top, c.bottom, c.left, c.right});
        }
    CHECK(seen.size() == 16, "layout: 16 unique edge-color tuples");
}

static void test_layout_adjacency() {
    // Every adjacency (including torus wrap) is color-legal.
    bool ok = true;
    const int N = tileset::kTorusN;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            tileset::EdgeColors c  = tileset::tile_colors(i, j);
            tileset::EdgeColors r  = tileset::tile_colors(i, (j + 1) % N);
            tileset::EdgeColors dn = tileset::tile_colors((i + 1) % N, j);
            if (c.right != r.left)   ok = false;
            if (c.bottom != dn.top)  ok = false;
        }
    CHECK(ok, "layout: all adjacencies match incl. torus wrap");
}

static void test_layout_atlas_inverse() {
    // atlas_row/atlas_col invert tile_colors.
    bool ok = true;
    for (int i = 0; i < tileset::kTorusN; ++i)
        for (int j = 0; j < tileset::kTorusN; ++j) {
            tileset::EdgeColors c = tileset::tile_colors(i, j);
            if (tileset::atlas_row(c.top, c.bottom) != i) ok = false;
            if (tileset::atlas_col(c.left, c.right) != j) ok = false;
        }
    CHECK(ok, "layout: atlas_row/col invert tile_colors");
    CHECK(tileset::atlas_row(2, 0) == -1, "layout: impossible pair returns -1");
}

static void test_layout_strip_occurrences() {
    // Each color occurs at exactly 2 boundaries x 4 lanes = 8 places.
    for (int color = 0; color < 2; ++color) {
        for (int vertical = 0; vertical < 2; ++vertical) {
            auto occ = tileset::strip_occurrences(color, vertical != 0);
            char msg[96];
            snprintf(msg, sizeof msg, "layout: color %d %s has 8 occurrences",
                     color, vertical ? "vertical" : "horizontal");
            CHECK(occ.size() == 8, msg);
            bool boundaries_ok = true;
            for (const auto& o : occ)
                if (tileset::kBoundaryColors[o.boundary] != color) boundaries_ok = false;
            CHECK(boundaries_ok, "layout: occurrence boundaries carry the color");
        }
    }
}

int main() {
    printf("== tileset_core_tests ==\n");
    test_layout_complete_set();
    test_layout_adjacency();
    test_layout_atlas_inverse();
    test_layout_strip_occurrences();
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
```

- [ ] **Step 2: Add the Makefile target and verify the test fails to build**

In `MatterEngine3/tests/Makefile` add:

```makefile
tileset_core_tests: tileset_core_tests.cpp ../src/tileset_layout.cpp
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include \
	    -o $@ tileset_core_tests.cpp ../src/tileset_layout.cpp

run-tilesetcore: tileset_core_tests
	./tileset_core_tests
```

Run: `make -C MatterEngine3/tests run-tilesetcore`
Expected: FAIL — `tileset_layout.h: No such file or directory`.

- [ ] **Step 3: Implement the layout module**

Create `MatterEngine3/include/tileset_layout.h`:

```cpp
#pragma once
#include <vector>

namespace tileset {

// 4x4 de Bruijn torus over 2 edge colors per orientation (complete 16-tile
// Wang set). Boundary color cycle B(2,2): consecutive pairs (0,0),(0,1),
// (1,1),(1,0) cover all combinations, wrapping.
inline constexpr int kTorusN = 4;
inline constexpr int kBoundaryColors[kTorusN] = { 0, 0, 1, 1 };

struct EdgeColors { int top, bottom, left, right; };

// Torus cell (row, col) -> its four edge colors.
EdgeColors tile_colors(int row, int col);

// Inverse: color pair -> torus row/col. Returns -1 for impossible pairs.
int atlas_row(int top, int bottom);
int atlas_col(int left, int right);

// One placement of an edge-color strip in the torus.
//   vertical strip:   line x = boundary * tileSize, lane = torus row (z cell)
//   horizontal strip: line z = boundary * tileSize, lane = torus col (x cell)
struct StripOccurrence { int boundary; int lane; };

// All torus placements of the given strip color: 2 boundaries x 4 lanes = 8.
std::vector<StripOccurrence> strip_occurrences(int color, bool vertical);

} // namespace tileset
```

Create `MatterEngine3/src/tileset_layout.cpp`:

```cpp
#include "tileset_layout.h"

namespace tileset {

EdgeColors tile_colors(int row, int col) {
    const int* C = kBoundaryColors;
    return EdgeColors{
        C[row], C[(row + 1) % kTorusN],
        C[col], C[(col + 1) % kTorusN],
    };
}

static int pair_index(int a, int b) {
    const int* C = kBoundaryColors;
    for (int k = 0; k < kTorusN; ++k)
        if (C[k] == a && C[(k + 1) % kTorusN] == b) return k;
    return -1;
}

int atlas_row(int top, int bottom) { return pair_index(top, bottom); }
int atlas_col(int left, int right) { return pair_index(left, right); }

std::vector<StripOccurrence> strip_occurrences(int color, bool /*vertical*/) {
    // Rows and columns share the same boundary cycle, so occurrences are
    // structurally identical for both orientations.
    std::vector<StripOccurrence> out;
    for (int k = 0; k < kTorusN; ++k) {
        if (kBoundaryColors[k] != color) continue;
        for (int lane = 0; lane < kTorusN; ++lane)
            out.push_back(StripOccurrence{ k, lane });
    }
    return out;
}

} // namespace tileset
```

- [ ] **Step 4: Run the tests**

Run: `make -C MatterEngine3/tests run-tilesetcore`
Expected: all `ok:`, `PASSED (0 failures)`, exit 0.

- [ ] **Step 5: Register the module and suite**

- In `MatterEngine3/Makefile`: append `src/tileset_layout.cpp` to the `ME3_CPP` list.
- In `build-all.sh`: append `run-tilesetcore` to the MatterEngine3 test target list.
- Run `make -C MatterEngine3` — expected: `libmatter_engine3.a` rebuilds cleanly.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/include/tileset_layout.h MatterEngine3/src/tileset_layout.cpp \
        MatterEngine3/tests/tileset_core_tests.cpp MatterEngine3/tests/Makefile \
        MatterEngine3/Makefile build-all.sh
git commit -m "feat: de Bruijn torus layout for tileset Wang set (16-tile complete map)"
```

---

### Task 3: Collider auto-fit

**Files:**
- Create: `MatterEngine3/include/tileset_collider.h`, `MatterEngine3/src/tileset_collider.cpp`
- Modify: `MatterEngine3/tests/tileset_core_tests.cpp`, `MatterEngine3/tests/Makefile` (add source to target), `MatterEngine3/Makefile` (ME3_CPP)

**Interfaces:**
- Consumes: nothing (pure geometry; no box3d dependency — descriptors become box3d shapes in Task 4).
- Produces (used by Tasks 4–6, Phase 2):
  - `tileset::ColliderType { Sphere, Capsule, Box, Hull }`
  - `tileset::ColliderFit { ColliderType type; float center[3]; float axis[3][3]; float half_extent[3]; float radius; float seg_half; std::vector<float> hull_points; float volume; }`
  - `tileset::ColliderFit tileset::fit_collider(const float* xyz, size_t vertex_count, const char* override_kind = nullptr)`

Semantics: `axis[0..2]` are orthonormal principal axes (descending extent) in the mesh's local frame; `half_extent[k]` is the half-extent along `axis[k]`. Capsule: core segment is `center ± seg_half * axis[0]`, radius `radius`. Hull: `hull_points` is a deterministically subsampled cloud (≤ 64 points, xyz triples) — final vertex reduction happens at shape-creation time via `b3CreateHull(points, n, 32)`. `volume` is the analytic volume of the fitted primitive (OBB-half volume for hulls); mass at settle time = `volume`-derived density heuristics are NOT used — density is passed separately and box3d computes mass from the real shape.

- [ ] **Step 1: Write the failing tests**

Append to `MatterEngine3/tests/tileset_core_tests.cpp` (before `main`), and add the four calls to `main` after the layout tests:

```cpp
#include "tileset_collider.h"
#include <cmath>
#include <cstdint>

// Deterministic SplitMix64 for fixture clouds (matches MatterEngine3/include/dsl_rng.h).
static uint64_t sm64(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static float sm64_unit(uint64_t& s) { return (float)((sm64(s) >> 11) * (1.0 / 9007199254740992.0)); }

static void test_collider_sphere() {
    // Fibonacci-ish sphere shell, radius 1.
    std::vector<float> pts;
    for (int k = 0; k < 200; ++k) {
        float y = 1.0f - 2.0f * (k + 0.5f) / 200.0f;
        float r = std::sqrt(1.0f - y * y);
        float a = 2.399963f * k;  // golden angle
        pts.push_back(r * std::cos(a)); pts.push_back(y); pts.push_back(r * std::sin(a));
    }
    tileset::ColliderFit f = tileset::fit_collider(pts.data(), pts.size() / 3);
    CHECK(f.type == tileset::ColliderType::Sphere, "collider: sphere cloud -> Sphere");
    CHECK(std::fabs(f.radius - 1.0f) < 0.15f, "collider: sphere radius ~= 1");
}

static void test_collider_twig() {
    // Thin cylinder along a rotated axis (length 1.0, radius 0.03).
    const float dir[3] = { 0.577350f, 0.577350f, 0.577350f };  // normalized (1,1,1)
    // Orthonormal basis around dir:
    const float u[3] = { 0.707107f, -0.707107f, 0.0f };
    const float v[3] = { 0.408248f, 0.408248f, -0.816497f };
    std::vector<float> pts;
    uint64_t s = 42;
    for (int k = 0; k < 300; ++k) {
        float t = (sm64_unit(s) - 0.5f) * 1.0f;           // along axis
        float a = sm64_unit(s) * 6.2831853f;
        float rr = 0.03f;
        for (int c = 0; c < 3; ++c)
            pts.push_back(t * dir[c] + rr * (std::cos(a) * u[c] + std::sin(a) * v[c]));
    }
    tileset::ColliderFit f = tileset::fit_collider(pts.data(), pts.size() / 3);
    CHECK(f.type == tileset::ColliderType::Capsule, "collider: twig cloud -> Capsule");
    float d = std::fabs(f.axis[0][0]*dir[0] + f.axis[0][1]*dir[1] + f.axis[0][2]*dir[2]);
    CHECK(d > 0.95f, "collider: capsule axis follows the twig direction");
}

static void test_collider_leaf() {
    // Flat ellipse in XZ with tiny Y jitter.
    std::vector<float> pts;
    uint64_t s = 7;
    for (int k = 0; k < 300; ++k) {
        float a = sm64_unit(s) * 6.2831853f, r = std::sqrt(sm64_unit(s));
        pts.push_back(0.05f * r * std::cos(a));
        pts.push_back(0.001f * (sm64_unit(s) - 0.5f));
        pts.push_back(0.03f * r * std::sin(a));
    }
    tileset::ColliderFit f = tileset::fit_collider(pts.data(), pts.size() / 3);
    CHECK(f.type == tileset::ColliderType::Box, "collider: leaf cloud -> thin Box");
    CHECK(f.half_extent[2] < 0.25f * f.half_extent[1], "collider: leaf box is thin");
}

static void test_collider_rock_and_override() {
    // Chunky irregular blob: jittered shell, mildly anisotropic.
    std::vector<float> pts;
    uint64_t s = 99;
    for (int k = 0; k < 300; ++k) {
        float y = 1.0f - 2.0f * (k + 0.5f) / 300.0f;
        float r = std::sqrt(1.0f - y * y);
        float a = 2.399963f * k;
        float j = 0.7f + 0.5f * sm64_unit(s);   // radial jitter
        pts.push_back(1.4f * j * r * std::cos(a));   // stretched in x
        pts.push_back(1.0f * j * y);
        pts.push_back(1.0f * j * r * std::sin(a));
    }
    tileset::ColliderFit f = tileset::fit_collider(pts.data(), pts.size() / 3);
    CHECK(f.type == tileset::ColliderType::Hull, "collider: rock blob -> Hull");
    CHECK(!f.hull_points.empty() && f.hull_points.size() <= 64 * 3,
          "collider: hull cloud subsampled to <= 64 points");
    tileset::ColliderFit o = tileset::fit_collider(pts.data(), pts.size() / 3, "sphere");
    CHECK(o.type == tileset::ColliderType::Sphere, "collider: override forces Sphere");
}
```

- [ ] **Step 2: Update the Makefile target and verify failure**

Change the `tileset_core_tests` target to compile the new source too:

```makefile
tileset_core_tests: tileset_core_tests.cpp ../src/tileset_layout.cpp ../src/tileset_collider.cpp
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include \
	    -o $@ tileset_core_tests.cpp ../src/tileset_layout.cpp ../src/tileset_collider.cpp
```

Run: `make -C MatterEngine3/tests run-tilesetcore`
Expected: FAIL — `tileset_collider.h: No such file or directory`.

- [ ] **Step 3: Implement the collider module**

Create `MatterEngine3/include/tileset_collider.h`:

```cpp
#pragma once
#include <cstddef>
#include <vector>

namespace tileset {

enum class ColliderType { Sphere, Capsule, Box, Hull };

struct ColliderFit {
    ColliderType type = ColliderType::Hull;
    float center[3] = { 0, 0, 0 };
    float axis[3][3] = { { 1,0,0 }, { 0,1,0 }, { 0,0,1 } };  // orthonormal, desc. extent
    float half_extent[3] = { 0, 0, 0 };                      // along axis[0..2]
    float radius = 0.0f;      // Sphere / Capsule
    float seg_half = 0.0f;    // Capsule core segment half-length along axis[0]
    std::vector<float> hull_points;  // Hull: xyz triples, <= 64 points
    float volume = 0.0f;      // analytic volume of the fitted primitive
};

// Fit a collision proxy to a vertex cloud (xyz triples) via PCA-OBB and an
// aspect-ratio heuristic. override_kind: nullptr/"auto" for the heuristic,
// or "sphere" | "capsule" | "box" | "hull" to force a type.
ColliderFit fit_collider(const float* xyz, size_t vertex_count,
                         const char* override_kind = nullptr);

} // namespace tileset
```

Create `MatterEngine3/src/tileset_collider.cpp`:

```cpp
#include "tileset_collider.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace tileset {

// Cyclic Jacobi eigen-decomposition of a symmetric 3x3 matrix.
// On return, a[] is (near-)diagonal and v[] holds column eigenvectors.
static void jacobi3(float a[3][3], float v[3][3]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) v[r][c] = (r == c) ? 1.0f : 0.0f;
    for (int sweep = 0; sweep < 24; ++sweep) {
        float off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        if (off < 1e-12f) break;
        for (int p = 0; p < 2; ++p) for (int q = p + 1; q < 3; ++q) {
            if (std::fabs(a[p][q]) < 1e-15f) continue;
            float theta = (a[q][q] - a[p][p]) / (2.0f * a[p][q]);
            float t = (theta >= 0 ? 1.0f : -1.0f) /
                      (std::fabs(theta) + std::sqrt(theta * theta + 1.0f));
            float c = 1.0f / std::sqrt(t * t + 1.0f), s = t * c;
            for (int k = 0; k < 3; ++k) {
                float akp = a[k][p], akq = a[k][q];
                a[k][p] = c * akp - s * akq;
                a[k][q] = s * akp + c * akq;
            }
            for (int k = 0; k < 3; ++k) {
                float apk = a[p][k], aqk = a[q][k];
                a[p][k] = c * apk - s * aqk;
                a[q][k] = s * apk + c * aqk;
            }
            for (int k = 0; k < 3; ++k) {
                float vkp = v[k][p], vkq = v[k][q];
                v[k][p] = c * vkp - s * vkq;
                v[k][q] = s * vkp + c * vkq;
            }
        }
    }
}

ColliderFit fit_collider(const float* xyz, size_t n, const char* override_kind) {
    ColliderFit f;
    if (n == 0) return f;

    // Centroid.
    double cx = 0, cy = 0, cz = 0;
    for (size_t i = 0; i < n; ++i) { cx += xyz[3*i]; cy += xyz[3*i+1]; cz += xyz[3*i+2]; }
    f.center[0] = (float)(cx / n); f.center[1] = (float)(cy / n); f.center[2] = (float)(cz / n);

    // Covariance.
    float cov[3][3] = {};
    for (size_t i = 0; i < n; ++i) {
        float d[3] = { xyz[3*i] - f.center[0], xyz[3*i+1] - f.center[1], xyz[3*i+2] - f.center[2] };
        for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) cov[r][c] += d[r] * d[c];
    }
    float evec[3][3];
    jacobi3(cov, evec);

    // Half-extents along each eigenvector (max |projection|), then sort desc.
    float ax[3][3], ext[3];
    for (int k = 0; k < 3; ++k) {
        ax[k][0] = evec[0][k]; ax[k][1] = evec[1][k]; ax[k][2] = evec[2][k];
        float len = std::sqrt(ax[k][0]*ax[k][0] + ax[k][1]*ax[k][1] + ax[k][2]*ax[k][2]);
        for (int c = 0; c < 3; ++c) ax[k][c] /= (len > 0 ? len : 1.0f);
        float m = 0;
        for (size_t i = 0; i < n; ++i) {
            float d = (xyz[3*i]   - f.center[0]) * ax[k][0]
                    + (xyz[3*i+1] - f.center[1]) * ax[k][1]
                    + (xyz[3*i+2] - f.center[2]) * ax[k][2];
            m = std::max(m, std::fabs(d));
        }
        ext[k] = m;
    }
    int order[3] = { 0, 1, 2 };
    std::sort(order, order + 3, [&](int a, int b) { return ext[a] > ext[b]; });
    for (int k = 0; k < 3; ++k) {
        f.half_extent[k] = ext[order[k]];
        std::memcpy(f.axis[k], ax[order[k]], sizeof(float) * 3);
    }
    const float e0 = f.half_extent[0], e1 = f.half_extent[1], e2 = f.half_extent[2];

    // Type: override or aspect-ratio heuristic.
    ColliderType type;
    if (override_kind && std::strcmp(override_kind, "auto") != 0) {
        type = ColliderType::Hull;
        if (!std::strcmp(override_kind, "sphere"))  type = ColliderType::Sphere;
        if (!std::strcmp(override_kind, "capsule")) type = ColliderType::Capsule;
        if (!std::strcmp(override_kind, "box"))     type = ColliderType::Box;
    } else if (e0 <= 1.3f * e2)        type = ColliderType::Sphere;   // isotropic
    else if (e0 >= 2.2f * e1)          type = ColliderType::Capsule;  // elongated
    else if (e2 <= 0.35f * e1)         type = ColliderType::Box;      // flat
    else                               type = ColliderType::Hull;     // chunky
    f.type = type;

    const float kPi = 3.14159265358979f;
    switch (type) {
    case ColliderType::Sphere:
        f.radius = (e0 + e1 + e2) / 3.0f;
        f.volume = (4.0f / 3.0f) * kPi * f.radius * f.radius * f.radius;
        break;
    case ColliderType::Capsule:
        f.radius = std::max(e1, e2);
        f.seg_half = std::max(0.0f, e0 - f.radius);
        f.volume = kPi * f.radius * f.radius * (2.0f * f.seg_half)
                 + (4.0f / 3.0f) * kPi * f.radius * f.radius * f.radius;
        break;
    case ColliderType::Box:
        f.volume = 8.0f * e0 * e1 * e2;
        break;
    case ColliderType::Hull: {
        size_t stride = std::max<size_t>(1, n / 64);
        for (size_t i = 0; i < n && f.hull_points.size() < 64 * 3; i += stride) {
            f.hull_points.push_back(xyz[3*i]);
            f.hull_points.push_back(xyz[3*i+1]);
            f.hull_points.push_back(xyz[3*i+2]);
        }
        f.volume = 4.0f * e0 * e1 * e2;   // ~half the OBB; box3d computes true mass
        break;
    }
    }
    return f;
}

} // namespace tileset
```

- [ ] **Step 4: Run the tests**

Run: `make -C MatterEngine3/tests run-tilesetcore`
Expected: all `ok:` (layout + 4 collider tests), `PASSED (0 failures)`. If the heuristic thresholds misclassify a fixture, tune the fixture-independent constants (1.3 / 2.2 / 0.35) — do not special-case fixtures.

- [ ] **Step 5: Register and commit**

- Append `src/tileset_collider.cpp` to `ME3_CPP` in `MatterEngine3/Makefile`; run `make -C MatterEngine3` to confirm the lib builds.

```bash
git add MatterEngine3/include/tileset_collider.h MatterEngine3/src/tileset_collider.cpp \
        MatterEngine3/tests/tileset_core_tests.cpp MatterEngine3/tests/Makefile MatterEngine3/Makefile
git commit -m "feat: PCA-OBB collider auto-fit for tileset scatter parts"
```

---

### Task 4: SettleWorld core (box3d wrapper: base heightfield, toroidal wrap, convergence)

**Files:**
- Create: `MatterEngine3/include/tileset_settle.h`, `MatterEngine3/src/tileset_settle.cpp`
- Modify: `MatterEngine3/tests/tileset_physics_tests.cpp`, `MatterEngine3/tests/Makefile`, `MatterEngine3/Makefile` (ME3_CPP + box3d include path)

**Interfaces:**
- Consumes: `tileset::ColliderFit` / `tileset::ColliderType` (Task 3).
- Produces (used by Tasks 5–6 and Phase 2) — the full public header is given in Step 3; key signatures:
  - `tileset::Pose { float px, py, pz, qx, qy, qz, qw; }` (world meters, xyzw quat)
  - `tileset::BodySpawn { const ColliderFit* collider; Pose start; float density = 400.0f; float friction = 0.6f; float vx = 0, vy = 0, vz = 0; int sync_group = -1; int instance = 0; }`
  - `tileset::SettleParams { float dt = 1/120.0f; int substeps = 4; float max_sim_time = 10.0f; float sleep_fraction = 0.99f; float sim_scale = 4.0f; int micro_relax_steps = 30; }`
  - `tileset::HeightField { int count_x, count_z; float cell; std::vector<float> heights; }` (row-major)
  - `tileset::LayerResult { bool converged; int awake_count; float sim_time; }`
  - `class tileset::SettleWorld` — `SettleWorld(float torus_size, const HeightField& base, const SettleParams& params)`, `LayerResult settle_layer(const std::vector<BodySpawn>&)`, `const std::vector<Pose>& poses() const`, `uint64_t pose_hash() const`. (Task 5 adds `int add_sync_group(const std::vector<Pose>&)` and `void finalize()`.)

Design notes locked here:
- **sim_scale:** box3d's `B3_LINEAR_SLOP` is 0.005 sim-units, spongy for centimeter litter. All geometry/positions are multiplied by `sim_scale` on the way in, divided on the way out; gravity is `-9.8 * sim_scale` so real-world timing is preserved.
- **Base collider is a triangle mesh built from the HeightField** (two CCW-up triangles per cell) via `b3MeshDef`/`b3CreateMesh`/`b3CreateMeshShape` — not `b3CreateHeightFieldShape`, so winding and scaling semantics are fully under our control. `b3MeshDef`/`b3ShapeDef` note: `b3ShapeDef` MUST come from `b3DefaultShapeDef()`; `b3MeshDef` has no default-fn/cookie and is plain zero-init.
- **Toroidal wrap:** after each step, any awake dynamic body whose sim-space x/z leaves `[0, torus)` is shifted by ±torus via `b3Body_SetTransform` (rotation and velocities untouched).
- **Convergence:** after each step, count awake dynamic bodies across ALL layers spawned so far; converged when `awake <= (1 - sleep_fraction) * total` (with the [0,1) fraction, 0.99 → all 50 test bodies must sleep since floor(0.01*50)=0 means awake==0... compute as `awake_count <= (int)((1.0f - sleep_fraction) * total)`).

- [ ] **Step 1: Write the failing tests**

Append to `MatterEngine3/tests/tileset_physics_tests.cpp` (and call the three new tests from `main` after `test_smoke_drop`):

```cpp
#include "tileset_settle.h"
#include "tileset_collider.h"
#include <cstdint>

static uint64_t phys_sm64(uint64_t& s) {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static float phys_unit(uint64_t& s) { return (float)((phys_sm64(s) >> 11) * (1.0 / 9007199254740992.0)); }

static tileset::HeightField flat_field(float torus, float cell) {
    tileset::HeightField hf;
    hf.count_x = hf.count_z = (int)(torus / cell) + 1;
    hf.cell = cell;
    hf.heights.assign((size_t)hf.count_x * hf.count_z, 0.0f);
    return hf;
}

static tileset::ColliderFit small_box_fit() {
    tileset::ColliderFit f;
    f.type = tileset::ColliderType::Box;
    f.half_extent[0] = f.half_extent[1] = f.half_extent[2] = 0.05f;
    f.volume = 8.0f * 0.05f * 0.05f * 0.05f;
    return f;
}

// 50 small boxes dropped on a flat 8m torus settle, sleep, and rest on the ground.
static void test_settle_boxes_converge() {
    const float T = 8.0f;
    tileset::HeightField hf = flat_field(T, 0.25f);
    tileset::SettleParams sp;
    tileset::SettleWorld sw(T, hf, sp);

    tileset::ColliderFit box = small_box_fit();
    std::vector<tileset::BodySpawn> spawns;
    uint64_t s = 1234;
    for (int i = 0; i < 50; ++i) {
        tileset::BodySpawn b;
        b.collider = &box;
        b.start = { phys_unit(s) * T, 0.3f + 0.7f * phys_unit(s), phys_unit(s) * T,
                    0, 0, 0, 1 };
        spawns.push_back(b);
    }
    tileset::LayerResult r = sw.settle_layer(spawns);
    CHECK(r.converged, "settle: 50 boxes converge within max_sim_time");

    bool grounded = true, in_range = true;
    for (const tileset::Pose& p : sw.poses()) {
        if (p.py < 0.03f || p.py > 0.20f) grounded = false;   // rest height ~0.05
        if (p.px < 0.0f || p.px >= T || p.pz < 0.0f || p.pz >= T) in_range = false;
    }
    CHECK(grounded, "settle: all boxes rest on the ground plane (no tunneling/float)");
    CHECK(in_range, "settle: all boxes inside the torus domain");
}

// A fast sphere crosses the +x boundary and reappears inside [0, T).
static void test_settle_toroidal_wrap() {
    const float T = 8.0f;
    tileset::HeightField hf = flat_field(T, 0.25f);
    tileset::SettleParams sp;
    sp.max_sim_time = 2.0f;
    sp.sleep_fraction = 2.0f;   // force full-duration run (never "converges")
    tileset::SettleWorld sw(T, hf, sp);

    tileset::ColliderFit ball;
    ball.type = tileset::ColliderType::Sphere;
    ball.radius = 0.1f;
    ball.volume = (4.0f / 3.0f) * 3.14159265f * 0.001f;

    tileset::BodySpawn b;
    b.collider = &ball;
    b.start = { 7.8f, 0.11f, 4.0f, 0, 0, 0, 1 };
    b.vx = 2.0f;               // will cross x = 8 within the 2s budget
    b.friction = 0.0f;
    sw.settle_layer({ b });

    const tileset::Pose& p = sw.poses()[0];
    CHECK(p.px >= 0.0f && p.px < T, "wrap: sphere re-entered [0, T) across +x boundary");
    CHECK(p.px < 6.0f, "wrap: sphere actually crossed (not just stopped at the edge)");
}

// Identical inputs -> bit-identical final poses (box3d determinism + our loop).
static void test_settle_determinism() {
    auto run = []() -> uint64_t {
        const float T = 8.0f;
        tileset::HeightField hf = flat_field(T, 0.25f);
        tileset::SettleParams sp;
        tileset::SettleWorld sw(T, hf, sp);
        tileset::ColliderFit box = small_box_fit();
        std::vector<tileset::BodySpawn> spawns;
        uint64_t s = 777;
        for (int i = 0; i < 30; ++i) {
            tileset::BodySpawn b;
            b.collider = &box;
            b.start = { phys_unit(s) * T, 0.3f + 0.5f * phys_unit(s), phys_unit(s) * T,
                        0, 0, 0, 1 };
            spawns.push_back(b);
        }
        sw.settle_layer(spawns);
        return sw.pose_hash();
    };
    uint64_t h1 = run(), h2 = run();
    CHECK(h1 == h2, "determinism: double-run pose hashes are identical");
}
```

- [ ] **Step 2: Update the Makefile target and verify failure**

Change the `tileset_physics_tests` target to include the new engine sources:

```makefile
tileset_physics_tests: tileset_physics_tests.cpp ../src/tileset_settle.cpp ../src/tileset_collider.cpp $(BOX3D_DIR)/libbox3d.a
	g++ -std=c++17 -Wall -Wno-missing-braces -O2 -I../include -I$(BOX3D_DIR)/include \
	    -o $@ tileset_physics_tests.cpp ../src/tileset_settle.cpp ../src/tileset_collider.cpp \
	    $(BOX3D_DIR)/libbox3d.a -lm
```

Run: `make -C MatterEngine3/tests run-tilesetphysics`
Expected: FAIL — `tileset_settle.h: No such file or directory`.

- [ ] **Step 3: Write the public header**

Create `MatterEngine3/include/tileset_settle.h`:

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include "tileset_collider.h"

namespace tileset {

// World-space rigid pose, meters, xyzw quaternion.
struct Pose { float px, py, pz; float qx, qy, qz, qw; };

struct BodySpawn {
    const ColliderFit* collider = nullptr;  // borrowed for the settle_layer call
    Pose start = { 0, 0, 0, 0, 0, 0, 1 };
    float density = 400.0f;   // kg/m^3 (dry wood-ish default)
    float friction = 0.6f;
    float vx = 0, vy = 0, vz = 0;
    int sync_group = -1;      // -1 = free body; >= 0 = portal-sync group id
    int instance = 0;         // occurrence index within the sync group
};

struct SettleParams {
    float dt = 1.0f / 120.0f;
    int substeps = 4;
    float max_sim_time = 10.0f;    // per layer
    float sleep_fraction = 0.99f;  // converged when this fraction is asleep
    float sim_scale = 4.0f;        // meters -> sim units (linear-slop tuning)
    int micro_relax_steps = 30;    // finalize(): relax after snap
};

// Base terrain over the full torus; row-major heights, `cell` meters apart.
struct HeightField {
    int count_x = 0, count_z = 0;
    float cell = 0.0f;
    std::vector<float> heights;
};

struct LayerResult {
    bool converged = false;
    int awake_count = 0;   // awake dynamic bodies when the loop ended
    float sim_time = 0.0f;
};

// One box3d world spanning the 4x4 torus. Bodies wrap toroidally in x/z.
class SettleWorld {
public:
    SettleWorld(float torus_size, const HeightField& base, const SettleParams& params);
    ~SettleWorld();
    SettleWorld(const SettleWorld&) = delete;
    SettleWorld& operator=(const SettleWorld&) = delete;

    // Portal-sync group: K world-space occurrence frames of one canonical
    // strip frame. Members are bound via BodySpawn::{sync_group, instance}.
    int add_sync_group(const std::vector<Pose>& occurrence_frames);

    // Spawn one layer and step until converged or out of time.
    // Earlier layers stay dynamic (asleep unless disturbed).
    LayerResult settle_layer(const std::vector<BodySpawn>& bodies);

    // After the last layer: snap sync groups to exact shared poses, switch
    // them kinematic, micro-relax, and refresh poses().
    void finalize();

    // Final world-space poses, in spawn order across all layers.
    const std::vector<Pose>& poses() const;

    // FNV-1a hash over all final poses (determinism gate).
    uint64_t pose_hash() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace tileset
```

- [ ] **Step 4: Write the implementation (sync-group methods as stubs)**

Create `MatterEngine3/src/tileset_settle.cpp`. `add_sync_group`/`finalize` bodies land in Task 5; here they are minimal (`add_sync_group` records frames and returns the id; `finalize` just refreshes poses).

```cpp
#include "tileset_settle.h"

#include <cmath>
#include <cstring>

#include "box3d/box3d.h"
#include "box3d/collision.h"
#include "box3d/math_functions.h"

namespace tileset {

// ---- small math helpers ----------------------------------------------------

static b3Quat to_b3quat(const Pose& p) {
    b3Quat q; q.v.x = p.qx; q.v.y = p.qy; q.v.z = p.qz; q.s = p.qw; return q;
}

// Column-axes rotation matrix (columns = collider axes) -> quaternion.
static void axes_to_quat(const float axis[3][3], float out_xyzw[4]) {
    // m[r][c], columns are axis[0], axis[1], axis[2] (right-handed enforced).
    float a2[3] = { axis[2][0], axis[2][1], axis[2][2] };
    float cx = axis[0][1] * axis[1][2] - axis[0][2] * axis[1][1];
    float cy = axis[0][2] * axis[1][0] - axis[0][0] * axis[1][2];
    float cz = axis[0][0] * axis[1][1] - axis[0][1] * axis[1][0];
    if (cx * a2[0] + cy * a2[1] + cz * a2[2] < 0.0f) { a2[0] = -a2[0]; a2[1] = -a2[1]; a2[2] = -a2[2]; }
    float m[3][3] = {
        { axis[0][0], axis[1][0], a2[0] },
        { axis[0][1], axis[1][1], a2[1] },
        { axis[0][2], axis[1][2], a2[2] },
    };
    float tr = m[0][0] + m[1][1] + m[2][2];
    float x, y, z, w;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        w = 0.25f * s; x = (m[2][1] - m[1][2]) / s; y = (m[0][2] - m[2][0]) / s; z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        w = (m[2][1] - m[1][2]) / s; x = 0.25f * s; y = (m[0][1] + m[1][0]) / s; z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        w = (m[0][2] - m[2][0]) / s; x = (m[0][1] + m[1][0]) / s; y = 0.25f * s; z = (m[1][2] + m[2][1]) / s;
    } else {
        float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        w = (m[1][0] - m[0][1]) / s; x = (m[0][2] + m[2][0]) / s; y = (m[1][2] + m[2][1]) / s; z = 0.25f * s;
    }
    out_xyzw[0] = x; out_xyzw[1] = y; out_xyzw[2] = z; out_xyzw[3] = w;
}

// ---- impl -------------------------------------------------------------------

struct SettleWorld::Impl {
    b3WorldId world;
    float torus = 0.0f;        // sim units
    SettleParams params;
    // base mesh data must outlive the world's mesh shape
    std::vector<b3Vec3> base_verts;
    std::vector<int32_t> base_indices;
    b3MeshData* base_mesh = nullptr;

    struct TrackedBody { b3BodyId id; int group; int instance; };
    std::vector<TrackedBody> bodies;   // spawn order
    std::vector<Pose> out_poses;

    struct SyncGroup {
        std::vector<Pose> frames;                    // sim-scaled occurrence frames
        std::vector<std::vector<int>> members;       // [occurrence][canonical idx] -> bodies index
    };
    std::vector<SyncGroup> groups;

    void wrap_bodies();
    void sync_groups_step();   // Task 5
    void refresh_poses();
    int count_awake() const;
};

SettleWorld::SettleWorld(float torus_size, const HeightField& base, const SettleParams& params) {
    impl_ = new Impl();
    impl_->params = params;
    const float S = params.sim_scale;
    impl_->torus = torus_size * S;

    b3WorldDef wdef = b3DefaultWorldDef();
    wdef.gravity = (b3Vec3){ 0.0f, -9.8f * S, 0.0f };
    impl_->world = b3CreateWorld(&wdef);

    // Base terrain: two CCW-up triangles per heightfield cell.
    const int nx = base.count_x, nz = base.count_z;
    impl_->base_verts.reserve((size_t)nx * nz);
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
            impl_->base_verts.push_back((b3Vec3){
                x * base.cell * S,
                base.heights[(size_t)z * nx + x] * S,
                z * base.cell * S });
    for (int z = 0; z + 1 < nz; ++z)
        for (int x = 0; x + 1 < nx; ++x) {
            int32_t a = z * nx + x,       b = z * nx + x + 1;
            int32_t c = (z + 1) * nx + x, d = (z + 1) * nx + x + 1;
            // +Y-up winding (counter-clockwise seen from above).
            impl_->base_indices.insert(impl_->base_indices.end(), { a, c, b });
            impl_->base_indices.insert(impl_->base_indices.end(), { b, c, d });
        }
    b3MeshDef mdef;
    std::memset(&mdef, 0, sizeof mdef);   // plain struct: no Default fn / cookie
    mdef.vertices = impl_->base_verts.data();
    mdef.vertexCount = (int)impl_->base_verts.size();
    mdef.indices = impl_->base_indices.data();
    mdef.triangleCount = (int)(impl_->base_indices.size() / 3);
    mdef.identifyEdges = true;
    impl_->base_mesh = b3CreateMesh(&mdef, nullptr, 0);

    b3BodyDef gdef = b3DefaultBodyDef();
    gdef.type = b3_staticBody;
    b3BodyId ground = b3CreateBody(impl_->world, &gdef);
    b3ShapeDef gsdef = b3DefaultShapeDef();
    b3CreateMeshShape(ground, &gsdef, impl_->base_mesh, (b3Vec3){ 1.0f, 1.0f, 1.0f });
}

SettleWorld::~SettleWorld() {
    b3DestroyWorld(impl_->world);
    if (impl_->base_mesh) b3DestroyMesh(impl_->base_mesh);
    delete impl_;
}

int SettleWorld::add_sync_group(const std::vector<Pose>& occurrence_frames) {
    Impl::SyncGroup g;
    g.frames = occurrence_frames;
    for (Pose& f : g.frames) { f.px *= impl_->params.sim_scale; f.py *= impl_->params.sim_scale; f.pz *= impl_->params.sim_scale; }
    impl_->groups.push_back(std::move(g));
    return (int)impl_->groups.size() - 1;
}

LayerResult SettleWorld::settle_layer(const std::vector<BodySpawn>& spawns) {
    const float S = impl_->params.sim_scale;

    for (const BodySpawn& sp : spawns) {
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position = (b3Pos){ sp.start.px * S, sp.start.py * S, sp.start.pz * S };
        bd.rotation = to_b3quat(sp.start);
        bd.linearVelocity = (b3Vec3){ sp.vx * S, sp.vy * S, sp.vz * S };
        b3BodyId id = b3CreateBody(impl_->world, &bd);

        b3ShapeDef sd = b3DefaultShapeDef();
        // Mass must stay scale-invariant: density is per sim-volume, which is
        // volume * S^3, so divide density by S^3.
        sd.density = sp.density / (S * S * S);
        sd.baseMaterial.friction = sp.friction;

        const ColliderFit& c = *sp.collider;
        const float cx = c.center[0] * S, cy = c.center[1] * S, cz = c.center[2] * S;
        switch (c.type) {
        case ColliderType::Sphere: {
            b3Sphere s = { (b3Vec3){ cx, cy, cz }, c.radius * S };
            b3CreateSphereShape(id, &sd, &s);
            break;
        }
        case ColliderType::Capsule: {
            b3Capsule cap;
            cap.center1 = (b3Vec3){ cx - c.seg_half * S * c.axis[0][0],
                                    cy - c.seg_half * S * c.axis[0][1],
                                    cz - c.seg_half * S * c.axis[0][2] };
            cap.center2 = (b3Vec3){ cx + c.seg_half * S * c.axis[0][0],
                                    cy + c.seg_half * S * c.axis[0][1],
                                    cz + c.seg_half * S * c.axis[0][2] };
            cap.radius = c.radius * S;
            b3CreateCapsuleShape(id, &sd, &cap);
            break;
        }
        case ColliderType::Box: {
            float q[4];
            axes_to_quat(c.axis, q);
            b3Transform xf;
            xf.p = (b3Vec3){ cx, cy, cz };
            xf.q.v.x = q[0]; xf.q.v.y = q[1]; xf.q.v.z = q[2]; xf.q.s = q[3];
            b3BoxHull bh = b3MakeTransformedBoxHull(
                c.half_extent[0] * S, c.half_extent[1] * S, c.half_extent[2] * S, xf);
            b3CreateHullShape(id, &sd, &bh.base);
            break;
        }
        case ColliderType::Hull: {
            std::vector<b3Vec3> pts;
            for (size_t i = 0; i + 2 < c.hull_points.size(); i += 3)
                pts.push_back((b3Vec3){ c.hull_points[i] * S,
                                        c.hull_points[i+1] * S,
                                        c.hull_points[i+2] * S });
            b3HullData* hull = b3CreateHull(pts.data(), (int)pts.size(), 32);
            b3CreateHullShape(id, &sd, hull);
            b3DestroyHull(hull);
            break;
        }
        }
        impl_->bodies.push_back(Impl::TrackedBody{ id, sp.sync_group, sp.instance });
        if (sp.sync_group >= 0) {
            Impl::SyncGroup& g = impl_->groups[sp.sync_group];
            if ((int)g.members.size() <= sp.instance) g.members.resize(sp.instance + 1);
            // Canonical index within the group is the order of first-occurrence
            // spawns; members[occurrence] lists bodies in canonical order.
            g.members[sp.instance].push_back((int)impl_->bodies.size() - 1);
        }
    }

    LayerResult r;
    const SettleParams& P = impl_->params;
    const int total = (int)impl_->bodies.size();
    while (r.sim_time < P.max_sim_time) {
        b3World_Step(impl_->world, P.dt, P.substeps);
        impl_->wrap_bodies();
        impl_->sync_groups_step();
        r.sim_time += P.dt;
        r.awake_count = impl_->count_awake();
        if (r.awake_count <= (int)((1.0f - P.sleep_fraction) * total)) {
            r.converged = true;
            break;
        }
    }
    impl_->refresh_poses();
    return r;
}

void SettleWorld::finalize() {
    // Task 5: snap sync groups, kinematic micro-relax. Base version: no-op.
    impl_->refresh_poses();
}

const std::vector<Pose>& SettleWorld::poses() const { return impl_->out_poses; }

uint64_t SettleWorld::pose_hash() const {
    uint64_t h = 1469598103934665603ull;   // FNV-1a
    for (const Pose& p : impl_->out_poses) {
        unsigned char buf[sizeof(Pose)];
        std::memcpy(buf, &p, sizeof(Pose));
        for (unsigned char b : buf) { h ^= b; h *= 1099511628211ull; }
    }
    return h;
}

void SettleWorld::Impl::wrap_bodies() {
    if (torus <= 0.0f) return;
    for (const TrackedBody& tb : bodies) {
        if (!b3Body_IsAwake(tb.id)) continue;
        b3Pos p = b3Body_GetPosition(tb.id);
        float nx = p.x, nz = p.z;
        while (nx < 0.0f)     nx += torus;
        while (nx >= torus)   nx -= torus;
        while (nz < 0.0f)     nz += torus;
        while (nz >= torus)   nz -= torus;
        if (nx != p.x || nz != p.z)
            b3Body_SetTransform(tb.id, (b3Pos){ nx, p.y, nz }, b3Body_GetRotation(tb.id));
    }
}

void SettleWorld::Impl::sync_groups_step() {
    // Implemented in Task 5.
}

void SettleWorld::Impl::refresh_poses() {
    const float inv = 1.0f / params.sim_scale;
    out_poses.clear();
    out_poses.reserve(bodies.size());
    for (const TrackedBody& tb : bodies) {
        b3Pos p = b3Body_GetPosition(tb.id);
        b3Quat q = b3Body_GetRotation(tb.id);
        out_poses.push_back(Pose{ p.x * inv, p.y * inv, p.z * inv,
                                  q.v.x, q.v.y, q.v.z, q.s });
    }
}

int SettleWorld::Impl::count_awake() const {
    int n = 0;
    for (const TrackedBody& tb : bodies)
        if (b3Body_IsAwake(tb.id)) ++n;
    return n;
}

} // namespace tileset
```

- [ ] **Step 5: Run the tests**

Run: `make -C MatterEngine3/tests run-tilesetphysics`
Expected: smoke + 3 new tests all `ok:`, `PASSED (0 failures)`.

Debug guidance for likely first-run failures (fix the root cause, don't loosen assertions):
- Boxes tunnel through the base → the mesh winding is backwards for box3d; swap the two index triples to `{a, b, c}` / `{b, d, c}`.
- `b3CreateMesh` asserts on the zeroed def → check whether `include/box3d/types.h` declares `b3DefaultMeshDef()`; if it exists, use it instead of memset.
- Boxes jitter forever (never sleep) → raise `subStepCount` is NOT the fix; verify `sd.density` scaling and that `sim_scale` is applied to gravity.

- [ ] **Step 6: Register the module**

- Append `src/tileset_settle.cpp` to `ME3_CPP` in `MatterEngine3/Makefile` and add `-I../Libraries/box3d/include` to its include flags; run `make -C MatterEngine3` to confirm.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/include/tileset_settle.h MatterEngine3/src/tileset_settle.cpp \
        MatterEngine3/tests/tileset_physics_tests.cpp MatterEngine3/tests/Makefile MatterEngine3/Makefile
git commit -m "feat: box3d settle world with toroidal wrap and convergence gate"
```

---

### Task 5: Portal-sync groups (pose averaging, exact snap, kinematic micro-relax)

**Files:**
- Modify: `MatterEngine3/src/tileset_settle.cpp` (replace the `sync_groups_step` stub and the `finalize` no-op; change the `Impl` method declaration)
- Modify: `MatterEngine3/tests/tileset_physics_tests.cpp` (two new tests)

**Interfaces:**
- Consumes: everything from Task 4 (`SettleWorld`, `Impl::groups` with `frames` sim-scaled and `members[occurrence][canonical]`, `Impl::bodies`, `wrap_bodies`, `refresh_poses`).
- Produces: working `add_sync_group` + `finalize()` semantics that Phase 2's tileset bake driver relies on:
  - After `finalize()`, all instances of a canonical body have **bit-identical rotations** and positions that differ from each other **exactly by their occurrence-frame translations** (in sim space; ≤1e-4 m drift after unscaling).
  - `finalize()` switches every sync-group body to kinematic and runs `micro_relax_steps` extra steps so free bodies re-settle against the frozen strips.

Design notes locked here:
- **Occurrence frames are pure translations** (the spec's de Bruijn strip instancing never rotates), so strip-local pose = world pose minus frame translation, and quaternions are directly comparable across instances.
- **Averaging = averaged net force.** Every step, each canonical body's K instances are mapped to strip-local, averaged (positions, velocities, sign-aligned quaternions), and written back to all K. Per-step divergence is tiny, so this is stable.
- **Torus-wrap-aware deltas:** local positions are compared as deltas from instance 0's local pose, with x/z deltas wrapped into `[-torus/2, torus/2)` — an instance that wrapped across the domain edge must not skew the average by ±torus.
- **Sleep preservation:** if no instance of a canonical body is awake, skip it; if all instances agree within 1e-6 sim units, skip the write-back (a `b3Body_SetTransform` call may wake bodies and would block convergence forever).
- **Deterministic accumulation:** always sum in occurrence order 0..K-1. No parallelism, no reordering.

- [ ] **Step 1: Write the failing tests**

Append to `MatterEngine3/tests/tileset_physics_tests.cpp` (and call both from `main` after `test_settle_determinism`):

```cpp
static tileset::ColliderFit heavy_ball_fit() {
    tileset::ColliderFit f;
    f.type = tileset::ColliderType::Sphere;
    f.radius = 0.06f;
    f.volume = (4.0f / 3.0f) * 3.14159265f * 0.06f * 0.06f * 0.06f;
    return f;
}

// A canonical box instanced at two occurrence frames stays consistent even
// when only ONE instance is struck by a free obstacle: the interaction is
// transmitted through the portal, and finalize() snaps the poses exactly.
static void test_sync_group_instances_match() {
    const float T = 8.0f;
    tileset::HeightField hf = flat_field(T, 0.25f);
    tileset::SettleParams sp;
    tileset::SettleWorld sw(T, hf, sp);

    int g = sw.add_sync_group({ { 0, 0, 0, 0, 0, 0, 1 },
                                { 4, 0, 0, 0, 0, 0, 1 } });

    tileset::ColliderFit box = small_box_fit();
    tileset::ColliderFit ball = heavy_ball_fit();

    std::vector<tileset::BodySpawn> spawns;
    // Canonical box at strip-local (2.0, 0.06, 4.0):
    // instance 0 lands at world x=2, instance 1 at world x=6.
    for (int k = 0; k < 2; ++k) {
        tileset::BodySpawn b;
        b.collider = &box;
        b.start = { 2.0f + 4.0f * k, 0.06f, 4.0f, 0, 0, 0, 1 };
        b.sync_group = g;
        b.instance = k;
        spawns.push_back(b);
    }
    // Heavy ball slides in and strikes ONLY instance 0.
    tileset::BodySpawn ob;
    ob.collider = &ball;
    ob.density = 2000.0f;
    ob.start = { 1.5f, 0.07f, 4.0f, 0, 0, 0, 1 };
    ob.vx = 3.0f;
    spawns.push_back(ob);

    tileset::LayerResult r = sw.settle_layer(spawns);
    CHECK(r.converged, "sync: world with a sync group still converges");
    sw.finalize();

    const tileset::Pose& a = sw.poses()[0];   // instance 0
    const tileset::Pose& b = sw.poses()[1];   // instance 1

    // The untouched instance moved: proof the hit crossed the portal.
    bool moved = std::fabs(b.px - 6.0f) > 1e-3f || std::fabs(b.pz - 4.0f) > 1e-3f;
    CHECK(moved, "sync: untouched instance was displaced through the portal");

    // Poses identical modulo the occurrence translation (1e-4 m float slack).
    CHECK(std::fabs((b.px - 4.0f) - a.px) < 1e-4f &&
          std::fabs(b.py - a.py) < 1e-4f &&
          std::fabs(b.pz - a.pz) < 1e-4f,
          "sync: instance positions identical modulo occurrence translation");
    CHECK(a.qx == b.qx && a.qy == b.qy && a.qz == b.qz && a.qw == b.qw,
          "sync: instance rotations bit-identical after finalize snap");
}

// Same scenario twice -> identical pose hashes.
static void test_sync_determinism() {
    auto run = []() -> uint64_t {
        const float T = 8.0f;
        tileset::HeightField hf = flat_field(T, 0.25f);
        tileset::SettleParams sp;
        tileset::SettleWorld sw(T, hf, sp);
        int g = sw.add_sync_group({ { 0, 0, 0, 0, 0, 0, 1 },
                                    { 4, 0, 0, 0, 0, 0, 1 } });
        tileset::ColliderFit box = small_box_fit();
        tileset::ColliderFit ball = heavy_ball_fit();
        std::vector<tileset::BodySpawn> spawns;
        for (int k = 0; k < 2; ++k) {
            tileset::BodySpawn b;
            b.collider = &box;
            b.start = { 2.0f + 4.0f * k, 0.06f, 4.0f, 0, 0, 0, 1 };
            b.sync_group = g;
            b.instance = k;
            spawns.push_back(b);
        }
        tileset::BodySpawn ob;
        ob.collider = &ball;
        ob.density = 2000.0f;
        ob.start = { 1.5f, 0.07f, 4.0f, 0, 0, 0, 1 };
        ob.vx = 3.0f;
        spawns.push_back(ob);
        sw.settle_layer(spawns);
        sw.finalize();
        return sw.pose_hash();
    };
    uint64_t h1 = run(), h2 = run();
    CHECK(h1 == h2, "sync determinism: double-run pose hashes are identical");
}
```

- [ ] **Step 2: Run tests to verify the new ones fail**

Run: `make -C MatterEngine3/tests run-tilesetphysics`
Expected: FAIL — `sync: instance positions identical modulo occurrence translation` (the stub never syncs, so the struck instance diverges from the untouched one) and/or `sync: untouched instance was displaced`.

- [ ] **Step 3: Implement sync_groups_step and finalize**

Three edits to `MatterEngine3/src/tileset_settle.cpp`:

**(a)** In `struct SettleWorld::Impl`, change the declaration:

```cpp
    void sync_groups_step(bool force_snap);
```

**(b)** In `settle_layer`'s step loop, change the call to:

```cpp
        impl_->sync_groups_step(false);
```

**(c)** Replace the `sync_groups_step` stub and the `finalize` no-op:

```cpp
void SettleWorld::Impl::sync_groups_step(bool force_snap) {
    const float half = torus * 0.5f;
    for (SyncGroup& g : groups) {
        const int K = (int)g.frames.size();
        if (K < 2 || (int)g.members.size() < K) continue;
        const int canon_count = (int)g.members[0].size();
        for (int c = 0; c < canon_count; ++c) {
            bool any_awake = false;
            for (int k = 0; k < K; ++k)
                if (b3Body_IsAwake(bodies[g.members[k][c]].id)) { any_awake = true; break; }
            if (!any_awake && !force_snap) continue;

            // Reference: instance 0's strip-local pose.
            const b3BodyId id0 = bodies[g.members[0][c]].id;
            b3Pos p0 = b3Body_GetPosition(id0);
            b3Quat q0 = b3Body_GetRotation(id0);
            const float l0x = p0.x - g.frames[0].px;
            const float l0y = p0.y - g.frames[0].py;
            const float l0z = p0.z - g.frames[0].pz;

            float ax = 0, ay = 0, az = 0;               // avg local delta from l0
            float sqx = 0, sqy = 0, sqz = 0, sqw = 0;   // sign-aligned quat sum
            float vx = 0, vy = 0, vz = 0;               // avg linear velocity
            float wx = 0, wy = 0, wz = 0;               // avg angular velocity
            float max_dev = 0.0f;
            for (int k = 0; k < K; ++k) {
                const b3BodyId id = bodies[g.members[k][c]].id;
                b3Pos p = b3Body_GetPosition(id);
                b3Quat q = b3Body_GetRotation(id);
                float dx = (p.x - g.frames[k].px) - l0x;
                float dy = (p.y - g.frames[k].py) - l0y;
                float dz = (p.z - g.frames[k].pz) - l0z;
                // An instance may have wrapped across the torus edge.
                while (dx >  half) dx -= torus;
                while (dx < -half) dx += torus;
                while (dz >  half) dz -= torus;
                while (dz < -half) dz += torus;
                ax += dx; ay += dy; az += dz;
                float dot = q.v.x * q0.v.x + q.v.y * q0.v.y + q.v.z * q0.v.z + q.s * q0.s;
                float sgn = (dot < 0.0f) ? -1.0f : 1.0f;
                sqx += sgn * q.v.x; sqy += sgn * q.v.y; sqz += sgn * q.v.z; sqw += sgn * q.s;
                b3Vec3 lv = b3Body_GetLinearVelocity(id);
                b3Vec3 av = b3Body_GetAngularVelocity(id);
                vx += lv.x; vy += lv.y; vz += lv.z;
                wx += av.x; wy += av.y; wz += av.z;
                float dev = std::fabs(dx) + std::fabs(dy) + std::fabs(dz)
                          + std::fabs(sgn * q.v.x - q0.v.x) + std::fabs(sgn * q.v.y - q0.v.y)
                          + std::fabs(sgn * q.v.z - q0.v.z) + std::fabs(sgn * q.s - q0.s);
                if (dev > max_dev) max_dev = dev;
            }
            // Already in sync: skip the write-back so sleeping bodies stay
            // asleep (SetTransform may wake them and block convergence).
            if (!force_snap && max_dev < 1e-6f) continue;

            const float invK = 1.0f / (float)K;
            ax *= invK; ay *= invK; az *= invK;
            vx *= invK; vy *= invK; vz *= invK;
            wx *= invK; wy *= invK; wz *= invK;
            float qn = std::sqrt(sqx * sqx + sqy * sqy + sqz * sqz + sqw * sqw);
            b3Quat aq;
            if (qn < 1e-12f) { aq = q0; }
            else { aq.v.x = sqx / qn; aq.v.y = sqy / qn; aq.v.z = sqz / qn; aq.s = sqw / qn; }

            const float alx = l0x + ax, aly = l0y + ay, alz = l0z + az;
            for (int k = 0; k < K; ++k) {
                const b3BodyId id = bodies[g.members[k][c]].id;
                float nx = alx + g.frames[k].px;
                float ny = aly + g.frames[k].py;
                float nz = alz + g.frames[k].pz;
                while (nx < 0.0f)   nx += torus;
                while (nx >= torus) nx -= torus;
                while (nz < 0.0f)   nz += torus;
                while (nz >= torus) nz -= torus;
                b3Body_SetTransform(id, (b3Pos){ nx, ny, nz }, aq);
                b3Body_SetLinearVelocity(id, (b3Vec3){ vx, vy, vz });
                b3Body_SetAngularVelocity(id, (b3Vec3){ wx, wy, wz });
            }
        }
    }
}
```

```cpp
void SettleWorld::finalize() {
    // Exact snap: every instance gets the identical averaged local pose.
    impl_->sync_groups_step(true);
    // Freeze strips so free bodies can re-settle against fixed geometry.
    for (const Impl::TrackedBody& tb : impl_->bodies) {
        if (tb.group < 0) continue;
        b3Body_SetType(tb.id, b3_kinematicBody);
        b3Body_SetLinearVelocity(tb.id, (b3Vec3){ 0, 0, 0 });
        b3Body_SetAngularVelocity(tb.id, (b3Vec3){ 0, 0, 0 });
    }
    for (int i = 0; i < impl_->params.micro_relax_steps; ++i) {
        b3World_Step(impl_->world, impl_->params.dt, impl_->params.substeps);
        impl_->wrap_bodies();
    }
    impl_->refresh_poses();
}
```

- [ ] **Step 4: Run the tests**

Run: `make -C MatterEngine3/tests run-tilesetphysics`
Expected: all tests `ok:`, `PASSED (0 failures)`.

Debug guidance (fix root causes, don't loosen assertions):
- Compile error on `b3Body_GetLinearVelocity` / `b3Body_SetAngularVelocity` / `b3Body_SetType` → grep `Libraries/box3d/include/box3d/box3d.h` for `Velocity` and `SetType`; box3d may use slightly different names (e.g. `b3Body_GetLinearVelocity` vs `b3Body_GetVelocity`). Use whatever the header declares; the semantics needed are get/set of linear + angular velocity and dynamic→kinematic type change.
- `sync: world with a sync group still converges` fails (times out) → the write-back is waking bodies every step. Verify the `max_dev < 1e-6f` skip runs BEFORE any `b3Body_Set*` call, and that `dev` includes the quaternion terms.
- `untouched instance was displaced` fails → the ball never reached the box (check `ob.vx` is scaled by `S` in `settle_layer` — Task 4 does this) or the sync loop iterated zero canonical bodies (check `g.members[0].size()` — members must be filled per-instance in spawn order).
- Positions match but rotations differ bit-wise → `finalize()` must call `sync_groups_step(true)` (force_snap) so the skip branch cannot bypass the final write.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/tileset_settle.cpp MatterEngine3/tests/tileset_physics_tests.cpp
git commit -m "feat: portal-sync groups with pose averaging and kinematic finalize"
```

---

### Task 6: Layered end-to-end fixture (the Phase 2 usage pattern, as a test)

**Files:**
- Modify: `MatterEngine3/tests/tileset_physics_tests.cpp` (one acceptance test; no production code changes)

**Interfaces:**
- Consumes: the complete Task 4+5 API, exercised exactly the way Phase 2's bake driver will drive it: construct → `add_sync_group` → `settle_layer` (rocks) → `settle_layer` (twigs) → `finalize` → `poses()` / `pose_hash()`.
- Produces: nothing new — this is the acceptance gate for Phase 1. If this test passes, Phase 2 can build on the physics core without revisiting it.

This task is test-only. It can still fail — that means a Task 4/5 bug slipped through the narrower tests, and the fix belongs in `tileset_settle.cpp`.

- [ ] **Step 1: Write the acceptance test**

Append to `MatterEngine3/tests/tileset_physics_tests.cpp` (and call it from `main` last):

```cpp
static tileset::ColliderFit twig_fit() {
    tileset::ColliderFit f;
    f.type = tileset::ColliderType::Capsule;
    f.axis[0][0] = 1.0f; f.axis[0][1] = 0.0f; f.axis[0][2] = 0.0f;  // segment dir
    f.radius = 0.02f;
    f.seg_half = 0.08f;
    f.volume = 3.14159265f * 0.02f * 0.02f * 0.16f
             + (4.0f / 3.0f) * 3.14159265f * 0.02f * 0.02f * 0.02f;
    return f;
}

// Toroidal min-image distance between two points on the [0,T)^2 ground plane.
static float torus_dist(const tileset::Pose& a, const tileset::Pose& b, float T) {
    float dx = std::fabs(a.px - b.px); if (dx > T * 0.5f) dx = T - dx;
    float dz = std::fabs(a.pz - b.pz); if (dz > T * 0.5f) dz = T - dz;
    float dy = a.py - b.py;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// End-to-end: two layers (boxes then capsules) + a sync group, finalize,
// and the invariants Phase 2 depends on.
static void test_layered_end_to_end() {
    const float T = 8.0f;

    auto run = [&](std::vector<tileset::Pose>* out) -> uint64_t {
        tileset::HeightField hf = flat_field(T, 0.25f);
        tileset::SettleParams sp;
        tileset::SettleWorld sw(T, hf, sp);
        int g = sw.add_sync_group({ { 0, 0, 0, 0, 0, 0, 1 },
                                    { 4, 0, 0, 0, 0, 0, 1 } });

        tileset::ColliderFit box = small_box_fit();
        tileset::ColliderFit twig = twig_fit();

        // Layer 1: 40 free boxes + 1 canonical box instanced twice.
        std::vector<tileset::BodySpawn> layer1;
        uint64_t s = 424242;
        for (int i = 0; i < 40; ++i) {
            tileset::BodySpawn b;
            b.collider = &box;
            b.start = { phys_unit(s) * T, 0.3f + 0.5f * phys_unit(s), phys_unit(s) * T,
                        0, 0, 0, 1 };
            layer1.push_back(b);
        }
        for (int k = 0; k < 2; ++k) {
            tileset::BodySpawn b;
            b.collider = &box;
            b.start = { 1.0f + 4.0f * k, 0.2f, 2.0f, 0, 0, 0, 1 };
            b.sync_group = g;
            b.instance = k;
            layer1.push_back(b);
        }
        tileset::LayerResult r1 = sw.settle_layer(layer1);
        CHECK(r1.converged, "e2e: layer 1 (boxes) converges");

        // Layer 2: 20 capsules dropped onto the settled boxes.
        std::vector<tileset::BodySpawn> layer2;
        for (int i = 0; i < 20; ++i) {
            tileset::BodySpawn b;
            b.collider = &twig;
            b.density = 500.0f;
            b.start = { phys_unit(s) * T, 0.4f + 0.4f * phys_unit(s), phys_unit(s) * T,
                        0, 0, 0, 1 };
            layer2.push_back(b);
        }
        tileset::LayerResult r2 = sw.settle_layer(layer2);
        CHECK(r2.converged, "e2e: layer 2 (capsules) converges");

        sw.finalize();
        if (out) *out = sw.poses();
        return sw.pose_hash();
    };

    std::vector<tileset::Pose> poses;
    uint64_t h1 = run(&poses);

    // 62 poses in spawn order: 40 boxes, 2 sync instances, 20 capsules.
    CHECK(poses.size() == 62, "e2e: one pose per spawned body, in order");

    bool in_range = true, grounded = true;
    for (const tileset::Pose& p : poses) {
        if (p.px < 0.0f || p.px >= T || p.pz < 0.0f || p.pz >= T) in_range = false;
        if (p.py < 0.015f || p.py > 0.5f) grounded = false;  // min radius 0.02
    }
    CHECK(in_range, "e2e: every body inside the torus domain");
    CHECK(grounded, "e2e: no body tunneled below ground or floated away");

    // No guaranteed interpenetration among the boxes: two 0.05-half boxes
    // whose centers are closer than 0.1 - slop MUST overlap (inscribed
    // spheres). Slop budget: B3_LINEAR_SLOP 0.005 sim units / sim_scale 4.
    bool separated = true;
    for (int i = 0; i < 42 && separated; ++i)
        for (int j = i + 1; j < 42; ++j)
            if (torus_dist(poses[i], poses[j], T) < 0.095f) { separated = false; break; }
    CHECK(separated, "e2e: no pair of boxes is inside the guaranteed-overlap radius");

    // Sync invariant survives the full pipeline.
    const tileset::Pose& a = poses[40];
    const tileset::Pose& b = poses[41];
    CHECK(std::fabs((b.px - 4.0f) - a.px) < 1e-4f &&
          std::fabs(b.py - a.py) < 1e-4f &&
          std::fabs(b.pz - a.pz) < 1e-4f &&
          a.qw == b.qw && a.qx == b.qx && a.qy == b.qy && a.qz == b.qz,
          "e2e: sync instances still identical modulo occurrence translation");

    // Whole-pipeline determinism.
    uint64_t h2 = run(nullptr);
    CHECK(h1 == h2, "e2e: full two-layer pipeline is deterministic");
}
```

- [ ] **Step 2: Run the full suite**

Run: `make -C MatterEngine3/tests run-tilesetphysics`
Expected: all tests `ok:`, `PASSED (0 failures)`. This test exercises only existing code, so it may pass first try; a failure here is a real Task 4/5 bug.

Debug guidance:
- `layer 2 converges` fails → dropping capsules wakes the settled boxes and the convergence gate counts ALL bodies (correct per Task 4); if the pile never re-sleeps within `max_sim_time`, the boxes are jittering — check the sync skip (Task 5 Step 4 guidance) before touching `max_sim_time`.
- `no pair of boxes...` fails → real penetration. Check that the capsule/box shapes are created about the collider's local center (`c.center` offsets in `settle_layer`), not double-offset by the body position.
- `sync instances still identical` fails only in this test → an earlier layer's sleep state was disturbed by layer 2 near one instance but not the other; that is exactly what sync_groups_step must handle — verify the `any_awake` check spans ALL instances of the canonical body.
- Wrap-region flakiness (`in_range` fails with px exactly == T) → floating-point: `while (nx >= torus) nx -= torus;` can produce `nx == torus` after subtraction underflow; if hit, clamp `if (nx >= torus) nx = 0.0f;` after the loop in `wrap_bodies`.

- [ ] **Step 3: Run the whole engine build to confirm nothing else broke**

Run: `./build-all.sh test 2>&1 | tail -20`
Expected: all suites pass, including the new `tileset_physics_tests`.

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/tests/tileset_physics_tests.cpp
git commit -m "test: layered settle end-to-end fixture (phase 1 acceptance gate)"
```

---

## Verification

After all tasks: `make -C MatterEngine3/tests run-tilesetphysics` green, `./build-all.sh test` green, and `git log --oneline -6` shows the six task commits. Phase 1 is done; Phase 2 (Tileset DSL root + placement) gets its own plan.

