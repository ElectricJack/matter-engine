# Raster Phases 2+3: Probe Volume Lighting + Clusters — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Phase 2 (baked probe-volume lighting: world light list, GL-free CPU
SH-L1 probe bake, 3D-texture probe sampling in the forward shader, tracer light
alignment) and Phase 3 (spatial cluster split, flat artifact v3, per-cluster ε ladders
with locked boundaries, per-cluster frustum cull + LOD) of the approved spec
`docs/superpowers/specs/2026-07-02-raster-switch-design.md`, on branch `feat/raster-mvp`.

**Architecture:** A world light list parsed from `light` lines in world.manifest is the
single source of truth for sun/sky/spotlights. A GL-free multithreaded CPU baker traces
the placed world (custom int32 instance layer over the parts' prebuilt BLAS BVHs — MSL's
TLAS packs instance ids into 12 bits, too small) and produces a probe grid: per cell,
ambient RGB + sun visibility (texture A) and dominant-light direction + intensity
(texture B), serialized to `cache/<world>.probes` keyed by an FNV-1a fingerprint over
(instances, grid, lights). The forward shader samples the two RGBA8 3D textures per
pixel. For clusters, the flattened mesh is median-split into ≤16k-tri clusters; each
cluster gets its own error-bounded ladder whose cut-boundary vertices are frozen by a
new topological boundary lock in mesh_simplifier (user-approved MSL extension), stored
in a v3 flat artifact and culled/LOD-selected per cluster each frame.

**Tech Stack:** C++17, raylib/rlgl + OpenGL 3.3 core, glad (via raylib's
`external/glad.h`) for the 3D-texture shim, std::thread for the bake,
MatterSurfaceLib QEM simplifier + BVH.

## Global Constraints

Copied from the spec (binding for every task):

- Rasterization is the default renderer; ray tracer available via `MATTER_RT=1` for A/B.
- Cluster target size: 16k tris (tunable); small meshes → exactly 1 cluster.
- Cluster invariants: every input triangle in exactly one cluster; boundary vertices
  bit-identical across a cluster's levels; deterministic re-split.
- `.flat.part` v3 stores the cluster table; a v2 flat fails the version guard and
  regenerates — same `<hash>.flat.part` path. Compositional `.part` v2 untouched.
- Light list is the single source of truth consumed by probe baker, raster shader
  uniforms, and the reference tracer; folded into the probe-cache fingerprint.
- Mesh lights via the existing material `emission` channel (materials table slot [5]).
- Probe storage: texture A = ambient c0.rgb + sun_vis in .a; texture B = dominant
  incoming-light direction (xyz remapped to [0,1]) + directional intensity in .a; RGBA8.
- Probe bake: multithreaded, fixed-seed deterministic; ~64 rays/cell; serialized
  `cache/<world>.probes` keyed by FNV-1a over (placed instance set, grid spec, light
  list); atomic write. Grid: world AABB padded by one cell; default cell 1.0 unit.
- Missing/failed `.probes` → flat-ambient fallback uniform, warning printed; never black.
- Probe sampling outside grid → clamp-to-edge. Empty light list → defaults reproduce
  today's Phase-1 look exactly.
- Instance cap 200k, truncate + warn (parity with TLAS path).
- MSL is read-only EXCEPT the user-approved topological-boundary-lock extension to
  `mesh_simplifier.cpp` (Task 8) — no other MSL changes.
- After any engine/viewer change: rebuild Windows too (`make windows`); processed
  tracer shader must be regenerated via the Makefile target if `.glsl`/tracer `.fs`
  sources change; never hand-edit `raytrace_tlas_blas_processed.fs`.
- Dependency order: Phase 2 tasks 1→2→3→4→5→6→7 first (lighting is the overnight
  priority), then Phase 3 tasks 8→14.

## Conventions used by every task

- Build engine: `cd MatterEngine3 && make`. Build viewer: `cd MatterEngine3/viewer &&
  make viewer` (NOT bare `make` — default target is not the binary). Windows:
  `make windows` from `MatterEngine3/`.
- Tests live in `MatterEngine3/tests/`; run a suite with `cd MatterEngine3/tests &&
  make run-<name>`. Follow the existing stanza pattern in `tests/Makefile` (g++ with
  `-I../include -I../../MatterSurfaceLib/include`, gcc for `.c`, link, `rm` objects).
- All fixture `.part` files are written into a fresh `/tmp/...` sandbox per test run
  (pattern: `tests/part_flatten_tests.cpp` and `tests/meadow_bake_tests.cpp` — `rm -rf`
  + `mkdir -p sandbox/parts` + `chdir`).
- `fnv1a64(const void*, size_t)` comes from `part_asset.h` (MSL include path).
- Commit after every task with a conventional `feat:`/`fix:` message.

---

## Phase 2 — Probe Volume Lighting

### Task 1: World light list

**Files:**
- Create: `MatterEngine3/include/world_lights.h`, `MatterEngine3/src/world_lights.cpp`
- Create: `MatterEngine3/tests/lighting_tests.cpp`
- Modify: `MatterEngine3/src/part_graph.cpp` (read_manifest, ~line 207-239: skip lines
  whose first token is `light`)
- Modify: `MatterEngine3/tests/Makefile` (new LIGHTING stanza + run-lighting target)
- Modify: `MatterEngine3/Makefile` (add `world_lights.o` to engine objects)

**Interfaces (produces):**

```cpp
// world_lights.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace world_lights {

struct SpotLight {
    float pos[3];
    float dir[3];        // normalized on parse
    float color[3];      // linear RGB intensity
    float range;         // hard distance cutoff (world units)
    float cos_inner;     // cos(inner cone half-angle), full intensity inside
    float cos_outer;     // cos(outer cone half-angle), zero outside
};

struct WorldLights {
    // Defaults reproduce the Phase-1 hardcoded raster look exactly, so worlds
    // without `light` lines render unchanged.
    float sun_dir[3]   = {-0.45f, -0.80f, -0.35f};  // normalized; FROM sun toward scene
    float sun_color[3] = {2.2f, 2.05f, 1.8f};
    float sky_color[3] = {0.38f, 0.43f, 0.52f};
    std::vector<SpotLight> spots;
};

// Parse `light ...` lines from a world.manifest. Every other line is ignored
// (the part-graph reader owns them). A missing file yields defaults and true.
// Malformed light lines fail with err set.
bool parse_lights(const std::string& manifest_path, WorldLights& out, std::string& err);

// FNV-1a over the packed float values (sun, sky, then spots in file order).
uint64_t lights_fingerprint(const WorldLights& l);

} // namespace world_lights
```

Manifest syntax (whitespace-tokenized, `#` comments already stripped by convention):

```
light sun  <dx> <dy> <dz>  <r> <g> <b>
light sky  <r> <g> <b>
light spot <px> <py> <pz>  <dx> <dy> <dz>  <r> <g> <b>  <range> <inner_deg> <outer_deg>
```

`sun`/`spot` directions are normalized on parse; spot cone degrees are half-angles
converted to cosines (`cos_inner = cosf(inner_deg * PI/180)`). Repeated `sun`/`sky`
lines: last one wins. Implementation: read file line by line, trim, skip blank/`#`,
`sscanf` per kind; on wrong field count return false with `err = "light: bad <kind> line: <line>"`.
`lights_fingerprint`: append all floats (sun_dir, sun_color, sky_color, then each
spot's 12 floats) into a `std::vector<float>` and `fnv1a64(data, size*4)`.

**read_manifest change** (part_graph.cpp): in the line loop, after extracting the first
token as `name`, add `if (name == "light") continue;` so light lines never become module
requests. (Currently any extra token that isn't `expand` is an error, so this is required
for manifests with lights to install at all.)

- [ ] **Step 1: Write failing tests** in `tests/lighting_tests.cpp` (new binary,
  CHECK-macro style copied from meadow_bake_tests.cpp:21-24):
  - defaults: `parse_lights` on a manifest with no light lines returns true and the
    exact default values above.
  - parse: manifest with `light sun 0 -1 0 3 3 3`, `light sky 0.2 0.3 0.5`,
    `light spot 0 5 0  0 -1 0  10 8 6  20 15 30` → fields match (dir normalized,
    cosines correct to 1e-5), 1 spot.
  - malformed: `light spot 1 2` → false, err non-empty.
  - fingerprint: differs when any float changes; equal for identical lights.
  - read_manifest skip: write a manifest with a `light sun ...` line plus a module
    line; call `part_graph` read path via installing in a sandbox (reuse the pattern
    from meadow_bake_tests.cpp main()) OR, simpler and sufficient: assert
    `parse_lights` ignores module lines — AND add one integration assertion in Task 5's
    provider test. For this task, test read_manifest directly if exposed; it is —
    `read_manifest` is declared in `part_graph.h`. Call it on the fixture manifest and
    assert it returns ok with only the module in roots.
- [ ] **Step 2: Add LIGHTING stanza to tests/Makefile** (sources for now:
  `lighting_tests.cpp ../src/world_lights.cpp ../src/part_graph.cpp` + whatever
  part_graph already links in other stanzas — copy the dependency list from the
  existing part_graph test stanza verbatim). Run `make run-lighting` → compile error
  (header missing) = failing state confirmed.
- [ ] **Step 3: Implement** world_lights.h/.cpp + the read_manifest skip + Makefile
  object.
- [ ] **Step 4: `make run-lighting`** → ALL PASS. Also `make run-part-graph` (or the
  existing part_graph suite target names — run ALL existing suites' targets touched by
  part_graph.cpp) to confirm no regression.
- [ ] **Step 5: Commit** `feat: world light list parsed from manifest light lines`

### Task 2: ProbeVolume + .probes serialization

**Files:**
- Create: `MatterEngine3/include/probe_volume.h`, `MatterEngine3/src/probe_volume.cpp`
- Modify: `MatterEngine3/tests/lighting_tests.cpp`, `tests/Makefile` (add
  `../src/probe_volume.cpp` to LIGHTING sources), `MatterEngine3/Makefile` (object)

**Interfaces (produces):**

```cpp
// probe_volume.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace probe_volume {

struct ProbeGrid {
    float origin[3] = {0,0,0};   // world position of cell (0,0,0) CENTER
    float cell      = 1.0f;
    int   nx = 0, ny = 0, nz = 0;
};

// Float storage on CPU; quantization to RGBA8 happens at GPU upload (Task 6).
// Layout x-fastest: idx = ((z*ny)+y)*nx + x, 4 floats per cell.
struct ProbeVolume {
    ProbeGrid grid;
    std::vector<float> ambient;   // rgb = ambient irradiance, a = sun visibility [0,1]
    std::vector<float> dominant;  // xyz = dominant incoming-light dir (unit or 0), a = intensity
    size_t cells() const { return (size_t)grid.nx * grid.ny * grid.nz; }
    bool   valid() const { return cells() > 0 && ambient.size() == cells()*4
                                              && dominant.size() == cells()*4; }
};

// 'PRB1' file: {magic u32, fingerprint u64, ProbeGrid, content fnv1a64 over both
// blobs} header + raw float blobs. Atomic tmp+rename (same pattern as save_v2).
// load returns false on magic/size/fingerprint/content-hash mismatch.
bool save_probes(const std::string& path, const ProbeVolume& v, uint64_t fingerprint);
bool load_probes(const std::string& path, ProbeVolume& out, uint64_t expected_fingerprint);

} // namespace probe_volume
```

Header layout (write field-by-field with fwrite, little-endian native):
`u32 magic = 0x31425250 ('PRB1')`, `u64 fingerprint`, `f32 origin[3]`, `f32 cell`,
`i32 nx, ny, nz`, `u64 content_hash` (fnv1a64 over ambient bytes, then folded with
dominant bytes: hash dominant with the ambient hash as seed — or simply hash the
concatenation by calling fnv over a temp buffer; either, but be deterministic), then
the two float blobs. Atomic: write `path + ".tmp"`, fclose, `rename`.

- [ ] **Step 1: Failing tests** in lighting_tests.cpp: build a 2×2×2 volume with
  distinct values per cell, save to sandbox, load with same fingerprint → memcmp-equal
  vectors + grid equal; load with different fingerprint → false; truncate the file by
  10 bytes (reopen "r+b", ftruncate) → load false; load nonexistent path → false.
- [ ] **Step 2: Run** `make run-lighting` → fails to compile (missing header).
- [ ] **Step 3: Implement**, **Step 4: run** → ALL PASS, **Step 5: Commit**
  `feat: probe volume container + PRB1 serialization`

### Task 3: WorldTracer (GL-free CPU world tracing for the baker)

**Files:**
- Create: `MatterEngine3/include/world_tracer.h`, `MatterEngine3/src/world_tracer.cpp`
- Modify: `tests/lighting_tests.cpp`, `tests/Makefile` (LIGHTING sources gain
  `../src/world_tracer.cpp ../src/part_asset_v2.cpp` + the MSL objects the
  FLATTEN stanza links: mesh_simplifier, blas, bvh, tlas, part_asset, vertex_ao,
  occupancy, material_registry.c — copy that list verbatim from the FLATTEN stanza),
  `MatterEngine3/Makefile` (object)

**Interfaces (produces):**

```cpp
// world_tracer.h
#pragma once
// GL-free CPU tracer over the placed world for the probe baker. Loads each unique
// part hash ONCE (flat artifact preferred, compositional fallback expands children
// into extra instances, depth cap 8), keeps the prebuilt BVHs from load_v2 alive in
// owning scratch managers, and intersects through a custom int32 instance BVH.
// (MSL's TLAS packs instance index into 12 bits of instPrim and uses u16 node
// links — too small for meadow scale, hence this instance layer.)
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace world_tracer {

struct TraceInstance {
    uint64_t part_hash;
    float    transform[16];   // row-major world placement
};

struct Hit {
    float t = -1.0f;
    float normal[3] = {0,0,0};   // world-space geometric normal, faces the ray origin
    int   material_id = -1;      // registry index (TriEx materialId % 1000000), -1 if no TriEx
    float emission = 0.0f;       // MaterialRegistryGet(material_id)->emission (0 if id<0)
    float albedo[3] = {0.5f,0.5f,0.5f};
};

class WorldTracer {
public:
    // cache_root contains parts/<hash>.part and optionally parts/<hash>.flat.part.
    bool build(const std::string& cache_root,
               const std::vector<TraceInstance>& instances, std::string& err);
    bool trace(const float origin[3], const float dir[3], float max_t, Hit& hit) const;
    bool occluded(const float origin[3], const float dir[3], float max_t) const;
    void world_bounds(float mn[3], float mx[3]) const;   // valid after build
    size_t instance_count() const;
    // ... private: per-hash loaded parts (owning BLASManager+entries selection),
    // expanded instance array {part index, transform, inverse, world AABB},
    // int32 instance BVH nodes.
};

} // namespace world_tracer
```

Implementation requirements (write exactly this logic):

1. **Loading.** For each unique hash: try `load_v2(cache_root + "/" +
   cache_path_flat(h), h, ...)` into a heap-allocated scratch
   `BLASManager`/`TLASManager`; if that fails, `load_v2` the compositional
   `cache_path_resolved(h)`. Keep the managers alive for the tracer's lifetime
   (`std::unique_ptr` in a `std::unordered_map<uint64_t, LoadedTracePart>`).
   Entry selection per part: if the file's `LodLevels` is non-empty, trace the
   COARSEST level's `blas_indices`; else trace all entries. Compositional children:
   append each `ChildInstance` as a new pending instance with transform =
   `parent_transform × child_transform` (row-major multiply — copy `mul16` from
   `viewer/world_composer.cpp:9`), recursing to depth 8. Missing child file → skip
   with a warning to stderr, not fatal.
2. **Per-instance data.** Precompute: full 4×4 inverse of the placement (general
   inverse — implement a 16-float row-major `invert4x4`; the flatten pass's
   `NormalMat` code in `src/part_flatten.cpp` has the 3×3 cofactor pattern to crib
   from, but you need the full 4×4 here: use the standard adjugate method, return
   identity + warning on |det| < 1e-12), the inverse-transpose upper-3×3 for normals,
   and the world AABB (transform the part's local AABB — computed once per hash from
   its traced entries' triangles — by the placement: transform all 8 corners).
3. **Instance BVH.** Median-split BVH over instance world-AABB centroids, int32
   child indices, leaf size ≤ 4. Node: `{float bmin[3], bmax[3]; int left, right,
   first, count;}` — internal node when `count == 0`. Standard slab-test ray/AABB
   with early-out against current best t.
4. **Ray vs instance.** Transform to local space WITHOUT normalizing the direction
   (this preserves the world t-parameterization): `localO = inv × [O,1]`,
   `localD = inv(3×3 part) × D`. Build an MSL `BVHRay` (`bvh.h`): set `O`, `D`,
   `rD = 1/localD` component-wise, `hit.t = current_best_t`. For each traced entry of
   the part call `entry->bvh->Intersect(ray, 0)` (instance index 0 — we only use
   `instPrim & 0xFFFFF` = triIdx, and must record WHICH entry the best hit came from:
   compare `ray.hit.t` before/after each entry's Intersect call). If `ray.hit.t`
   improved, compute: triangle = `entry->triangles[triIdx]`; geometric normal =
   normalized cross of edges transformed by the instance's inverse-transpose,
   re-normalized, flipped so `dot(n, worldD) < 0`; material from
   `entry->tri_extra[triIdx].materialId % 1000000` when `tri_extra` is non-empty,
   else -1. Fill albedo/emission from `MaterialRegistryGet` (id −1 → gray defaults,
   emission 0).
5. **occluded** = `trace(...)` returning `hit.t > 0 && hit.t < max_t` (v1: no
   early-out shortcut needed).
6. Empty instance list: `build` succeeds, `world_bounds` returns a unit box at
   origin, `trace` always misses.

- [ ] **Step 1: Failing tests** in lighting_tests.cpp. Fixture: in the sandbox,
  register a unit cube (12 tris, use the triangle-list helper pattern from
  `tests/part_flatten_tests.cpp` — copy its cube builder) into a BLASManager with a
  TriEx array whose materialId = a registry id with emission 0 (use id 1), save_v2
  with one LOD level (threshold 1e9, blas index 0), hash 0xAAAA. Tests:
  - ray from (0,0,-5) dir (0,0,1) at instance identity → hit, `t ≈ 4.5` (cube spans
    ±0.5), normal ≈ (0,0,-1), material_id == 1.
  - same ray, instance translated +10x → miss; ray at x=10 → hit.
  - scaled instance (uniform 2×): t reflects the scaled surface (t ≈ 4.0).
  - occluded((0,0,-5)→(0,0,1), max_t 10) true; max_t 3 false.
  - two instances, nearer one wins.
  - world_bounds contains both instances' boxes.
- [ ] **Step 2: Run** → compile failure. **Step 3: Implement.** **Step 4: Run** →
  ALL PASS. **Step 5: Commit** `feat: GL-free world tracer for probe baking`

### Task 4: Probe bake

**Files:**
- Create: `MatterEngine3/include/probe_bake.h`, `MatterEngine3/src/probe_bake.cpp`
- Modify: `tests/lighting_tests.cpp`, `tests/Makefile` (add `../src/probe_bake.cpp`,
  link `-pthread`), `MatterEngine3/Makefile` (object)

**Interfaces (produces):**

```cpp
// probe_bake.h
#pragma once
#include "probe_volume.h"
#include "world_lights.h"
#include "world_tracer.h"

namespace probe_bake {

struct BakeParams {
    float cell = 1.0f;
    int   max_cells_axis = 96;   // cell size grows to fit oversized worlds
    int   pad_cells = 1;
    int   rays_per_cell = 64;    // spherical Fibonacci set (deterministic, no RNG)
    int   sun_rays = 16;         // per-cell jittered cone rays (splitmix64 by cell idx)
    float sun_cone_deg = 2.0f;
    int   threads = 0;           // 0 = std::thread::hardware_concurrency()
    bool  has_bounds = false;    // test override for the grid AABB
    float bounds_min[3] = {0,0,0}, bounds_max[3] = {0,0,0};
};

probe_volume::ProbeVolume bake_probes(const world_tracer::WorldTracer& tracer,
                                      const world_lights::WorldLights& lights,
                                      const BakeParams& p = BakeParams());

} // namespace probe_bake
```

Algorithm (write exactly this; determinism is a tested requirement):

1. **Grid.** AABB = override when `has_bounds`, else `tracer.world_bounds`. Pad by
   `pad_cells * cell` on all sides. If any axis would exceed `max_cells_axis` cells,
   grow `cell` to `extent_max_axis / max_cells_axis` (single uniform cell size).
   `n* = max(1, (int)ceilf(extent / cell))`; `origin = padded_min + 0.5*cell` (cell
   CENTERS).
2. **Directions.** Spherical Fibonacci over the full sphere:
   `for i in 0..N-1: z = 1 - 2*(i+0.5)/N; r = sqrt(1-z*z); phi = i * 2.399963229728653; d = (r*cos(phi), z, r*sin(phi))`.
   Computed once, shared by all cells.
3. **Per cell** at center P (each cell fully independent → thread-sliced by z-plane,
   `threads` workers, no locks, results written to disjoint ranges):
   - `sumL = 0; sumL1 = (0,0,0)`. For each Fibonacci dir d:
     `Hit h; bool hit = tracer.trace(P, d, 1e30f, h);`
     radiance L = miss ? `sky_color` : `h.albedo * h.emission` (mesh lights; no
     bounce in v1). `sumL += L; lum = 0.2126*L.r + 0.7152*L.g + 0.0722*L.b;
     sumL1 += d * lum`.
   - **Spotlights** (analytic + one shadow ray each): `v = spot.pos - P; dist = |v|`;
     skip if `dist > range` or `dist < 1e-4`. `dirToLight = v/dist`;
     `cosang = dot(-dirToLight, spot.dir)`; cone factor
     `k = clamp((cosang - cos_outer) / max(cos_inner - cos_outer, 1e-4), 0, 1)`;
     skip if k == 0 or `tracer.occluded(P, dirToLight, dist - 1e-3)`. Contribution
     `Ls = spot.color * k / (1 + 0.005*dist*dist)` (same falloff constant as the
     tracer's lighting.glsl). `ambient_contrib += Ls` (added AFTER the /N average of
     the ray sum — spotlight is analytic, not Monte-Carlo);
     `sumL1_analytic += dirToLight * lum(Ls) * N` (pre-scaled so the shared /N below
     treats it consistently).
   - **Sun visibility:** 16 rays toward `-sun_dir` jittered inside `sun_cone_deg`:
     RNG = splitmix64 seeded with the linear cell index; build an orthonormal basis
     around `-sun_dir`, sample disk offsets `(u1, u2)` from the RNG mapped to the
     cone. `vis = unoccluded_count / sun_rays` using `tracer.occluded(P, dir, 1e30f)`.
   - Outputs: `ambient.rgb = sumL/N + spot_ambient`, `ambient.a = vis`;
     `L1 = (sumL1 + sumL1_analytic)/N`; `dominant.xyz = |L1|>1e-6 ? normalize(L1) : 0`;
     `dominant.a = |L1|`.
4. **Calibration anchor (tested):** a cell with nothing in the scene has every ray
   miss → ambient.rgb == sky_color exactly (bitwise from `sum/N` — assert within
   1e-5), vis == 1, dominant intensity ≈ 0 (Fibonacci near-symmetry; assert < 0.05).

- [ ] **Step 1: Failing tests** (all with `has_bounds` overrides so grids stay tiny,
  e.g. 4×4×4; reuse the Task 3 cube fixture parts):
  - open world (no instances): ambient == sky_color ±1e-5, sun_vis == 1,
    |dominant intensity| < 0.05, for every cell.
  - occluder plane above (a wide flat box at y=+2 spanning the grid): cells below
    have sun_vis < 0.1 (default sun points down-ish) and cells above the plane ≈ 1.
  - closed box around a cell (6 cube instances forming walls, or one hollow-box
    part): interior ambient luminance < 0.05 × sky luminance.
  - emissive quad: find an emissive registry material by scanning
    `for id in 0..MaterialRegistryCount()-1: MaterialRegistryGet(id)->emission > 0.5`
    (record which); bake with `sky_color = (0,0,0)` and a cube of that material near
    a cell → that cell's ambient luminance > 0, and a far cell (behind an occluder or
    outside line of sight) is darker. If NO registry material has emission > 0.5,
    print "WARN: no emissive material in registry" and mark the check skipped — then
    REPORT this in your completion notes (do not silently pass).
  - spotlight: no geometry, sky black, one spot at (0,5,0) pointing −y, inner 15°
    outer 30°, range 20 → cell directly below has ambient luminance > 0; cell far
    off-axis (e.g. x=+5, same height) has ≈ 0; dominant dir at the lit cell points
    toward the spot (dot(dominant, up) > 0.7).
  - determinism: two bakes of the occluder scene → `memcmp` equal ambient AND
    dominant vectors, with threads=4.
- [ ] **Step 2: Run** → fail. **Step 3: Implement** (std::thread z-slices).
  **Step 4: Run** → ALL PASS. **Step 5: Commit**
  `feat: CPU SH-L1 probe bake (sky, mesh lights, spots, sun visibility)`

### Task 5: Provider integration — parse lights, bake/cache probes

**Files:**
- Modify: `MatterEngine3/viewer/world_source.h` (WorldManifest gains lights + probes)
- Modify: `MatterEngine3/viewer/local_provider.cpp` (+ its header if config grows)
- Modify: `MatterEngine3/tests/Makefile` (VIEWER_LOGIC_CPP gains
  `../src/world_lights.cpp ../src/probe_volume.cpp ../src/world_tracer.cpp
  ../src/probe_bake.cpp`; ensure `-pthread`), `MatterEngine3/viewer/Makefile`
  (same four sources for the viewer link — follow how part_flatten.cpp was added)
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces (produces):**

```cpp
// world_source.h additions
#include "world_lights.h"      // engine include path is already on the viewer -I list
#include "probe_volume.h"
#include <memory>

struct WorldManifest {
    uint64_t world_root_hash = 0;
    std::vector<WorldManifestEntry> instances;
    world_lights::WorldLights lights;                       // defaults if no light lines
    std::shared_ptr<probe_volume::ProbeVolume> probes;      // null => fallback shading
};
```

**local_provider.cpp changes**, in `connect()` after `flatten_placed()` and after the
manifest instance list is final:

1. `world_lights::parse_lights(abs_world_data + "/" + world_name + ".manifest"?)` —
   NOTE: use the SAME manifest path string that `read_manifest` was called with
   (read the existing code; do not guess the extension). Parse failure → print
   warning, keep defaults. Store into `out.lights`.
2. Compute the probe fingerprint: `fnv1a64` folded over, in order: each placed
   instance's `(part_hash, transform[16])` in manifest order, then the bake grid
   constants (cell, max_cells_axis, pad_cells, rays_per_cell, sun_rays as raw bytes
   of a packed struct), then `lights_fingerprint(out.lights)`. Fold = hash the
   concatenation of all bytes appended to one `std::vector<uint8_t>`.
3. Probe cache path: `<cache_root>/cache/<world_name>.probes`; `mkdir -p
   <cache_root>/cache` (use `mkdir(dir, 0755)` ignoring EEXIST — matches how the
   parts dir is handled elsewhere in the provider; check and mirror it).
4. `load_probes(path, vol, fingerprint)` → hit: wrap in shared_ptr, done. Miss:
   build `world_tracer::WorldTracer` from the manifest instances (map
   `WorldManifestEntry{part_hash, transform}` → `TraceInstance`), `bake_probes`
   with default `BakeParams`, `save_probes`, assign. ANY failure (tracer build,
   save) → print `"probe bake failed: <err>"` and leave `out.probes` null —
   NEVER fatal, connect() still succeeds (spec: fallback shading, never black).
   Print bake wall time (`std::chrono`) — e.g. `"probes: 96x24x96 baked in 3.2s"`.

- [ ] **Step 1: Failing test** in viewer_logic_tests.cpp (extend the existing
  LocalProvider fixture that already builds a sandbox world — find the existing
  connect() test and extend it): after a successful `connect`,
  - `m.probes != nullptr && m.probes->valid()`
  - the `.probes` file exists under the sandbox cache root
  - a SECOND provider instance's `connect` also yields valid probes AND does not
    re-bake: capture the file's mtime (or write a sentinel byte comparison — read
    file bytes before and after; must be identical).
  - a manifest with a `light sky 1 0 0` line yields `m.lights.sky_color[0] == 1.0f`
    and a DIFFERENT `.probes` content than the default-light bake (fingerprint
    changed → re-bake).
- [ ] **Step 2: Run** `make run-viewer-logic` → fails (field missing / not baked).
- [ ] **Step 3: Implement.** **Step 4: Run** run-viewer-logic + run-lighting +
  the flatten/composition/example suites (provider touched — full sweep:
  `cd MatterEngine3/tests && make` if there is an aggregate target, else each
  run-* target). ALL PASS. **Step 5: Commit**
  `feat: provider bakes and caches world probe volume keyed by fingerprint`

### Task 6: Probe 3D textures + forward-shader probe lighting

**Files:**
- Create: `MatterEngine3/viewer/probe_texture.h`, `MatterEngine3/viewer/probe_texture.cpp`
- Modify: `MatterEngine3/viewer/shaders/raster.vs`, `raster.fs`
- Modify: `MatterEngine3/viewer/raster_composer.h`, `raster_composer.cpp`
- Modify: `MatterEngine3/viewer/main.cpp`
- Modify: `MatterEngine3/viewer/Makefile` (probe_texture.cpp)

**Interfaces (produces):**

```cpp
// probe_texture.h
#pragma once
// Direct-GL 3D-texture shim: rlgl has no 3D-texture API. Uses raylib's bundled
// glad loader (function pointers are live after InitWindow on both Linux and the
// MinGW Windows build). Requires a current GL context — viewer-side only.
#include "probe_volume.h"
#include <cstdint>

namespace viewer {

// Quantization scale for ambient rgb and dominant intensity into RGBA8.
constexpr float kProbeAmbientScale = 4.0f;

struct ProbeTextures {
    unsigned int tex_ambient  = 0;   // GL texture ids (GL_TEXTURE_3D)
    unsigned int tex_dominant = 0;
    probe_volume::ProbeGrid grid;
    bool valid() const { return tex_ambient != 0 && tex_dominant != 0; }
};

// Uploads both volumes as RGBA8 3D textures, GL_LINEAR min/mag, CLAMP_TO_EDGE.
// A.rgb = clamp(ambient/kProbeAmbientScale) ; A.a = sun_vis
// B.rgb = dir*0.5+0.5                       ; B.a = clamp(intensity/kProbeAmbientScale)
ProbeTextures upload_probe_textures(const probe_volume::ProbeVolume& v);
void release_probe_textures(ProbeTextures& t);

} // namespace viewer
```

Implementation: `#include "external/glad.h"` inside an `extern "C"`-safe TU that does
NOT include raylib.h first if that conflicts — follow how rlgl/raylib are included in
renderer.cpp and test-compile both orders; glad.h is header-only declarations of
loaded pointers. `glGenTextures / glBindTexture(GL_TEXTURE_3D, ..) / glTexParameteri
(MIN/MAG=GL_LINEAR, WRAP_S/T/R=GL_CLAMP_TO_EDGE) / glTexImage3D(GL_TEXTURE_3D, 0,
GL_RGBA8, nx, ny, nz, 0, GL_RGBA, GL_UNSIGNED_BYTE, staging)` where staging is a
`std::vector<uint8_t>` quantized per the header comment
(`(uint8_t)(clamp(x,0,1)*255 + 0.5f)`).

**raster.vs**: add `out vec3 fragWorldPos;` set to `world.xyz`.

**raster.fs** (complete new lighting block — replace the current ambient+sun line):

```glsl
in  vec3 fragWorldPos;                     // add with the other ins
uniform sampler3D probeAmbient;            // unit 4
uniform sampler3D probeDominant;           // unit 5
uniform vec3  probeOrigin;                 // cell(0,0,0) CENTER
uniform float probeCell;
uniform vec3  probeDims;                   // (nx,ny,nz) as floats
uniform int   useProbes;                   // 0 => Phase-1 flat fallback

// inside main(), replacing `vec3 color = albedo * (ambientColor*ao + sunColor*ndl) + albedo*emission;`
vec3 lit;
if (useProbes == 1) {
    vec3 uvw = ((fragWorldPos - probeOrigin) / probeCell + 0.5) / probeDims;
    vec4 A = texture(probeAmbient,  clamp(uvw, 0.0, 1.0));
    vec4 B = texture(probeDominant, clamp(uvw, 0.0, 1.0));
    vec3  pAmb   = A.rgb * 4.0;                    // kProbeAmbientScale
    float sunVis = A.a;
    vec3  dm     = B.xyz * 2.0 - 1.0;
    float dmLen  = max(length(dm), 1e-4);
    vec3  domDir = dm / dmLen;
    float domI   = B.a * 4.0;
    float ambLum = max(dot(pAmb, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    vec3  ambChroma = pAmb / ambLum;
    vec3  direct = ambChroma * domI * 2.0 * max(dot(N, domDir), 0.0);
    lit = (pAmb + direct) * ao + sunColor * ndl * sunVis;
} else {
    lit = ambientColor * ao + sunColor * ndl;      // Phase-1 fallback, never black
}
vec3 color = albedo * lit + albedo * emission;
```

**raster_composer** additions:

```cpp
void set_lights(const world_lights::WorldLights& l);        // stores; draw() uploads
void set_probes(const ProbeTextures& t);                    // stores ids + grid
```

`init()` gains uniform locations for the six new uniforms. `draw()` — replace the
hardcoded `sdx/sdy/sdz`, sun_col, ambient with the stored `WorldLights` values
(normalize sun_dir before upload; defaults keep today's exact values). When probes
are set: bind `glActiveTexture(GL_TEXTURE4)` + `glBindTexture(GL_TEXTURE_3D, ...)`
(and unit 5), set sampler uniforms to 4/5 (`SetShaderValue` with
`SHADER_UNIFORM_INT`), `useProbes = 1`, upload origin/cell/dims; else `useProbes=0`.
Bind textures INSIDE draw() each frame (raylib's default material binding only
touches unit 0, but be explicit every frame anyway).

**main.cpp**: after a successful `connect_sequence` when `!use_rt`: if
`manifest.probes` valid → `auto pt = upload_probe_textures(*manifest.probes);
raster->set_probes(pt);` else print `"probes unavailable - flat ambient fallback"`.
Always `raster->set_lights(manifest.lights)`. Release textures on reload/teardown
(release before recreating; release before CloseWindow, alongside the existing
`raster.reset()` ordering). Set the clear color from the light list: tone-map
sky_color exactly like the shader (`c/(c+1)` then `pow(1/2.2)`) into a raylib Color
replacing the hardcoded `(Color){96,118,143,255}` — with default lights this should
compute to approximately the same value; print it once.

- [ ] **Step 1** (no GL-free test possible for upload; composer-side logic test):
  in viewer_logic_tests, if RasterComposer's batch logic is testable headlessly,
  assert `set_lights` values land in the getters (add a
  `const world_lights::WorldLights& lights() const` accessor). Keep this minimal —
  the real verification is visual in Step 4.
- [ ] **Step 2: Build** `make viewer` (Linux) — compiles clean.
- [ ] **Step 3: Headless visual check** (Tree world, default lights): launch with
  `MATTER_SCREENSHOT` + `MATTER_CAM` (grab the exact env invocation from
  `main.cpp` / prior session notes in git log), capture BEFORE (git stash or use the
  previous commit's binary) and AFTER shots. Expected: overall look close to
  Phase 1, but shaded areas under the oak canopy noticeably darker (sun_vis < 1),
  no black frame. Read the screenshot file with the Read tool to verify visually.
- [ ] **Step 4: Run all test suites** (`cd tests && make run-*` set). PASS.
- [ ] **Step 5: `make windows`** from MatterEngine3/ (memory rule: always rebuild
  the Windows binary; clean-rebuild if headers changed — they did:
  clear the obj dir per the clean-rebuild rule).
- [ ] **Step 6: Commit** `feat: probe-volume lighting in the forward raster path`

### Task 7: Reference-tracer light alignment

**Files:**
- Modify: `MatterEngine3/viewer/shaders/raytrace_tlas_blas.fs` (lines ~21-23: the
  `lightPos/lightColor/ambient` globals; line ~94 `sunDir`)
- Modify: `MatterEngine3/viewer/shaders/lighting.glsl` (sampleSky sky tint)
- Modify: `MatterEngine3/viewer/renderer.cpp` (+ header) — upload the three uniforms
- Modify: `MatterEngine3/viewer/main.cpp` — pass `manifest.lights` to the renderer
- Regenerate: `raytrace_tlas_blas_processed.fs` via `cd viewer && make shaders`
  (NEVER hand-edit the processed file; beware CRLF — if the diff shows the whole
  file changed, check line endings before committing)

**Changes:**

```glsl
// raytrace_tlas_blas.fs — replace the three hardcoded globals with:
uniform vec3 wlSunDir   = vec3(-0.45, -0.80, -0.35);   // GLSL initializers = defaults
uniform vec3 wlSunColor = vec3(2.2, 2.05, 1.8);
uniform vec3 wlSkyColor = vec3(0.38, 0.43, 0.52);
vec3 lightPos;      // assigned in main() below (kept as globals: lighting.glsl reads them)
vec3 lightColor;
vec3 ambient;
// ...at the top of main():
lightPos   = -normalize(wlSunDir) * 12.0;
lightColor = wlSunColor * 1.8;
ambient    = wlSkyColor;
```

(The scale factors 12.0 / 1.8 preserve the tracer's previous absolute magnitudes with
the default light list: `-normalize(-0.45,-0.80,-0.35)*12 ≈ (5.4, 9.6, 4.2)` vs the
old `(3, 8, 2)` — close in direction and distance-falloff regime; `2.2*1.8 ≈ 3.96` vs
old `4.0`. Verify by A/B screenshot that the traced image is not wildly re-lit.)
In `lighting.glsl` `sampleSky`, multiply the returned gradient by
`(wlSkyColor / vec3(0.38, 0.43, 0.52))` so light-list sky edits tint the traced sky
while the default list leaves it bit-identical.

renderer.cpp: in the same place cameraPos etc. are uploaded (inside BeginShaderMode,
with the `if (loc != -1)` guard pattern), add the three `SetShaderValue` calls from a
new `void set_lights(const world_lights::WorldLights&)` stored member.

- [ ] **Step 1:** Make the shader edits + renderer plumbing; `make shaders` then
  `make viewer`.
- [ ] **Step 2:** A/B: `MATTER_RT=1` screenshot before vs after with default lights —
  images should match closely (same sun/sky regime). Then a manifest with
  `light sky 0.8 0.2 0.2` → traced sky visibly red-tinted AND raster path also
  reddened (shared source of truth demonstrated).
- [ ] **Step 3:** Run all suites; `make windows` + `make win-shaders` if that target
  exists (check viewer/Makefile).
- [ ] **Step 4: Commit** `feat: reference tracer consumes the world light list`

**PHASE 2 CHECKPOINT:** at this commit, lighting works end-to-end (Jack's overnight
priority). If anything after this point goes badly, this checkpoint must remain
shippable.

---

## Phase 3 — Clusters

### Task 8: MSL topological boundary lock (user-approved MSL extension)

**Files:**
- Modify: `MatterSurfaceLib/src/mesh_simplifier.cpp` (decimate(), after the existing
  face-plane lock block at ~line 309-320)
- Modify: `MatterSurfaceLib/include/mesh_simplifier.hpp` (doc comment for
  `lock_boundary` — new semantics)
- Modify: `MatterEngine3/tests/part_flatten_tests.cpp` (test lives engine-side; the
  flatten suite already links mesh_simplifier)

**Why this is allowed:** MSL is read-only by convention; Jack explicitly approved this
specific extension (2026-07-02) because per-cluster ladders need arbitrary cut-edge
locking and CellBounds only locks AABB face planes.

**New semantics:** `lock_boundary == true` now ALSO locks topological boundary
vertices — endpoints of edges with incidence != 2 in the welded topology — regardless
of whether CellBounds is supplied. CellBounds face-plane locking is unchanged and
additive. `lock_boundary == false` behavior unchanged.

**Implementation** (insert after the face-plane block, guarded by
`if (opts.lock_boundary)`):

```cpp
    // 1b. Topological boundary lock: vertices on open or non-manifold edges
    // (incidence != 2 in the welded topology) are frozen. A cluster cut from a
    // larger mesh has its shared cut edges as exactly this open boundary, so
    // per-cluster decimation keeps cut vertices bit-identical across LOD levels
    // and across neighboring clusters. (Engine cluster ladders rely on this;
    // approved extension to the read-only convention, 2026-07-02.)
    if (opts.lock_boundary) {
        std::unordered_map<uint64_t, int> edge_count;
        edge_count.reserve(tris.size() * 3);
        auto ekey = [](int a, int b) {
            if (a > b) std::swap(a, b);
            return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b;
        };
        for (const auto& t : tris) {
            if (t.removed) continue;
            edge_count[ekey(t.v[0], t.v[1])]++;
            edge_count[ekey(t.v[1], t.v[2])]++;
            edge_count[ekey(t.v[2], t.v[0])]++;
        }
        for (const auto& kv : edge_count) {
            if (kv.second != 2) {
                verts[(int)(kv.first >> 32)].locked        = true;
                verts[(int)(kv.first & 0xFFFFFFFF)].locked = true;
            }
        }
    }
```

(add `#include <unordered_map>` at the top.)

**Caller audit (required):** grep ALL of MatterSurfaceLib + MatterEngine3 for
`simplify_mesh` callers and list each with its `lock_boundary`/bounds usage in your
report. Expected: `lod_bake.cpp decimate_tris` (false — unchanged),
`lod_bake.cpp decimate_to_error` (true + AABB — open rims were already face-locked;
topological lock is a superset, closed organic meshes have no open edges → no
change), plus any MSL-internal cell-mesh callers (their open boundaries lie on cell
faces, already locked — the topological lock adds the same vertices). If you find a
caller where the new lock plausibly changes output NOT covered by that reasoning,
STOP and report instead of proceeding.

- [ ] **Step 1: Failing test** in part_flatten_tests.cpp: build an OPEN 8×8 grid
  sheet of quads (128 tris, all in one plane is degenerate for QEM — give it a
  gentle height bump `y = 0.05*sin(x)*cos(z)` so interior collapses have real
  targets), record its boundary-vertex positions (vertices on edges with incidence
  1 in the input). `decimate_to_error(..., large epsilon)`... NOTE
  `decimate_to_error` passes an AABB — for THIS test call `simplify_mesh` directly
  with `SimplifyOptions{target_ratio=0, max_error=huge, lock_boundary=true}` and
  `bounds = nullptr`. Assert: every recorded boundary position appears EXACTLY
  (float bit-compare) in the output vertex set, and the output has fewer tris than
  the input (interior did decimate).
- [ ] **Step 2:** Run `make run-flatten` → the new check FAILS (boundary erodes
  today when bounds==nullptr).
- [ ] **Step 3:** Implement. **Step 4:** run-flatten ALL PASS + the FULL suite sweep
  (meadow, example_world, viewer-logic, tree_bake_check — decimation behavior is
  load-bearing for terrain).
- [ ] **Step 5: Commit** `feat(msl): topological boundary-vertex lock in simplify_mesh (approved extension)`

### Task 9: Cluster split

**Files:**
- Create: `MatterEngine3/include/part_cluster.h`, `MatterEngine3/src/part_cluster.cpp`
- Modify: `MatterEngine3/tests/part_flatten_tests.cpp`, `tests/Makefile`
  (FLATTEN_CPP += `../src/part_cluster.cpp`), `MatterEngine3/Makefile` (object)

**Interfaces (produces):**

```cpp
// part_cluster.h
#pragma once
// Spatial cluster split for flattened meshes: recursive longest-axis centroid
// median split until every cluster is <= target_tris. Deterministic.
#include "bvh.h"        // Tri, TriEx (MSL include path)
#include <cstdint>
#include <vector>

namespace part_cluster {

struct Cluster {
    uint32_t first_tri = 0;      // range into the REORDERED tri array
    uint32_t tri_count = 0;
    float aabb_min[3] = {0,0,0};
    float aabb_max[3] = {0,0,0}; // vertex AABB (not centroid AABB) of the range
};

// Reorders tris (and triex in parallel, when non-empty — sizes must match) so
// each cluster is one contiguous range. Returns clusters in emission order.
// count <= target_tris => exactly one cluster covering everything.
std::vector<Cluster> split_clusters(std::vector<Tri>& tris,
                                    std::vector<TriEx>& triex,
                                    uint32_t target_tris = 16000);

} // namespace part_cluster
```

Implementation: build `std::vector<uint32_t> order` = identity; recurse on index
sub-ranges: if `len <= target_tris` emit a cluster; else pick the longest axis of the
range's CENTROID AABB and `std::nth_element` on `(centroid[axis], index)` (index as
tie-break → deterministic), recurse left/right halves. After recursion, apply the
permutation once to `tris`/`triex` and compute each cluster's VERTEX AABB. Guard: if a
range's centroid AABB is degenerate (all centroids identical) emit it as one cluster
even if oversized (can't split; avoids infinite recursion).

- [ ] **Step 1: Failing tests** in part_flatten_tests.cpp:
  - synthetic 40,000-tri grid sheet → every cluster `tri_count <= 16000`, counts sum
    to 40,000, `first_tri` ranges are contiguous and non-overlapping from 0.
  - every output triangle's 3 vertices inside its cluster AABB (± 1e-5).
  - conservation: multiset of triangle centroids before == after reorder (sort two
    vectors of (x,y,z) and compare).
  - triex parallelism: give tri i a TriEx whose `materialId = i`; after split, for
    every j, `triex[j].materialId` identifies a source triangle whose centroid equals
    `tris[j].centroid`.
  - 100-tri input → exactly 1 cluster.
  - determinism: run twice on copies → identical cluster tables AND identical
    reordered arrays (memcmp).
- [ ] **Step 2:** run-flatten → fail. **Step 3:** implement. **Step 4:** ALL PASS.
- [ ] **Step 5: Commit** `feat: deterministic spatial cluster split`

### Task 10: Flat artifact v3 (cluster table)

**Files:**
- Modify: `MatterEngine3/include/part_asset_v2.h`, `MatterEngine3/src/part_asset_v2.cpp`
- Modify: `MatterEngine3/tests/part_flatten_tests.cpp`

**Interfaces (produces):**

```cpp
// part_asset_v2.h additions
constexpr uint32_t kFormatVersionV3 = 3u;

// One cluster of a v3 flat artifact: its vertex AABB and its own LOD ladder
// (same LodLevel type; blas_indices point into the shared BLAS table).
struct FlatCluster {
    float aabb_min[3];
    float aabb_max[3];
    LodLevels lods;
};

// v3 flat save/load: identical body to v2 (materials, BLAS table, internal
// instances, EMPTY children, EMPTY top-level lods) + an appended cluster table.
// load_v2 on a v3 file fails its version guard (callers regenerate), and
// load_flat_v3 on a v2 file fails likewise.
bool save_flat_v3(const std::string& path, const BLASManager& blas,
                  const TLASManager& tlas,
                  const std::vector<FlatCluster>& clusters,
                  uint64_t resolved_hash);
bool load_flat_v3(const std::string& path, uint64_t expected_resolved_hash,
                  BLASManager& blas, TLASManager& tlas,
                  std::vector<FlatCluster>& clusters_out);

// Header sniff without a full load: returns the format_version field (0 on any
// read/magic failure). The provider uses this to spot stale v2 flats.
uint32_t peek_format_version(const std::string& path);
```

Implementation: refactor, don't duplicate. `save_v2`/`load_v2` internals become
`save_core(..., uint32_t version, extra_writer)` / `load_core(...)` — the existing
body writer/reader parameterized by version (header writes `version` and the
`resolved_hash ^ version` guard exactly as today's code does with 2). v3 appends,
after the lods section: `u32 cluster_count`, then per cluster `f32 aabb_min[3],
aabb_max[3]`, `u32 level_count`, per level `f32 threshold, u32 index_count,
u32 indices[]` (same encoding shape the existing lods writer uses — reuse its
helpers). The content-fnv in the header covers the cluster bytes too (it already
hashes the whole body buffer if the implementation builds the body in memory —
follow the existing structure; if it streams, extend the running hash). Keep
`save_v2`/`load_v2` signatures and byte output BIT-IDENTICAL for version 2 (tested).

- [ ] **Step 1: Failing tests** (part_flatten_tests.cpp):
  - round-trip: build a BLASManager with 2 entries, 2 clusters with distinct AABBs
    and 2-level ladders → save_flat_v3 + load_flat_v3 → clusters equal
    (fields + thresholds + indices), BLAS entries equal (tri counts).
  - cross-version guards: `load_v2` on the v3 file → false; `load_flat_v3` on an
    existing v2 file (save_v2 output) → false.
  - `peek_format_version` returns 3 / 2 / 0 (v3 file, v2 file, garbage file).
  - v2 stability: save_v2 of a fixture before and after the refactor produce
    byte-identical files (write the "before" bytes as a golden copy INSIDE the test
    by saving once at the top — i.e., assert save→load→save is byte-stable, which
    catches refactor drift).
- [ ] **Step 2:** run-flatten → fail. **Step 3:** implement (refactor + v3).
- [ ] **Step 4:** run-flatten + ALL suites that read .part files (meadow,
  example_world, viewer-logic, tree_bake_check) → PASS. **Step 5: Commit**
  `feat: flat artifact v3 with per-cluster LOD tables`

### Task 11: Flatten pass emits clustered v3 + provider regen sniff

**Files:**
- Modify: `MatterEngine3/src/part_flatten.cpp` (+ `include/part_flatten.h`:
  FlattenResult gains `size_t clusters = 0;`, FlattenTargets gains
  `uint32_t cluster_target_tris = 16000;`)
- Modify: `MatterEngine3/viewer/local_provider.cpp` (`flatten_placed`: regenerate
  when `peek_format_version(flat_path) != 3`, not just when the file is missing)
- Modify: `MatterEngine3/tests/part_flatten_tests.cpp`, `tests/viewer_logic_tests.cpp`
  (fixtures that pre-create flats must create v3 ones or expect regeneration)

**Changes to flatten_part** (after the Gatherer produces the merged `tris`/`triex`):

1. `auto clusters = part_cluster::split_clusters(tris, triex, targets.cluster_target_tris);`
2. Per cluster (loop): slice `std::vector<Tri> ctris(tris.begin()+first, ...+first+count)`
   and matching `ctriex`. Level 0: `register_triangles(ctris, ctriex)` into the output
   BLASManager (same dedup-aware index lookup as the current code). Ladder levels
   i ≥ 1: `eps_i = cluster_radius / radius_divisor[i-1]` where `cluster_radius` =
   half the cluster AABB diagonal; `decimate_to_error(ctris, eps_i)` — **MODIFY
   `decimate_to_error` in `src/lod_bake.cpp`: add a
   `bool use_aabb_bounds = true` parameter; when false it passes `bounds = nullptr`
   (topological lock only — cluster cut edges are open edges, Task 8) while keeping
   `lock_boundary = true`.** Call it with `false` here. Then `reproject_triex`
   against the cluster's source. Stop conditions per cluster unchanged
   (min_tris — scale it down to `min_tris / clusters.size() + 64` is WRONG; keep a
   per-cluster floor of `max(64u, targets.min_tris / (uint32_t)clusters.size())` so
   many-cluster parts still ladder down; document this in a comment), no-progress
   stop unchanged. Thresholds: same formula as today but with cluster_radius.
3. Build `std::vector<part_asset::FlatCluster>` (AABB from the cluster table, ladder
   from step 2) and `save_flat_v3` to the SAME `cache_path_flat` path.
   `FlattenResult.levels` = max levels over clusters; `.clusters` = cluster count.

**Provider:** in `flatten_placed`, replace the existence check with
`if (part_asset::peek_format_version(flat_abs_path) != 3) { flatten_part(...); }`.

- [ ] **Step 1: Failing tests:**
  - flatten the existing two-box fixture → output file `peek_format_version == 3`,
    `load_flat_v3` yields ≥1 cluster, cluster tri counts sum to `full_tris`.
  - big mesh: synthesize a 40k-tri part (grid sheet as one BLAS entry, save_v2 to
    the sandbox) → flatten → `result.clusters > 1`, every cluster level-0 range
    ≤ 16000 tris.
  - **watertight invariant (the Task 8 payoff):** for the 40k grid flatten, collect
    cross-cluster shared vertex positions (positions appearing in ≥2 clusters'
    level-0 triangle sets, exact float compare). For EVERY cluster and EVERY ladder
    level of that cluster: each shared position belonging to that cluster is present
    bit-identically in that level's vertex set.
  - regen sniff: write a v2 flat at the path (save_v2), run the provider connect
    fixture → file is now v3 (viewer_logic_tests).
- [ ] **Step 2:** run-flatten + run-viewer-logic → fail. **Step 3:** implement.
- [ ] **Step 4:** ALL suites pass. **Step 5: Commit**
  `feat: flatten emits clustered v3 artifacts; provider regenerates stale v2 flats`

### Task 12: PartStore cluster loading (+ RT legacy path)

**Files:**
- Modify: `MatterEngine3/viewer/part_store.h`, `part_store.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces (produces):** `LoadedPart` gains

```cpp
struct LoadedCluster {
    float aabb_min[3], aabb_max[3];
    float radius;                          // half AABB diagonal
    std::vector<float>    thresholds;      // per ladder level
    std::vector<uint32_t> lod_blas;        // per level: BLAS index in the SHARED blas_
    std::vector<int>      lod_mesh;        // per level: raster mesh-data id (existing scheme)
};
// in LoadedPart:
std::vector<LoadedCluster> clusters;       // non-empty iff a v3 flat was loaded
```

**load_flat changes:** try `load_flat_v3` first. Per cluster per level: read the
scratch entries for that level's blas_indices, `register_triangles` into the shared
`blas_` (existing dedup-aware handle→index lookup) and `build_raster_mesh_data`
(existing helper — one mesh-data per (cluster, level)). Fill `LoadedCluster`. ALSO
build the legacy whole-part view so the RT path and existing composer logic keep
working unchanged: legacy level count = max cluster levels; legacy level i =
concatenation over clusters of level `min(i, cluster.levels-1)` blas indices; legacy
threshold i = max over clusters of that same choice. `lp.bound_radius` from the union
of cluster AABBs. If `load_flat_v3` fails, fall back to the existing `load_v2` single
ladder path (v2 flats → 1 synthetic cluster from the stored lods so the raster path
is uniform), then the existing compositional fallback (clusters stays empty; composer
treats it as today).

- [ ] **Step 1: Failing tests** (viewer_logic_tests): fixture with a v3 flat (from
  Task 11's flatten): after `get_or_load`, `lp.clusters.size() >= 1`, every cluster
  has `thresholds.size() == lod_blas.size() == lod_mesh.size() >= 1`, legacy
  `lp.lod_blas`/`lp.thresholds` non-empty and level 0 tri total == sum of cluster
  level-0 tri counts, `bound_radius > 0`. A v2-flat fixture still loads (1 cluster).
- [ ] **Step 2:** run-viewer-logic → fail. **Step 3:** implement. **Step 4:** ALL
  suites pass (incl. RT-path suites — legacy view must keep them green).
- [ ] **Step 5: Commit** `feat: part store loads v3 cluster ladders`

### Task 13: RasterComposer per-cluster cull + LOD + batch reuse

**Files:**
- Modify: `MatterEngine3/viewer/raster_composer.h`, `raster_composer.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Changes:** in `build_batches`, when a LoadedPart has `clusters`:

1. **Frustum:** hand-roll (raymath.h conflicts with precomp.h's float3 — do NOT
   include it; see the existing comment in raster_composer.cpp). Build row-major
   view (look-at from Camera3D position/target/up) and perspective (fovy, aspect =
   width/height from `GetScreenWidth/Height`, near 0.05, far 4000 — match the
   values the existing projection uses if visible in the file; else these) with
   small static helpers; `vp = view × proj` using the row-major `mul16` convention
   from world_composer.cpp:9. Extract 6 planes (Gribb-Hartmann rows method for
   row-major: plane_i = col4 ± col_i of vp transposed — implement and TEST against
   known points, see Step 1). Cluster test: transform the 8 AABB corners by the
   instance transform, then all-outside-any-plane → cull.
2. **Level select per cluster:** projected size = `cluster.radius * inst_scale /
   max(dist_to_camera, 0.01)` — the SAME metric the composer already uses for
   whole-part LOD (find that code and reuse the identical formula so thresholds
   mean the same thing); pick the coarsest level whose threshold ≤ projected size
   (existing convention); clamp to finest available.
3. **Batch key** becomes (part_hash, cluster_index, level): change the existing
   `key = (hash<<4)|level` map key to a struct or `hash*8192 + cluster*8 + level`
   — use a proper `std::map<std::tuple<uint64_t,uint32_t,uint32_t>, Batch>` to
   avoid collisions. Parts WITHOUT clusters keep the existing whole-part path
   (treat as 1 cluster with the part's bound_radius and no AABB cull beyond the
   existing behavior).
4. **Fingerprint reuse:** extend the existing fingerprint to fold (camera position,
   target, fovy) so camera motion invalidates cached batches (LOD/cull now depend
   on the camera; the Phase-1 fingerprint may not include it — check and fix).
5. HUD counters: expose `size_t batches() const, drawn_tris() const,
   culled_clusters() const` accumulated in build_batches.

- [ ] **Step 1: Failing tests** (viewer_logic_tests; build_batches is GL-free):
  synthetic store with one v3-clustered part, camera at origin looking +z:
  - instance far behind the camera → 0 batches (culled).
  - instance ahead near → every visible cluster at level 0; far → coarser levels
    (assert selected level increases with distance).
  - two instances ahead → batch instance counts sum correctly per (cluster, level).
  - same camera+instances twice → second build reports reuse (existing
    fingerprint-hit counter or add one).
  - plane-extraction sanity: a point straight ahead inside near/far → inside all 6
    planes; a point behind → outside.
- [ ] **Step 2:** run-viewer-logic → fail. **Step 3:** implement. **Step 4:** ALL
  suites. **Step 5: Commit** `feat: per-cluster frustum cull + LOD in raster composer`

### Task 14: Integration, HUD, docs, Windows, A/B evidence

**Files:**
- Modify: `MatterEngine3/viewer/main.cpp` (HUD lines), `MatterEngine3/docs/architecture.md`,
  `MatterEngine3/docs/rendering.md`
- No new tests; this is the verification task.

- [ ] **Step 1: HUD:** add to the existing debug text: batch count, drawn tris,
  culled clusters, probe grid dims (or "probes: OFF"), frame ms for the active path.
- [ ] **Step 2: Full test sweep:** every `run-*` target in tests/Makefile passes.
- [ ] **Step 3: Clean-cache run:** wipe the sandbox/demo cache `parts/` + `cache/`
  once, run the viewer headless (Tree world, then meadow world): first run bakes
  flats v3 + probes (watch prints), second run loads both from cache (no bake
  prints). Capture screenshots of both worlds via MATTER_SCREENSHOT; inspect with
  the Read tool: lit scene, no black frame, no missing parts, no cracks between
  clusters (zoom cam positions if needed via MATTER_CAM).
- [ ] **Step 4: A/B:** same MATTER_CAM shot with `MATTER_RT=1` vs raster; silhouettes
  and materials must match; lighting is soft-vs-traced by design.
- [ ] **Step 5: Windows:** from MatterEngine3/: clean the Windows objs (headers
  changed across this branch — clean-rebuild rule), `make windows` (+ win-shaders
  if present). Confirm viewer.exe builds.
- [ ] **Step 6: Docs:** update architecture.md (flatten → clusters → v3 artifact;
  probe pipeline; light list) and rendering.md (raster default, probe sampling,
  per-cluster LOD, fallback matrix: no-probes / no-flat / v2-flat).
- [ ] **Step 7: Commit** `feat: clustered raster integration, HUD, docs, windows build`

---

## Verification (whole branch)

1. `cd MatterEngine3/tests && make` then every `run-*` target — all green.
2. `cd MatterEngine3 && make && make windows`; `cd viewer && make viewer && make shaders`.
3. Headless Tree + meadow screenshots (raster default): lit, stable, no cracks;
   `MATTER_RT=1` A/B matches silhouettes.
4. Probe cache: second run does not re-bake; editing a `light` line re-bakes.
5. Phase-2 checkpoint commit (after Task 7) is independently shippable.

## Out of scope

- GPU probe baking, texture baking, CSM/dynamic lights, cluster DAG (spec non-goals).
- Any MSL change beyond Task 8's approved boundary lock.
- Imposter far-field tier (parked).

## Self-review notes (writing-plans checklist)

- Spec coverage: components 1(T9), 2(T10), 3(T1/T5), 4(T2/T3/T4), 5(T12), 6(T13),
  7(T6), 8(T5/T6/T14); error cases: probes-missing fallback (T6), v2-flat regen
  (T11), degenerate cluster (T9 guard/T12 clamp), outside-grid clamp (T6 shader),
  empty light list = defaults (T1), instance cap already shipped in Phase 1.
- Types cross-checked: LodLevels/FlatCluster (T10) consumed by T11/T12;
  WorldLights (T1) consumed by T4/T5/T6/T7; ProbeVolume (T2) by T4/T5/T6;
  TraceInstance/Hit (T3) by T4/T5; LoadedCluster (T12) by T13.
- Known judgment points delegated with explicit stop-and-report rules: emissive
  registry material existence (T4), simplify_mesh caller audit (T8), manifest path
  string (T5), projection constants (T13).



