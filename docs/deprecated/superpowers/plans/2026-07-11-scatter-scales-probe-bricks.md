# Scatter Scales + Tiered Per-Sector Probe Bricks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Noise-shaped scale variation (trees 1–3x, grass 1–5x), 5x grass density, and restored baked lighting in the infinite streamed MeadowWorld via per-sector probe bricks composited into the existing single-volume probe shader path.

**Architecture:** Scatter changes are pure JS schema edits. Probes: a GL-free `probe_bricks::BrickStore` holds one small `ProbeVolume` per resident sector (4m cells near ring, 8m mid ring, none far), baked on a dedicated engine background thread with `probe_bake::bake_probes` over a `world_tracer::WorldTracer` built from resident sectors; a compositor resamples resident bricks into one camera-centered window volume that the unchanged `probe_texture` + raster shader path consumes.

**Tech Stack:** C++17, QuickJS DSL schemas, raylib/GL 4.6, existing MatterEngine3 subsystems (probe_bake, world_tracer, probe_volume, probe_texture, raster_composer).

**Spec:** `docs/superpowers/specs/2026-07-11-scatter-scales-probe-bricks-design.md`

## Global Constraints

- Worktree root: `/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.claude/worktrees/phase-c-explorer-demo` — run everything from inside it; never cd to the original repo.
- Every GPU/viewer/smoke run: `GALLIUM_DRIVER=d3d12` (WSLg; without it Mesa falls back to llvmpipe GL 4.5 and FATALs).
- NEVER run two test suites in parallel (each 12–15GB; parallel runs OOM-crash WSL2). Chain sequentially.
- After any engine change, rebuild the Windows binary: `make -C ExplorerDemo windows` (standing user requirement).
- Terrain policy: NO skirts/overlap strips; terrain always meshes at voxel rung 0. Nothing in this plan may touch terrain meshing.
- `p.rung` on WorldSector is a SCATTER DETAIL TIER (2=near adds grass, 1=mid adds rocks/pebbles, 0=far trees+boulders only), not a mesh resolution.
- Shader files: `MatterEngine3/shaders_gpu/*` are embedded into a header by `make -C MatterEngine3`; the probe raster shader is NOT modified by this plan at all.
- Commit after every task (small, described commits; no --no-verify).

---

### Task 1: Noise-shaped scale distributions (trees 1–3x, grass 1–5x)

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/WorldSector.js` (tree block ~line 100–113, grass block ~line 148–160)

**Interfaces:**
- Consumes: existing `patch()` channels `GROVE`/`TUFT`, candidate fields `c.v`, per-sector rng `r`.
- Produces: nothing consumed by later tasks (self-contained schema change).

Background: trees currently use `const s = 0.9 + 0.5 * c.v;` (0.9–1.4x); grass uses `put('Grass', {...}, wx, wz, r.range(0.8, 1.3), 0.02);`. Requirement: trees 1–3x and grass 1–5x with a long-tail distribution biased by the SAME patch channel that gated placement (grove/tuft strength). All inputs must stay tier-independent (c.v and patch values are; `r` draw ORDER inside the grass loop must stay consistent for determinism within a bake).

- [ ] **Step 1: Edit the tree scale**

In the tree loop of `WorldSector.js`, `g` (grove patch value) is already computed before the slope check. Replace:

```js
      const s = 0.9 + 0.5 * c.v;
```

with:

```js
      // Scale 1..3, long-tail: raw blends candidate jitter with grove
      // strength so giants only appear deep in grove cores.
      const gN = Math.min(1, Math.max(0, (g - 0.10) / 0.90));
      const s  = 1 + 2 * Math.pow(0.65 * c.v + 0.35 * gN, 1.7);
```

- [ ] **Step 2: Edit the grass scale**

In the tier-2 grass loop, replace:

```js
      if (patch(wx, wz, TUFT, 1 / 30) < -0.05) continue;
      if (this.slopeAt(wx, wz) > GRASS_SLOPE_MAX && r.random() < 0.7) continue;
      put('Grass', { seed: r.int(GRASS_VARIANTS) }, wx, wz, r.range(0.8, 1.3), 0.02);
```

with:

```js
      const t = patch(wx, wz, TUFT, 1 / 30);
      if (t < -0.05) continue;
      if (this.slopeAt(wx, wz) > GRASS_SLOPE_MAX && r.random() < 0.7) continue;
      // Scale 1..5, long-tail: mostly 1-2x, rare 4-5x clumps at tuft cores.
      const tuftN = Math.min(1, Math.max(0, (t + 0.05) / 1.05));
      const gs = 1 + 4 * Math.pow(r.random(), 2.5) * (0.5 + 0.5 * tuftN);
      put('Grass', { seed: r.int(GRASS_VARIANTS) }, wx, wz, gs, 0.02);
```

Note the `r.random()` for scale is drawn AFTER `r.int(...)`? No — argument evaluation order in JS is left-to-right: `r.int(GRASS_VARIANTS)` is inside the `put(...)` call, and `gs` is computed BEFORE the call. Keep exactly the order shown above (gs computed on its own line before `put`), so the draw order is deterministic and identical every bake.

- [ ] **Step 3: Run the sector bake suite**

```bash
cd MatterEngine3/tests && make run-sectorbake
```

Expected: ALL PASS (suite checks requires-list size 30, bake determinism, hash sensitivity — none depend on scale values, but they DO exercise the edited build() end-to-end; a JS syntax error fails here).

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/WorldSector.js
git commit -m "feat(world): noise-shaped scale distributions — trees 1-3x by grove strength, grass 1-5x by tuft strength"
```

---

### Task 2: 5x grass density

**Files:**
- Modify: `MatterEngine3/examples/world_demo/schemas/MeadowWorld.js:25-26`

**Interfaces:** none (data-only change; sector_bake_tests use their own biome table).

- [ ] **Step 1: Edit biome counts**

In `MeadowWorld.js` `biomes()`, change:

```js
      meadow:    { grass: 600, pebbles: 64, rocks: 8, trees: 6 },
      foothills: { grass: 160, rocks: 10, trees: 10 },
```

to:

```js
      meadow:    { grass: 3000, pebbles: 64, rocks: 8, trees: 6 },
      foothills: { grass: 800, rocks: 10, trees: 10 },
```

- [ ] **Step 2: Run the sector bake suite**

```bash
cd MatterEngine3/tests && make run-sectorbake
```

Expected: ALL PASS.

- [ ] **Step 3: Commit**

```bash
git add MatterEngine3/examples/world_demo/schemas/MeadowWorld.js
git commit -m "feat(world): 5x grass density (meadow 3000, foothills 800)"
```

---

### Task 3: probe_bricks — BrickStore + trilinear sampler + window compositor (TDD)

**Files:**
- Create: `MatterEngine3/src/probe_bricks.h`
- Create: `MatterEngine3/src/probe_bricks.cpp`
- Create: `MatterEngine3/tests/probe_brick_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile` (new `run-probebrick` target; mirror an existing GL-free target's pattern, e.g. the one for `run-graph`)
- Modify: `MatterEngine3/Makefile` (add `probe_bricks.o` to the library objects, next to `probe_volume.o`)

**Interfaces:**
- Consumes: `probe_volume::ProbeVolume` / `ProbeGrid` (probe_volume.h — grid origin is the CENTER of cell (0,0,0); layout x-fastest `idx = ((z*ny)+y)*nx + x`, 4 floats/cell in `ambient` and `dominant`); `world_lights::WorldLights` (sky_color).
- Produces (used verbatim by Task 5):
  - `probe_bricks::WindowParams { float cell = 4.0f; int nx = 64, ny = 32, nz = 64; }`
  - `void BrickStore::put(int64_t tx, int64_t tz, probe_volume::ProbeVolume brick)` (replaces existing; thread-safe)
  - `bool BrickStore::erase(int64_t tx, int64_t tz)` — returns true if a brick was removed
  - `bool BrickStore::has(int64_t tx, int64_t tz) const`
  - `void BrickStore::clear()`
  - `size_t BrickStore::count() const`
  - `size_t BrickStore::composite(float cx, float cy, float cz, const world_lights::WorldLights& lights, float sector_size, const WindowParams& wp, probe_volume::ProbeVolume& out) const` — returns covered-cell count
  - `bool sample_volume(const probe_volume::ProbeVolume& v, const float wp[3], float ambient_out[4], float dominant_out[4])`

- [ ] **Step 1: Write the header**

`MatterEngine3/src/probe_bricks.h`:

```cpp
#pragma once
// GL-free per-sector probe brick store + camera-window compositor for the
// infinite streamed world. Bricks are baked per resident sector (cell size by
// streamer ring) and composited into ONE window ProbeVolume so the existing
// probe_texture upload + raster shader probe path is consumed unchanged.
#include "probe_volume.h"
#include "world_lights.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

namespace probe_bricks {

// Camera-centred window: 64x32x64 cells at 4m = 256x128x256 metres.
struct WindowParams {
    float cell = 4.0f;
    int   nx = 64, ny = 32, nz = 64;
};

// Trilinear sample of a ProbeVolume at a world position, clamped to the grid
// (grid.origin is the CENTER of cell (0,0,0)). Plain component-wise lerp for
// both volumes — matches the GL_LINEAR filtering the shader applies, so CPU
// resampling introduces no direction renormalization the GPU wouldn't.
// Returns false when v is invalid.
bool sample_volume(const probe_volume::ProbeVolume& v, const float world_pos[3],
                   float ambient_out[4], float dominant_out[4]);

class BrickStore {
public:
    void   put(int64_t tx, int64_t tz, probe_volume::ProbeVolume brick);
    bool   erase(int64_t tx, int64_t tz);   // true if a brick was removed
    bool   has(int64_t tx, int64_t tz) const;
    void   clear();
    size_t count() const;

    // Build the window volume centred on (cx,cy,cz) — centre snapped to whole
    // cells so re-centres don't swim. Each window cell samples the brick of
    // the sector containing its centre (floor(x / sector_size)); cells with
    // no brick get ambient = lights.sky_color, sun_vis = 1, dominant = 0.
    // Returns the number of cells covered by a brick (0 = don't bother
    // uploading).
    size_t composite(float cx, float cy, float cz,
                     const world_lights::WorldLights& lights,
                     float sector_size, const WindowParams& wp,
                     probe_volume::ProbeVolume& out) const;

private:
    mutable std::mutex mu_;
    std::map<std::pair<int64_t, int64_t>, probe_volume::ProbeVolume> bricks_;
};

} // namespace probe_bricks
```

- [ ] **Step 2: Write the failing tests**

`MatterEngine3/tests/probe_brick_tests.cpp` (uses the existing `check.h` CHECK/check_summary pattern — see `sector_bake_tests.cpp`):

```cpp
// MatterEngine3/tests/probe_brick_tests.cpp
#include "check.h"
#include "../src/probe_bricks.h"
#include <cmath>

using probe_volume::ProbeVolume;

// Constant-valued brick covering sector (tx,tz): sector_size m in x/z from
// tx*S, vertical y0..y0+ny*cell, given cell size.
static ProbeVolume make_brick(int64_t tx, int64_t tz, float S, float cell,
                              float y0, float ar, float ag, float ab, float sun) {
    ProbeVolume v;
    v.grid.cell = cell;
    v.grid.nx = (int)std::lround(S / cell);
    v.grid.nz = (int)std::lround(S / cell);
    v.grid.ny = 8;
    // origin = centre of cell (0,0,0)
    v.grid.origin[0] = (float)tx * S + cell * 0.5f;
    v.grid.origin[1] = y0 + cell * 0.5f;
    v.grid.origin[2] = (float)tz * S + cell * 0.5f;
    size_t n = v.cells();
    v.ambient.assign(n * 4, 0.0f);
    v.dominant.assign(n * 4, 0.0f);
    for (size_t i = 0; i < n; ++i) {
        v.ambient[i*4+0] = ar; v.ambient[i*4+1] = ag;
        v.ambient[i*4+2] = ab; v.ambient[i*4+3] = sun;
        v.dominant[i*4+1] = 1.0f;   // dir +y
        v.dominant[i*4+3] = 0.5f;   // intensity
    }
    return v;
}

int main() {
    const float S = 64.0f;
    world_lights::WorldLights lights;   // sky_color {0.38,0.43,0.52}

    // ---- sample_volume: trilinear between two known cells ----
    {
        ProbeVolume v;
        v.grid.cell = 2.0f; v.grid.nx = 2; v.grid.ny = 1; v.grid.nz = 1;
        v.grid.origin[0] = 0; v.grid.origin[1] = 0; v.grid.origin[2] = 0;
        v.ambient = {0,0,0,0,  1,1,1,1};       // cell0 black, cell1 white
        v.dominant = {0,0,0,0, 0,0,0,0};
        float a[4], d[4];
        float mid[3] = {1.0f, 0.0f, 0.0f};     // halfway between cell centres
        CHECK(probe_bricks::sample_volume(v, mid, a, d), "sample ok");
        CHECK(std::fabs(a[0] - 0.5f) < 1e-4f, "trilinear midpoint = 0.5");
        float lo[3] = {-50.0f, 0.0f, 0.0f};    // clamps to cell 0
        probe_bricks::sample_volume(v, lo, a, d);
        CHECK(a[0] < 1e-4f, "clamped to first cell");
        ProbeVolume bad;
        CHECK(!probe_bricks::sample_volume(bad, mid, a, d), "invalid volume rejected");
    }

    probe_bricks::BrickStore store;
    probe_bricks::WindowParams wp;      // 64x32x64 @ 4m
    ProbeVolume win;

    // ---- empty store: full fallback, 0 covered ----
    {
        size_t covered = store.composite(32, 20, 32, lights, S, wp, win);
        CHECK(covered == 0, "empty store covers nothing");
        CHECK(win.valid(), "window still a valid volume");
        CHECK(win.grid.nx == wp.nx && win.grid.ny == wp.ny && win.grid.nz == wp.nz,
              "window dims match params");
        // every cell = sky fallback
        CHECK(std::fabs(win.ambient[0] - lights.sky_color[0]) < 1e-4f, "fallback r");
        CHECK(std::fabs(win.ambient[3] - 1.0f) < 1e-4f, "fallback sun_vis = 1");
        CHECK(std::fabs(win.dominant[3]) < 1e-4f, "fallback dominant intensity 0");
    }

    // ---- one red brick at sector (0,0): inside red, outside fallback ----
    {
        store.put(0, 0, make_brick(0, 0, S, 4.0f, 0.0f, 1, 0, 0, 0.25f));
        CHECK(store.has(0, 0), "has after put");
        CHECK(store.count() == 1, "count 1");
        size_t covered = store.composite(32, 16, 32, lights, S, wp, win);
        CHECK(covered > 0, "some cells covered");

        auto cell_at = [&](float x, float y, float z, float out[4]) {
            int ix = (int)std::floor((x - win.grid.origin[0]) / win.grid.cell + 0.5f);
            int iy = (int)std::floor((y - win.grid.origin[1]) / win.grid.cell + 0.5f);
            int iz = (int)std::floor((z - win.grid.origin[2]) / win.grid.cell + 0.5f);
            size_t idx = (((size_t)iz * win.grid.ny) + iy) * win.grid.nx + ix;
            for (int k = 0; k < 4; ++k) out[k] = win.ambient[idx*4 + k];
        };
        float a[4];
        cell_at(30, 16, 30, a);                    // inside sector (0,0), inside brick y
        CHECK(std::fabs(a[0] - 1.0f) < 1e-3f && a[1] < 1e-3f, "inside brick = red");
        CHECK(std::fabs(a[3] - 0.25f) < 1e-3f, "inside brick sun_vis");
        cell_at(-30, 16, 30, a);                   // sector (-1,0): no brick
        CHECK(std::fabs(a[0] - lights.sky_color[0]) < 1e-3f, "outside brick = fallback");
    }

    // ---- mixed densities: 8m green brick at sector (1,0) ----
    {
        store.put(1, 0, make_brick(1, 0, S, 8.0f, 0.0f, 0, 1, 0, 1.0f));
        size_t covered = store.composite(64, 16, 32, lights, S, wp, win);
        CHECK(covered > 0, "covered with two bricks");
        float a[4], d[4];
        float p0[3] = {30, 16, 30}, p1[3] = {96, 16, 30};
        probe_bricks::sample_volume(win, p0, a, d);
        CHECK(a[0] > 0.9f, "sector 0 still red");
        probe_bricks::sample_volume(win, p1, a, d);
        CHECK(a[1] > 0.9f, "sector 1 green via 8m brick");
    }

    // ---- window centre snapping: re-centres move by whole cells ----
    {
        ProbeVolume w1, w2, w3;
        store.composite(32.0f, 16.0f, 32.0f, lights, S, wp, w1);
        store.composite(33.9f, 16.7f, 31.2f, lights, S, wp, w2);   // same 4m cell
        CHECK(std::fabs(w2.grid.origin[0] - w1.grid.origin[0]) < 1e-3f,
              "same snapped cell -> identical origin");
        store.composite(32.0f + 3.0f * wp.cell, 16.0f, 32.0f, lights, S, wp, w3);
        float frac = (w3.grid.origin[0] - w1.grid.origin[0]) / wp.cell;
        CHECK(std::fabs(frac - std::round(frac)) < 1e-3f, "origin delta is integer cells");
        CHECK(std::fabs(frac - 3.0f) < 1e-3f, "moved exactly 3 cells");
    }

    // ---- erase / clear ----
    {
        store.erase(0, 0);
        CHECK(!store.has(0, 0), "erased");
        CHECK(store.count() == 1, "one left");
        store.clear();
        CHECK(store.count() == 0, "cleared");
    }

    return check_summary();
}
```

- [ ] **Step 3: Add the Makefile target and verify the test FAILS to build**

In `MatterEngine3/tests/Makefile`, copy the pattern of an existing GL-free suite (find the target that builds `graph` / `run-graph`) to add `probe_brick_tests` compiling `probe_brick_tests.cpp` + `../src/probe_bricks.cpp` + `../src/probe_volume.cpp` (+ whatever the GL-free suites already link — mirror includes/flags exactly), and a `run-probebrick` target. Add `run-probebrick` to the suite aggregate list if one exists (look for how `run-sectorbake` is registered).

```bash
cd MatterEngine3/tests && make run-probebrick
```

Expected: FAILS — `probe_bricks.cpp` doesn't exist yet.

- [ ] **Step 4: Implement `probe_bricks.cpp`**

```cpp
// probe_bricks.cpp — see probe_bricks.h.
#include "probe_bricks.h"
#include <algorithm>
#include <cmath>

namespace probe_bricks {

bool sample_volume(const probe_volume::ProbeVolume& v, const float wp[3],
                   float ambient_out[4], float dominant_out[4]) {
    if (!v.valid()) return false;
    const auto& g = v.grid;
    // Continuous cell coords (origin = centre of cell 0).
    float fx = (wp[0] - g.origin[0]) / g.cell;
    float fy = (wp[1] - g.origin[1]) / g.cell;
    float fz = (wp[2] - g.origin[2]) / g.cell;
    fx = std::clamp(fx, 0.0f, (float)(g.nx - 1));
    fy = std::clamp(fy, 0.0f, (float)(g.ny - 1));
    fz = std::clamp(fz, 0.0f, (float)(g.nz - 1));
    int x0 = (int)fx, y0 = (int)fy, z0 = (int)fz;
    int x1 = std::min(x0 + 1, g.nx - 1);
    int y1 = std::min(y0 + 1, g.ny - 1);
    int z1 = std::min(z0 + 1, g.nz - 1);
    float tx = fx - x0, ty = fy - y0, tz = fz - z0;

    auto idx = [&](int x, int y, int z) {
        return ((((size_t)z * g.ny) + y) * g.nx + x) * 4;
    };
    auto lerp_fetch = [&](const std::vector<float>& src, float out[4]) {
        for (int k = 0; k < 4; ++k) {
            float c000 = src[idx(x0,y0,z0)+k], c100 = src[idx(x1,y0,z0)+k];
            float c010 = src[idx(x0,y1,z0)+k], c110 = src[idx(x1,y1,z0)+k];
            float c001 = src[idx(x0,y0,z1)+k], c101 = src[idx(x1,y0,z1)+k];
            float c011 = src[idx(x0,y1,z1)+k], c111 = src[idx(x1,y1,z1)+k];
            float c00 = c000 + (c100 - c000) * tx;
            float c10 = c010 + (c110 - c010) * tx;
            float c01 = c001 + (c101 - c001) * tx;
            float c11 = c011 + (c111 - c011) * tx;
            float c0 = c00 + (c10 - c00) * ty;
            float c1 = c01 + (c11 - c01) * ty;
            out[k] = c0 + (c1 - c0) * tz;
        }
    };
    lerp_fetch(v.ambient,  ambient_out);
    lerp_fetch(v.dominant, dominant_out);
    return true;
}

void BrickStore::put(int64_t tx, int64_t tz, probe_volume::ProbeVolume brick) {
    std::lock_guard<std::mutex> lk(mu_);
    bricks_[{tx, tz}] = std::move(brick);
}
bool BrickStore::erase(int64_t tx, int64_t tz) {
    std::lock_guard<std::mutex> lk(mu_);
    return bricks_.erase({tx, tz}) != 0;
}
bool BrickStore::has(int64_t tx, int64_t tz) const {
    std::lock_guard<std::mutex> lk(mu_);
    return bricks_.count({tx, tz}) != 0;
}
void BrickStore::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    bricks_.clear();
}
size_t BrickStore::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return bricks_.size();
}

size_t BrickStore::composite(float cx, float cy, float cz,
                             const world_lights::WorldLights& lights,
                             float sector_size, const WindowParams& wp,
                             probe_volume::ProbeVolume& out) const {
    // Snap centre to whole cells so re-centres shift by integral cells.
    auto snap = [&](float v) { return std::floor(v / wp.cell) * wp.cell; };
    out.grid.cell = wp.cell;
    out.grid.nx = wp.nx; out.grid.ny = wp.ny; out.grid.nz = wp.nz;
    out.grid.origin[0] = snap(cx) - wp.cell * (wp.nx / 2) + wp.cell * 0.5f;
    out.grid.origin[1] = snap(cy) - wp.cell * (wp.ny / 2) + wp.cell * 0.5f;
    out.grid.origin[2] = snap(cz) - wp.cell * (wp.nz / 2) + wp.cell * 0.5f;
    const size_t n = out.cells();
    out.ambient.resize(n * 4);
    out.dominant.resize(n * 4);

    std::lock_guard<std::mutex> lk(mu_);
    size_t covered = 0;
    size_t i = 0;
    for (int z = 0; z < wp.nz; ++z)
    for (int y = 0; y < wp.ny; ++y)
    for (int x = 0; x < wp.nx; ++x, ++i) {
        float wpos[3] = { out.grid.origin[0] + x * wp.cell,
                          out.grid.origin[1] + y * wp.cell,
                          out.grid.origin[2] + z * wp.cell };
        int64_t stx = (int64_t)std::floor(wpos[0] / sector_size);
        int64_t stz = (int64_t)std::floor(wpos[2] / sector_size);
        auto it = bricks_.find({stx, stz});
        float* amb = &out.ambient[i * 4];
        float* dom = &out.dominant[i * 4];
        if (it != bricks_.end() &&
            sample_volume(it->second, wpos, amb, dom)) {
            ++covered;
        } else {
            amb[0] = lights.sky_color[0];
            amb[1] = lights.sky_color[1];
            amb[2] = lights.sky_color[2];
            amb[3] = 1.0f;                       // full sun visibility
            dom[0] = dom[1] = dom[2] = dom[3] = 0.0f;
        }
    }
    return covered;
}

} // namespace probe_bricks
```

Also add `probe_bricks.o` to the kernel library object list in `MatterEngine3/Makefile` (next to `probe_volume.o` — grep for it).

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd MatterEngine3/tests && make run-probebrick
```

Expected: `--- Results: N/N passed --- ALL PASS`.

- [ ] **Step 6: Verify the library still builds**

```bash
make -C MatterEngine3 -j8
```

Expected: exit 0.

- [ ] **Step 7: Commit**

```bash
git add MatterEngine3/src/probe_bricks.h MatterEngine3/src/probe_bricks.cpp \
        MatterEngine3/tests/probe_brick_tests.cpp MatterEngine3/tests/Makefile \
        MatterEngine3/Makefile
git commit -m "feat(probe): GL-free per-sector probe BrickStore + trilinear window compositor with tests"
```

---

### Task 4: WorldTracer scratch-dir support (streamed sector parts)

**Files:**
- Modify: `MatterEngine3/src/world_tracer.h` (public setter)
- Modify: `MatterEngine3/src/world_tracer.cpp` (`load_part` path resolution, ~lines 214–244)

**Interfaces:**
- Produces (used by Task 5): `void world_tracer::WorldTracer::set_scratch_dir(const std::string& dir);` — must be called BEFORE `build()`.

Background: streamed sector `.part` files live under the provider's transient dir, not `cache_root`. `PartStore` already handles this: `resolve_artifact_path` in `part_store.cpp:71-82` checks `scratch_dir + "/" + part_asset::cache_path_resolved(hash)` first, then `cache_root`. The engine sets it with `store->set_scratch_dir(provider->transient_dir())` (matter_engine.cpp:810, 1850). WorldTracer must resolve paths IDENTICALLY or brick bakes will trace a world with no terrain.

- [ ] **Step 1: Add the setter to world_tracer.h**

After the `build(...)` declaration in the `WorldTracer` class:

```cpp
    // Optional secondary artifact dir (streamed transient parts). Checked
    // FIRST, exactly like PartStore's scratch dir (same path construction:
    // scratch + "/" + cache_path_flat/_resolved). Set before build().
    void set_scratch_dir(const std::string& dir);
```

- [ ] **Step 2: Implement in world_tracer.cpp**

Store `std::string scratch_dir_;` on the Impl (or the class front-object — follow where `build` stores `cache_root`; the setter forwards into the Impl). In `load_part` (~line 220), replace:

```cpp
        const std::string flat_path =
            cache_root + "/" + part_asset::cache_path_flat(hash);
        const std::string comp_path =
            cache_root + "/" + part_asset::cache_path_resolved(hash);
```

with:

```cpp
        auto file_exists = [](const std::string& p) {
            struct stat st; return ::stat(p.c_str(), &st) == 0;
        };
        std::string flat_path = cache_root + "/" + part_asset::cache_path_flat(hash);
        std::string comp_path = cache_root + "/" + part_asset::cache_path_resolved(hash);
        if (!scratch_dir_.empty()) {
            const std::string sf = scratch_dir_ + "/" + part_asset::cache_path_flat(hash);
            const std::string sc = scratch_dir_ + "/" + part_asset::cache_path_resolved(hash);
            if (file_exists(sf)) flat_path = sf;
            if (file_exists(sc)) comp_path = sc;
        }
```

Add `#include <sys/stat.h>` if not present (part_store.cpp already uses `::stat` on both platforms — mirror its include set).

- [ ] **Step 3: Build**

```bash
make -C MatterEngine3 -j8
```

Expected: exit 0. (No dedicated unit test: exercising this requires a baked transient sector fixture; end-to-end coverage lands in Task 6's flight smoke, where bricks with zero terrain hits would produce fully-lit probes and the visual/log gate would show `covered` cells but sun_vis≈1 everywhere. Compile + Task 6 is the verification.)

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/src/world_tracer.h MatterEngine3/src/world_tracer.cpp
git commit -m "feat(tracer): scratch-dir fallback so WorldTracer can load streamed transient sector parts"
```

---

### Task 5: Engine wiring — brick bake thread, stream hooks, GL window upload

**Files:**
- Modify: `MatterEngine3/src/matter_engine.cpp` (Impl members ~line 413; stream publish job ~line 2218; both eviction lambdas ~lines 2017–2043 and 2080–2107; streamer creation/teardown sites; `WorldSession::render` GpuDriven branch ~line 2766 and stats refresh ~2709; `frame_stats()` ~line 2832)
- Modify: `MatterEngine3/include/matter/world_session.h` (FrameStats: add `probe_bricks`; WorldSession: add `debug_probe_brick`)
- Modify: `MatterEngine3/tests/world_stream_tests.cpp` (brick appear/freed assertions — the spec's "brick bake smoke")

**Interfaces:**
- Consumes: `probe_bricks::BrickStore/WindowParams/composite` (Task 3), `WorldTracer::set_scratch_dir` (Task 4), `probe_bake::bake_probes(tracer, lights, BakeParams)` with `has_bounds=true`, `terrain_field::FieldRuntime::height_at(x,z)`, `viewer::upload_probe_textures/release_probe_textures`, `RasterComposer::set_probes`.
- Produces: stderr log lines `[probe] brick (%lld,%lld r%d) %dx%dx%d in %.2fs` (success — Task 6's smoke gate greps `"[probe] brick ("`) and `[probe] brick freed (%lld,%lld)`; `FrameStats::probe_bricks`; `bool WorldSession::debug_probe_brick(int64_t tx, int64_t tz) const` (test seam).

Threading rules (existing): `sector_map` guarded by `sector_map_mutex`; `focus` guarded by `focus_mutex`; GL work only on GL thread via `gpu_jobs`; worker thread owns streaming. The new probe thread only reads `sector_map`/`focus` under their mutexes, never touches GL, and communicates results through `BrickStore` + a mutex-guarded pending window volume.

- [ ] **Step 1: Add Impl members**

In `WorldSession::Impl`, after the `refine_pending_upgrades_` block (~line 413):

```cpp
    // --- Phase C: per-sector probe bricks (streamed-world baked lighting) ---
    // Probe thread bakes bricks with probe_bake over a WorldTracer of nearby
    // resident sectors, stores them in probe_store, and composites a camera-
    // centred window volume. The GL thread uploads the window when ready.
    probe_bricks::BrickStore probe_store;
    std::thread probe_thread;
    std::mutex probe_mu;                    // guards probe_stop/probe_pending/probe_window
    std::condition_variable probe_cv;
    bool probe_stop = false;
    struct BrickReq { int64_t tx, tz; int rung; };
    std::vector<BrickReq> probe_pending;
    probe_volume::ProbeVolume probe_window;         // pending composited window
    std::atomic<bool> probe_window_ready{false};
    float probe_center[3] = {0, 0, 0};              // probe-thread-only: last composite centre
    uint64_t probe_epoch = 0;                        // probe-thread-only: tracer instance fingerprint
    std::unique_ptr<world_tracer::WorldTracer> probe_tracer;   // probe-thread-only

    void probe_thread_start();
    void probe_thread_join();
    void probe_request_brick(int64_t tx, int64_t tz, int rung);
    void probe_note_evicted_locked(int64_t tx, int64_t tz);   // caller holds sector_map_mutex
    void probe_thread_loop();
    void probe_composite_now(float cx, float cy, float cz);
```

Includes at the top of matter_engine.cpp: add `#include "probe_bricks.h"`, `#include "probe_bake.h"`, `#include "world_tracer.h"`, `#include <condition_variable>` (check which already exist).

- [ ] **Step 2: Implement the probe thread**

Place next to `execute_sector_stream_step` (after line 2245):

```cpp
void WorldSession::Impl::probe_thread_start() {
    {
        std::lock_guard<std::mutex> lk(probe_mu);
        probe_stop = false;
    }
    if (!probe_thread.joinable())
        probe_thread = std::thread([this] { probe_thread_loop(); });
}

void WorldSession::Impl::probe_thread_join() {
    {
        std::lock_guard<std::mutex> lk(probe_mu);
        probe_stop = true;
    }
    probe_cv.notify_all();
    if (probe_thread.joinable()) probe_thread.join();
    probe_tracer.reset();
    probe_store.clear();
    {
        std::lock_guard<std::mutex> lk(probe_mu);
        probe_pending.clear();
    }
    probe_window_ready = false;
}

void WorldSession::Impl::probe_request_brick(int64_t tx, int64_t tz, int rung) {
    if (rung < 1) return;                       // far ring: flat ambient
    if (probe_store.has(tx, tz)) return;        // baked once per residency
    {
        std::lock_guard<std::mutex> lk(probe_mu);
        for (const auto& r : probe_pending)
            if (r.tx == tx && r.tz == tz) return;
        probe_pending.push_back(BrickReq{tx, tz, rung});
    }
    probe_cv.notify_one();
}

void WorldSession::Impl::probe_note_evicted_locked(int64_t tx, int64_t tz) {
    // Only drop the brick when NO rung of this sector remains resident
    // (rung upgrades evict the old rung while the new one stays).
    for (int r = 0; r <= 3; ++r)
        if (sector_map.count(SectorKey{tx, tz, r})) return;
    if (probe_store.erase(tx, tz))
        fprintf(stderr, "[probe] brick freed (%lld,%lld)\n",
                (long long)tx, (long long)tz);
    std::lock_guard<std::mutex> lk(probe_mu);
    for (size_t i = 0; i < probe_pending.size(); ++i)
        if (probe_pending[i].tx == tx && probe_pending[i].tz == tz) {
            probe_pending.erase(probe_pending.begin() + i);
            break;
        }
}

void WorldSession::Impl::probe_composite_now(float cx, float cy, float cz) {
    probe_bricks::WindowParams wp;
    probe_volume::ProbeVolume vol;
    size_t covered = probe_store.composite(cx, cy, cz, manifest.lights,
                                           world_sector_size, wp, vol);
    if (covered == 0) return;
    {
        std::lock_guard<std::mutex> lk(probe_mu);
        probe_window = std::move(vol);
    }
    probe_center[0] = cx; probe_center[1] = cy; probe_center[2] = cz;
    probe_window_ready = true;
}

void WorldSession::Impl::probe_thread_loop() {
    const world_lights::WorldLights lights = manifest.lights;
    const float S = world_sector_size;
    for (;;) {
        BrickReq req{0, 0, -1};
        bool have_req = false;
        float fx, fy, fz;
        {
            std::unique_lock<std::mutex> lk(probe_mu);
            // 500ms tick doubles as the composite/upload throttle.
            probe_cv.wait_for(lk, std::chrono::milliseconds(500),
                              [&] { return probe_stop || !probe_pending.empty(); });
            if (probe_stop) return;
            {
                std::lock_guard<std::mutex> fl(focus_mutex);
                fx = focus[0]; fy = focus[1]; fz = focus[2];
            }
            if (!probe_pending.empty()) {
                size_t best = 0; float bd = 1e30f;   // nearest-to-camera first
                for (size_t i = 0; i < probe_pending.size(); ++i) {
                    float dx = ((float)probe_pending[i].tx + 0.5f) * S - fx;
                    float dz = ((float)probe_pending[i].tz + 0.5f) * S - fz;
                    float d = dx * dx + dz * dz;
                    if (d < bd) { bd = d; best = i; }
                }
                req = probe_pending[best];
                probe_pending.erase(probe_pending.begin() + best);
                have_req = true;
            }
        }

        if (!have_req) {
            // Idle tick: re-composite when the camera drifted > 2 cells.
            probe_bricks::WindowParams wp;
            float dx = fx - probe_center[0], dy = fy - probe_center[1],
                  dz = fz - probe_center[2];
            if (probe_store.count() > 0 &&
                dx * dx + dy * dy + dz * dz > 4.0f * wp.cell * wp.cell)
                probe_composite_now(fx, fy, fz);
            continue;
        }
        if (probe_store.has(req.tx, req.tz)) continue;

        // ---- gather tracer instances: resident sectors within 12 sectors ----
        const int64_t R = 12;
        std::vector<world_tracer::TraceInstance> inst;
        uint64_t fp = 1469598103934665603ull;
        {
            std::lock_guard<std::mutex> lk(sector_map_mutex);
            for (const auto& kv : sector_map) {
                if (std::llabs(kv.first.tx - req.tx) > R ||
                    std::llabs(kv.first.tz - req.tz) > R) continue;
                world_tracer::TraceInstance ti;
                ti.part_hash = kv.second.part_hash;
                std::memset(ti.transform, 0, sizeof(ti.transform));
                ti.transform[0] = ti.transform[5] = ti.transform[10] = ti.transform[15] = 1.0f;
                ti.transform[3]  = (float)kv.first.tx * S;
                ti.transform[11] = (float)kv.first.tz * S;
                inst.push_back(ti);
                fp = (fp ^ kv.second.part_hash) * 1099511628211ull;
                fp = (fp ^ (uint64_t)(kv.first.tx * 73856093ll ^ kv.first.tz * 19349663ll))
                     * 1099511628211ull;
            }
        }
        if (inst.empty() || !world_field) continue;

        if (!probe_tracer || fp != probe_epoch) {
            auto tr = std::make_unique<world_tracer::WorldTracer>();
            if (provider) tr->set_scratch_dir(provider->transient_dir());
            std::string terr;
            if (!tr->build(engine->cache_root, inst, terr)) {
                fprintf(stderr, "[probe] tracer build failed (%lld,%lld): %s\n",
                        (long long)req.tx, (long long)req.tz, terr.c_str());
                continue;
            }
            probe_tracer = std::move(tr);
            probe_epoch = fp;
        }

        // ---- vertical bounds: field heights over a 5x5 sample of the sector ----
        float y_lo = 1e30f, y_hi = -1e30f;
        for (int i = 0; i <= 4; ++i)
            for (int j = 0; j <= 4; ++j) {
                float h = world_field->height_at(((float)req.tx + i * 0.25f) * S,
                                                 ((float)req.tz + j * 0.25f) * S);
                y_lo = std::min(y_lo, h);
                y_hi = std::max(y_hi, h);
            }

        probe_bake::BakeParams bp;
        bp.cell = (req.rung >= 2) ? 4.0f : 8.0f;   // near ring dense, mid ring half
        bp.has_bounds = true;
        bp.bounds_min[0] = (float)req.tx * S;  bp.bounds_max[0] = (float)(req.tx + 1) * S;
        bp.bounds_min[2] = (float)req.tz * S;  bp.bounds_max[2] = (float)(req.tz + 1) * S;
        bp.bounds_min[1] = y_lo - 8.0f;        bp.bounds_max[1] = y_hi + 24.0f;  // canopy pad
        bp.threads = 2;   // sector bakes own the big cores

        auto t0 = std::chrono::steady_clock::now();
        probe_volume::ProbeVolume brick = probe_bake::bake_probes(*probe_tracer, lights, bp);
        float secs = std::chrono::duration<float>(
                         std::chrono::steady_clock::now() - t0).count();
        if (!brick.valid()) {
            fprintf(stderr, "[probe] brick bake FAILED (%lld,%lld r%d)\n",
                    (long long)req.tx, (long long)req.tz, req.rung);
            continue;
        }
        // Evicted mid-bake? Discard — otherwise the brick would linger forever
        // (its eviction notification already ran).
        {
            std::lock_guard<std::mutex> lk(sector_map_mutex);
            bool resident = false;
            for (int r = 0; r <= 3 && !resident; ++r)
                resident = sector_map.count(SectorKey{req.tx, req.tz, r}) != 0;
            if (!resident) continue;
        }
        fprintf(stderr, "[probe] brick (%lld,%lld r%d) %dx%dx%d in %.2fs\n",
                (long long)req.tx, (long long)req.tz, req.rung,
                brick.grid.nx, brick.grid.ny, brick.grid.nz, secs);
        probe_store.put(req.tx, req.tz, std::move(brick));
        probe_composite_now(fx, fy, fz);
    }
}
```

NOTE for the implementer: verify `probe_bake::bake_probes` honors `has_bounds` verbatim (read `probe_bake.cpp` grid-setup, ~lines 297–340). If it still applies `pad_cells` on top of the given bounds, that's fine (1 cell of extra skirt); if it IGNORES `has_bounds` outside tests, stop and report — do not work around it.

- [ ] **Step 3: Hook stream publish + evictions**

(a) In the `stream.publish` GL job (~line 2218), right after the `sector_map[sk] = SectorEntry{...};` insert inside the mutex scope ends, add:

```cpp
            probe_request_brick(pub_tx, pub_tz, pub_rung);
```

(Adapt `pub_tx/pub_tz/pub_rung` to the tx/tz/rung locals the publish job actually captures. Likewise check the real field names of `SectorKey` and the eviction record before using `SectorKey{tx, tz, r}` / `ev.tx, ev.tz` below.)

(b) In BOTH eviction lambdas (`drain_sector_evictions` ~line 2038 and `execute_sector_stream_step` step 2 ~line 2101), right after `sector_map.erase(it);` (still under `sector_map_mutex`), add:

```cpp
            probe_note_evicted_locked(ev.tx, ev.tz);
```

- [ ] **Step 4: Thread lifecycle**

(a) Start: find where the streamer is created (`grep -n "sector_streamer = std::make_unique" MatterEngine3/src/matter_engine.cpp`). Immediately after that assignment add `probe_thread_start();`.

(b) Stop: find every teardown of the streamer/session (`grep -n "sector_streamer.reset()" MatterEngine3/src/matter_engine.cpp` plus the destructor/disconnect path around line 2570 where `release_probe_textures(impl_->probe_tex)` runs). At EACH site call `probe_thread_join();` BEFORE `sector_streamer.reset()` / `world_field.reset()` / provider teardown (the probe thread dereferences `world_field`, `provider`, `sector_map`). If a site runs on the GL/app thread while the worker owns those members, keep the call ordering consistent with how `sector_streamer.reset()` itself is sequenced there — same thread, immediately before.

- [ ] **Step 5: GL-side window upload in render()**

In `WorldSession::render`, GpuDriven branch, immediately before `auto resolved = resolver.resolve(...)` (~line 2768), add:

```cpp
        // Streamed-world probe window: upload the latest composited volume.
        // Closed worlds keep their static compose_world probes untouched.
        if (impl_->sector_streamer && impl_->probe_window_ready.exchange(false)) {
            probe_volume::ProbeVolume vol;
            {
                std::lock_guard<std::mutex> lk(impl_->probe_mu);
                vol = std::move(impl_->probe_window);
            }
            if (vol.valid()) {
                viewer::release_probe_textures(impl_->probe_tex);
                impl_->probe_tex = viewer::upload_probe_textures(vol);
                impl_->raster->set_probes(impl_->probe_tex);
                impl_->stats.probe_dims[0] = vol.grid.nx;
                impl_->stats.probe_dims[1] = vol.grid.ny;
                impl_->stats.probe_dims[2] = vol.grid.nz;
            }
        }
```

- [ ] **Step 6: FrameStats field + debug seam**

(a) In `include/matter/world_session.h`, find `struct FrameStats` (it already has `probe_dims`/`resident_sectors`) and add:

```cpp
    uint32_t probe_bricks = 0;   // resident probe bricks (streamed world)
```

(b) In the `WorldSession` class declaration (next to `frame_stats()`), add:

```cpp
    // Test seam: does the streamed-world probe store hold a brick for this
    // sector column? Always false for closed-world sessions.
    bool debug_probe_brick(int64_t tx, int64_t tz) const;
```

(c) In matter_engine.cpp, `render()` refreshes `stats.resident_sectors` at the top (~line 2709) and `frame_stats()` refreshes it again (~line 2832). At BOTH sites, directly below the resident_sectors assignment, add the brick count so the stat works regardless of render path:

```cpp
    impl_->stats.probe_bricks = impl_->sector_streamer
        ? (uint32_t)impl_->probe_store.count() : 0;
```

(d) Next to `frame_stats()`'s definition, implement the seam:

```cpp
bool WorldSession::debug_probe_brick(int64_t tx, int64_t tz) const {
    return impl_->probe_store.has(tx, tz);
}
```

- [ ] **Step 7: Build both binaries**

```bash
make -C MatterEngine3 -j8 && make -C ExplorerDemo -j8 && make -C ExplorerDemo windows
```

Expected: all exit 0.

- [ ] **Step 8: Brick bake smoke — extend world_stream_tests.cpp**

`world_stream_tests` (target `run-worldstream`, Makefile ~lines 1186–1193) is the end-to-end streamed-session suite: open → stream → move focus +200 → regenerate → destroy, with rings `MATTER_STREAM_RINGS="32:3,80:2"`. Add the spec's brick assertions:

(a) After the `rs1` assertion (`resident_sectors > 0 after first stream cycle`), insert:

```cpp
    // Probe bricks appear for resident sectors (bounded poll; bakes are async).
    auto poll_brick = [&](int64_t tx, int64_t tz, bool want, double timeout_s) {
        double t0 = GetTime();
        while (GetTime() - t0 < timeout_s) {
            session->pump_gpu_jobs(16.0f);
            session->tick();
            if (session->debug_probe_brick(tx, tz) == want) return true;
        }
        return false;
    };
    bool brick0 = poll_brick(0, 0, true, 90.0);
    printf("probe brick (0,0) appeared: %d\n", (int)brick0);
    assert(brick0 && "probe brick baked for resident sector (0,0)");
    assert(session->frame_stats().probe_bricks > 0 && "probe_bricks stat > 0");
```

(b) After the `rs2` assertion in step 4 (focus moved to (200, 0, 200) — sector (0,0) is now far outside the 80m outer ring, so its column fully evicts), insert:

```cpp
    // Bricks are freed when the sector column fully evicts.
    bool brick0_gone = poll_brick(0, 0, false, 60.0);
    printf("probe brick (0,0) freed after eviction: %d\n", (int)brick0_gone);
    assert(brick0_gone && "probe brick freed on full eviction");
```

- [ ] **Step 9: Run the streamed-session suite**

```bash
cd MatterEngine3/tests && GALLIUM_DRIVER=d3d12 make run-worldstream
```

Expected: `ALL TESTS PASSED` — covers bricks appearing, bricks freed on eviction, AND session teardown (catches probe-thread join deadlocks/races on destroy).

- [ ] **Step 10: Commit**

```bash
git add MatterEngine3/src/matter_engine.cpp MatterEngine3/include/matter/world_session.h \
        MatterEngine3/tests/world_stream_tests.cpp
git commit -m "feat(engine): probe brick bake thread + window upload for streamed worlds"
```

---

### Task 6: Flight smoke gate + full verification

**Files:**
- Modify: `ExplorerDemo/tools/flight_smoke.sh` (new gate)

**Interfaces:**
- Consumes: `[probe] brick` stderr lines (Task 5).

- [ ] **Step 1: Add the probe gate to flight_smoke.sh**

Read the script's existing gate section (PASS/FAIL echo pattern) and add, in the same style, after the resident_sectors gate:

```bash
probe_bricks=$(grep -c "\[probe\] brick (" "$LOG" || true)
if [ "${probe_bricks:-0}" -gt 0 ]; then
  echo "PASS: probe bricks baked ($probe_bricks)"
else
  echo "FAIL: no probe bricks baked"; FAILED=1
fi
```

(Adapt variable names — `$LOG` / `FAILED` — to whatever the script actually uses.)

- [ ] **Step 2: Run the flight smoke**

```bash
cd ExplorerDemo && GALLIUM_DRIVER=d3d12 tools/flight_smoke.sh
```

Expected: `FLIGHT SMOKE: ALL PASS` including the new probe gate; zero bake errors; fps avg not materially below the pre-change baseline (58.0 — a few fps of probe-composite overhead is acceptable, single-digit avg drop is a FAIL: investigate before committing).

- [ ] **Step 3: Chain the remaining suites (sequentially, never parallel)**

```bash
cd MatterEngine3/tests && make run-probebrick && make run-sectorbake && \
  GALLIUM_DRIVER=d3d12 make run-gpucull
```

Expected: ALL PASS each.

- [ ] **Step 4: Rebuild Windows binary (if any code changed since Task 5's build)**

```bash
make -C ExplorerDemo windows
```

- [ ] **Step 5: Commit**

```bash
git add ExplorerDemo/tools/flight_smoke.sh
git commit -m "test(smoke): gate on probe bricks baked during flight"
```

---

## Verification Summary

| Gate | Command | Covers |
|------|---------|--------|
| Sector bake suite | `make run-sectorbake` | Tasks 1–2 schema edits |
| Probe brick unit | `make run-probebrick` | Task 3 sampler/compositor |
| World stream suite | `GALLIUM_DRIVER=d3d12 make run-worldstream` | Task 5 brick smoke (appear/freed) + lifecycle/teardown |
| GPU cull parity | `GALLIUM_DRIVER=d3d12 make run-gpucull` | no regression |
| Flight smoke | `GALLIUM_DRIVER=d3d12 tools/flight_smoke.sh` | end-to-end: bricks bake, fps holds, zero bake errors |
| Windows build | `make -C ExplorerDemo windows` | standing requirement |

Final acceptance is Jack flying the Windows build: denser/scaled scatter visible, lighting no longer flat.
