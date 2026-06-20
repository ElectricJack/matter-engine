# Baked Vertex Ambient Occlusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Precompute per-vertex ambient occlusion on the CPU after meshing (sampling the persistent occupancy field) and read it back in the raytrace shader via barycentric interpolation, retiring the per-pixel AO rays.

**Architecture:** A new pure, GL-free stage `bake_vertex_ao` runs in the dirty-cell rebuild between meshing (Phase 2) and BLAS commit (Phase 3). It writes a per-vertex AO value into each triangle's `TriEx`. The BLAS packer stores the three values byte-packed into the one unused triangle-record slot (`row5.w`). The shader unpacks and interpolates them at shade time, and the AO ray loop is deleted. Works identically for oriented cubes and marching cubes because it keys only on vertex position + normal + occupancy.

**Tech Stack:** C++ (raylib `float3`/`Mesh`), GLSL fragment shader with a custom `#include` preprocessor (`make shaders`), headless C++ unit tests via `tests/Makefile`.

**Spec:** `docs/superpowers/specs/2026-06-20-baked-vertex-ao-design.md`

---

## File Structure

- **Create** `MatterSurfaceLib/include/vertex_ao.h` — `AoParams`, `AoGrid`, `bake_vertex_ao`, `pack_ao_w`.
- **Create** `MatterSurfaceLib/src/vertex_ao.cpp` — implementation.
- **Create** `MatterSurfaceLib/tests/vertex_ao_tests.cpp` — headless unit tests.
- **Modify** `MatterSurfaceLib/include/bvh.h:27` — add AO fields to `TriEx`.
- **Modify** `MatterSurfaceLib/tests/Makefile` — new `run-ao` target.
- **Modify** `MatterSurfaceLib/src/blas_manager.cpp` (~470-485) — pack AO into `row5.w`.
- **Modify** `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` — `Triangle`/`HitResult` AO field, shade-block unpack + barycentric interpolation.
- **Modify** `MatterSurfaceLib/shaders/raytrace_tlas_blas.fs` — `calculatePBR` takes baked AO; delete `calculateAmbientOcclusion`.
- **Modify** `MatterSurfaceLib/include/cluster.h` + `src/cluster.cpp` — `set_ao_baker(...)` + bake call at the Phase-3 hook.
- **Modify** `MatterSurfaceLib/main.cpp` (~after 674) — wire occupancy + grid into the cluster.

Reference facts (verified):
- `TriEx` at `include/bvh.h:27`: `struct TriEx { float2 uv0,uv1,uv2; float3 N0,N1,N2; int materialId; float4 tint; };`
- `Tri` at `include/bvh.h:15` has `vertex0/vertex1/vertex2` (`float3`).
- BLAS packer loop at `src/blas_manager.cpp:420-489`; `row5.w` is set to `0.0f` at line 475 and never overwritten (the free slot).
- Triangle texture is 6 rows/triangle, `PIXELFORMAT_UNCOMPRESSED_R32G32B32A32`.
- Shader `Triangle` struct at `shaders/bvh_tlas_common.glsl:35`; `HitResult` at `:67`; `decodeTriangle` at `:180`; shade block at `:485-527`.
- AO is consumed in `calculatePBR` at `shaders/raytrace_tlas_blas.fs:350` and multiplied into ambient/indirect terms (`:354+`).
- Hook point: `src/cluster.cpp:323-327`, the Phase-3 drain loop, before `commit_cell_meshes`. `results[i].groups[j]` is a `GroupMeshResult` with `.triangles` (`vector<Tri>`) and `.triangle_normals` (`vector<TriEx>`).
- Occupancy: `Occupancy::occupied(SlotCoord)` at `include/occupancy.h:23`; `scene_occ_` built in `main.cpp:644-674`.
- Lattice: `GridLattice::slot_position(c) = {c.x*spacing_, ...}` (`src/lattice.cpp:7`); `spacing()` at `include/lattice.h:27`; no inverse exists.
- Shader build: `make shaders` runs `$(PREPROCESSOR)` to inline `bvh_tlas_common.glsl` into `raytrace_tlas_blas_processed.fs` (`Makefile:154`). The `_processed.fs` is gitignored; edit the sources and rebuild.
- Tests build with `make WSL_LINUX=1` style flags via `tests/Makefile`; existing targets `run-cull`, etc.

---

## Task 1: Add AO fields to TriEx

**Files:**
- Modify: `MatterSurfaceLib/include/bvh.h:27`

- [ ] **Step 1: Check for positional aggregate initialization of TriEx**

Run: `grep -rn "TriEx{" MatterSurfaceLib/src MatterSurfaceLib/main.cpp`
Expected: only `TriEx ex{}` / `TriEx{}` empty-brace forms (which stay valid). If any positional `TriEx{a, b, ...}` exists, it must be updated in Step 2 to account for the new trailing fields. (Current known users do `TriEx ex{}; ex.N0 = ...;` — safe.)

- [ ] **Step 2: Add default-initialized AO fields**

In `include/bvh.h`, change line 27 from:

```cpp
struct TriEx { float2 uv0, uv1, uv2; float3 N0, N1, N2; int materialId; float4 tint; };
```

to:

```cpp
struct TriEx {
    float2 uv0, uv1, uv2; float3 N0, N1, N2; int materialId; float4 tint;
    // Per-vertex baked ambient occlusion in [0,1]; 1.0 = fully unoccluded.
    // Defaulted so unbaked meshes (e.g. marching cubes before any bake) render bright.
    float ao0 = 1.0f, ao1 = 1.0f, ao2 = 1.0f;
};
```

Default member initializers keep `TriEx` an aggregate (C++14+), so existing `TriEx ex{}` value-init still works and yields `ao* = 1.0f`.

- [ ] **Step 3: Build a translation unit that uses TriEx to confirm it compiles**

Run: `cd MatterSurfaceLib && make WSL_LINUX=1 build/linux/obj/blas_manager.o 2>&1 | tail -5`
Expected: compiles with no errors (object file produced). If the object path differs, run `make WSL_LINUX=1` and confirm `blas_manager.cpp` compiles.

- [ ] **Step 4: Commit**

```bash
cd MatterSurfaceLib && git add include/bvh.h
git commit -m "feat: add per-vertex AO fields to TriEx (default unoccluded)"
```

---

## Task 2: vertex_ao module + unit tests (TDD)

**Files:**
- Create: `MatterSurfaceLib/include/vertex_ao.h`
- Create: `MatterSurfaceLib/src/vertex_ao.cpp`
- Create: `MatterSurfaceLib/tests/vertex_ao_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile`

- [ ] **Step 1: Write the header**

Create `include/vertex_ao.h`:

```cpp
#pragma once

#include "bvh.h"        // Tri, TriEx, float3, make_float3
#include "occupancy.h"  // Occupancy, SlotCoord
#include <vector>

// Tunables for the AO bake.
struct AoParams {
    float radius   = 1.5f; // occlusion reach in cluster-local units
    float strength = 1.0f; // 0 = no darkening, 1 = full strength
};

// Maps a cluster-local position to the occupancy slot grid and back.
// slot_of(p) = round((p - origin) / spacing);  pos_of(c) = origin + c*spacing.
struct AoGrid {
    float spacing = 1.0f;
    float3 origin = make_float3(0.0f, 0.0f, 0.0f);
};

// For each triangle i, compute a per-vertex AO value in [0,1] from nearby
// occupied slots and write it into triEx[i].ao0/ao1/ao2. Pure; no GL calls.
// tris and triEx are parallel arrays (same length / same order).
void bake_vertex_ao(const std::vector<Tri>& tris,
                    std::vector<TriEx>& triEx,
                    const Occupancy& occ,
                    const AoGrid& grid,
                    const AoParams& params);

// Pack three [0,1] AO values into one float's raw bits (8 bits each), matching
// the shader's floatBitsToUint unpack. Exposed for the BLAS packer and tests.
float pack_ao_w(float ao0, float ao1, float ao2);
```

- [ ] **Step 2: Write the failing test**

Create `tests/vertex_ao_tests.cpp`:

```cpp
// Headless unit tests for bake_vertex_ao (pure, no GL context needed).
#include <cstdio>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "vertex_ao.h"
#include "occupancy.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
                              else { printf("ok: %s\n", msg); } } while (0)

// A single triangle whose 3 vertices all sit at world position `p` with normal `n`
// (degenerate geometrically, but bake_vertex_ao treats vertices independently, so
// each gets the same AO — convenient for asserting a single vertex's value).
static void make_point_tri(std::vector<Tri>& tris, std::vector<TriEx>& ex,
                           float3 p, float3 n) {
    Tri t; std::memset(&t, 0, sizeof(Tri));
    t.vertex0 = p; t.vertex1 = p; t.vertex2 = p;
    tris.push_back(t);
    TriEx e{}; e.N0 = n; e.N1 = n; e.N2 = n;
    ex.push_back(e);
}

static Occupancy solid_block(int N) {
    Occupancy occ;
    for (int z = 0; z < N; ++z)
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
                occ.set(SlotCoord{x, y, z}, SlotData{8});
    return occ;
}

static void test_exposed_vertex_is_bright() {
    // A vertex in empty space with no occupied neighbors -> AO == 1.0.
    Occupancy occ; // empty
    std::vector<Tri> tris; std::vector<TriEx> ex;
    make_point_tri(tris, ex, make_float3(0.5f, 0.5f, 0.5f), make_float3(0,1,0));
    bake_vertex_ao(tris, ex, occ, AoGrid{1.0f, make_float3(0,0,0)}, AoParams{2.0f, 1.0f});
    CHECK(ex[0].ao0 > 0.99f, "exposed vertex AO ~ 1.0");
}

static void test_buried_vertex_is_dark() {
    // A vertex at the center of a solid block, normal up -> many occluders in the
    // upper hemisphere -> AO clearly below 1.0.
    Occupancy occ = solid_block(7);
    std::vector<Tri> tris; std::vector<TriEx> ex;
    make_point_tri(tris, ex, make_float3(3.0f, 3.0f, 3.0f), make_float3(0,1,0));
    bake_vertex_ao(tris, ex, occ, AoGrid{1.0f, make_float3(0,0,0)}, AoParams{2.0f, 1.0f});
    CHECK(ex[0].ao0 < 0.5f, "buried vertex AO < 0.5");
}

static void test_monotonic_with_added_occluder() {
    // Adding an occluder slot directly above (in the hemisphere) only lowers AO.
    Occupancy a; // empty
    Occupancy b; b.set(SlotCoord{0, 1, 0}, SlotData{8}); // one occluder above origin
    std::vector<Tri> ta, ea_t; std::vector<TriEx> ea, eb;
    std::vector<Tri> tb; 
    make_point_tri(ta, ea, make_float3(0,0,0), make_float3(0,1,0));
    make_point_tri(tb, eb, make_float3(0,0,0), make_float3(0,1,0));
    AoGrid g{1.0f, make_float3(0,0,0)}; AoParams p{2.0f, 1.0f};
    bake_vertex_ao(ta, ea, a, g, p);
    bake_vertex_ao(tb, eb, b, g, p);
    CHECK(eb[0].ao0 < ea[0].ao0, "adding an occluder lowers AO");
}

static void test_behind_surface_does_not_occlude() {
    // An occluder below a surface whose normal points up must NOT occlude.
    Occupancy occ; occ.set(SlotCoord{0, -1, 0}, SlotData{8}); // directly below
    std::vector<Tri> tris; std::vector<TriEx> ex;
    make_point_tri(tris, ex, make_float3(0,0,0), make_float3(0,1,0));
    bake_vertex_ao(tris, ex, occ, AoGrid{1.0f, make_float3(0,0,0)}, AoParams{2.0f, 1.0f});
    CHECK(ex[0].ao0 > 0.99f, "occluder behind surface does not darken");
}

static void test_pack_roundtrip() {
    float vals[3] = {0.0f, 0.5f, 1.0f};
    float packed = pack_ao_w(vals[0], vals[1], vals[2]);
    uint32_t bits; std::memcpy(&bits, &packed, sizeof(bits));
    float u0 = float(bits & 0xFFu) / 255.0f;
    float u1 = float((bits >> 8) & 0xFFu) / 255.0f;
    float u2 = float((bits >> 16) & 0xFFu) / 255.0f;
    CHECK(std::fabs(u0 - vals[0]) <= 1.0f/255.0f, "pack roundtrip ch0");
    CHECK(std::fabs(u1 - vals[1]) <= 1.0f/255.0f, "pack roundtrip ch1");
    CHECK(std::fabs(u2 - vals[2]) <= 1.0f/255.0f, "pack roundtrip ch2");
}

int main() {
    test_exposed_vertex_is_bright();
    test_buried_vertex_is_dark();
    test_monotonic_with_added_occluder();
    test_behind_surface_does_not_occlude();
    test_pack_roundtrip();
    if (g_failures) { printf("\n%d FAILURE(S)\n", g_failures); return 1; }
    printf("\n=== All vertex_ao tests PASSED ===\n");
    return 0;
}
```

- [ ] **Step 3: Add the Makefile target**

In `tests/Makefile`, after the `run-cull` block (around line 171), add:

```makefile
# Baked per-vertex AO unit tests (headless, no GL window)
AO_TARGET = vertex_ao_tests
AO_SOURCES = vertex_ao_tests.cpp ../src/vertex_ao.cpp ../src/occupancy.cpp

$(AO_TARGET): $(AO_SOURCES)
	$(CC) $(AO_SOURCES) -o $(AO_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)

run-ao: $(AO_TARGET)
	./$(AO_TARGET)
```

- [ ] **Step 4: Run the test to verify it fails to build (no implementation yet)**

Run: `cd MatterSurfaceLib/tests && make run-ao 2>&1 | tail -15`
Expected: link error — `undefined reference to bake_vertex_ao` / `pack_ao_w` (header exists, `src/vertex_ao.cpp` is empty/missing).

- [ ] **Step 5: Write the implementation**

Create `src/vertex_ao.cpp`:

```cpp
#include "vertex_ao.h"

#include <cmath>
#include <cstdint>
#include <cstring>

static inline SlotCoord slot_of(const AoGrid& g, float3 p) {
    return SlotCoord{
        (int)lroundf((p.x - g.origin.x) / g.spacing),
        (int)lroundf((p.y - g.origin.y) / g.spacing),
        (int)lroundf((p.z - g.origin.z) / g.spacing)};
}

static float vertex_ao(float3 p, float3 n, const Occupancy& occ,
                       const AoGrid& g, const AoParams& params) {
    const float R = params.radius;
    if (R <= 0.0f || g.spacing <= 0.0f) return 1.0f;
    const int reach = (int)std::ceil(R / g.spacing);
    const SlotCoord c = slot_of(g, p);
    float accum = 0.0f;
    for (int dz = -reach; dz <= reach; ++dz)
    for (int dy = -reach; dy <= reach; ++dy)
    for (int dx = -reach; dx <= reach; ++dx) {
        const SlotCoord s{c.x + dx, c.y + dy, c.z + dz};
        if (!occ.occupied(s)) continue;
        const float3 sp = make_float3(g.origin.x + s.x * g.spacing,
                                      g.origin.y + s.y * g.spacing,
                                      g.origin.z + s.z * g.spacing);
        const float3 d = make_float3(sp.x - p.x, sp.y - p.y, sp.z - p.z);
        const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dist <= 1e-5f || dist > R) continue;
        const float inv = 1.0f / dist;
        const float align = (d.x * n.x + d.y * n.y + d.z * n.z) * inv; // dot(normalize(d), n)
        if (align <= 0.0f) continue;          // occluder is behind the surface
        const float falloff = 1.0f - dist / R; // linear falloff over the radius
        accum += align * falloff;
    }
    float ao = 1.0f - params.strength * accum;
    if (ao < 0.0f) ao = 0.0f;
    if (ao > 1.0f) ao = 1.0f;
    return ao;
}

void bake_vertex_ao(const std::vector<Tri>& tris, std::vector<TriEx>& triEx,
                    const Occupancy& occ, const AoGrid& grid, const AoParams& params) {
    const size_t n = tris.size() < triEx.size() ? tris.size() : triEx.size();
    for (size_t i = 0; i < n; ++i) {
        const Tri& t = tris[i];
        TriEx& ex = triEx[i];
        ex.ao0 = vertex_ao(t.vertex0, ex.N0, occ, grid, params);
        ex.ao1 = vertex_ao(t.vertex1, ex.N1, occ, grid, params);
        ex.ao2 = vertex_ao(t.vertex2, ex.N2, occ, grid, params);
    }
}

float pack_ao_w(float ao0, float ao1, float ao2) {
    auto q = [](float v) -> uint32_t {
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return (uint32_t)(v * 255.0f + 0.5f);
    };
    const uint32_t bits = q(ao0) | (q(ao1) << 8) | (q(ao2) << 16);
    float f; std::memcpy(&f, &bits, sizeof(f));
    return f;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-ao 2>&1 | tail -15`
Expected: all `ok:` lines, ending `=== All vertex_ao tests PASSED ===`.

- [ ] **Step 7: Commit**

```bash
cd MatterSurfaceLib && git add include/vertex_ao.h src/vertex_ao.cpp tests/vertex_ao_tests.cpp tests/Makefile
git commit -m "feat: add bake_vertex_ao occupancy-sampled per-vertex AO with unit tests"
```

---

## Task 3: Pack baked AO into the BLAS triangle texture

**Files:**
- Modify: `MatterSurfaceLib/src/blas_manager.cpp` (~457-485)

- [ ] **Step 1: Include the packer header**

Near the top of `src/blas_manager.cpp`, with the other includes, add:

```cpp
#include "vertex_ao.h"  // pack_ao_w
```

- [ ] **Step 2: Write AO into row5.w**

In the triangle-packing loop, the block at lines 478-485 currently packs tint into rows 3 and 4 and leaves `row5.w` at 0. Replace that block:

```cpp
                    // Pack tint.b/.a into the spare .w of normal rows 3 and 4
                    // (row 5 .w stays unused); the shader reads these back as tint.
                    {
                        int rowB = texel_off(static_cast<int>(triangle_index), 3);
                        int rowA = texel_off(static_cast<int>(triangle_index), 4);
                        texture_data[rowB + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(original_idx), 2); // tint.b
                        texture_data[rowA + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(original_idx), 3); // tint.a
                    }
```

with:

```cpp
                    // Pack tint.b/.a into the spare .w of normal rows 3 and 4,
                    // and the three baked per-vertex AO values into row 5 .w
                    // (8 bits each; see pack_ao_w / shader floatBitsToUint unpack).
                    {
                        int rowB = texel_off(static_cast<int>(triangle_index), 3);
                        int rowA = texel_off(static_cast<int>(triangle_index), 4);
                        int rowAO = texel_off(static_cast<int>(triangle_index), 5);
                        texture_data[rowB + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(original_idx), 2); // tint.b
                        texture_data[rowA + 3] = pack_tint_w(entry->mesh->triEx, static_cast<int>(original_idx), 3); // tint.a
                        if (has_normals) {
                            const TriEx& exr = entry->mesh->triEx[original_idx];
                            texture_data[rowAO + 3] = pack_ao_w(exr.ao0, exr.ao1, exr.ao2);
                        } else {
                            texture_data[rowAO + 3] = pack_ao_w(1.0f, 1.0f, 1.0f); // no triEx -> unoccluded
                        }
                    }
```

(`has_normals` is already in scope from line 423. `exr` avoids shadowing the `ex` used in the smooth-normal block above if present.)

- [ ] **Step 3: Build the app to confirm it compiles**

Run: `cd MatterSurfaceLib && make WSL_LINUX=1 2>&1 | tail -8`
Expected: links to the Linux binary with no errors.

- [ ] **Step 4: Run the existing BLAS tests to confirm no regression**

Run: `cd MatterSurfaceLib/tests && make run-blas 2>&1 | tail -8` (use the actual BLAS test target name; check `tests/Makefile` — e.g. `blas_refcount_tests`)
Expected: existing BLAS tests still PASS (packing change is additive; refcount/dedup unaffected).

- [ ] **Step 5: Commit**

```bash
cd MatterSurfaceLib && git add src/blas_manager.cpp
git commit -m "feat: pack baked per-vertex AO into BLAS triangle row5.w"
```

---

## Task 4: Shader — unpack AO, interpolate, retire AO rays

**Files:**
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` (struct `Triangle` :35, `HitResult` :67, `decodeTriangle` :180, shade block :491-521)
- Modify: `MatterSurfaceLib/shaders/raytrace_tlas_blas.fs` (`calculateAmbientOcclusion` :260-291, `calculatePBR` :294/:350, AO call site)

- [ ] **Step 1: Add an `ao` field to the `Triangle` and `HitResult` structs**

In `bvh_tlas_common.glsl`, change the `Triangle` struct (line 35-39) to add `ao`:

```glsl
struct Triangle
{
    vec3 v0, v1, v2; vec3 n0, n1, n2; // triangle vertices + per-vertex shading normals
    vec3 center;     // for BVH construction (renamed from centroid - reserved keyword)
    vec3 ao;         // per-vertex baked AO (x=v0, y=v1, z=v2); default 1.0
};
```

In the same file, change `HitResult` (line 67-78) to add `ao`:

```glsl
struct HitResult
{
    bool hit;
    float t;
    vec3 position;
    vec3 normal;
    int material;
    int instanceId;
    int triangleTests; / Debug: number of triangle tests performed
    vec3 tint;       // per-triangle tint rgb (from spare .w of rows 1-3)
    float tintAlpha; // blend strength (from spare .w of row 4); 0 = no tint
    float ao;        // baked ambient occlusion at the hit, [0,1]; 1.0 = unoccluded
};
```

- [ ] **Step 2: Default `tri.ao` in decodeTriangle (no extra fetch during traversal)**

In `decodeTriangle` (line 192), after `tri.n0 = tri.n1 = tri.n2 = vec3(0.0);` add:

```glsl
    tri.ao = vec3(1.0); // baked AO sampled lazily at shade time, not during traversal
```

- [ ] **Step 3: Unpack + interpolate AO in the shade block**

In `bvh_tlas_common.glsl`, replace the shade block from line 491 through the close of the normal branch (line 521). Current code:

```glsl
        // Get triangle data for normal calculation
        Triangle    tri  = decodeTriangle(int(triIdx));
        BVHInstance inst = decodeInstance(int(instIdx));
```
... (material/tint fetches) ...
```glsl
        vec3 normal;
        if (matProps.flatShading) {
            // Use face normal for flat shading
            normal = normalize(cross(tri.v1 - tri.v0, tri.v2 - tri.v0));
        } else {
            // Transform world hit position to local space
            vec3 localHitPos = transformPosition(result.position, inst.invTransform);
            // Smooth normal: lazily sample the per-vertex normals (rows 3-5) and interpolate by barycentrics at the hit
            { vec3 N0 = ...; ... normal = normalize(bu * N0 + bv * N1 + bw * N2); } }
        }
```

Replace the `vec3 normal; if (matProps.flatShading) { ... } else { ... }` section (lines 512-521) with a version that computes barycentrics once and uses them for both AO and smooth normals:

```glsl
        // Barycentrics at the hit (shared by baked AO and smooth-normal shading).
        vec3 localHitPos = transformPosition(result.position, inst.invTransform);
        vec3 e1 = tri.v1 - tri.v0, e2 = tri.v2 - tri.v0, ep = localHitPos - tri.v0;
        float d00 = dot(e1, e1), d01 = dot(e1, e2), d11 = dot(e2, e2);
        float d20 = dot(ep, e1), d21 = dot(ep, e2);
        float den = d00 * d11 - d01 * d01;
        float bv = 0.0, bw = 0.0, bu = 1.0;
        if (abs(den) >= 1e-12) {
            bv = (d11 * d20 - d01 * d21) / den;
            bw = (d00 * d21 - d01 * d20) / den;
            bu = 1.0 - bv - bw;
        }

        // Baked AO: unpack 3x8-bit from row 5 .w and interpolate. Bits==0 means the
        // slot was never written (old texture / disabled) -> treat as unoccluded.
        float aoPacked = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 5, 6)).w;
        uint aoBits = floatBitsToUint(aoPacked);
        vec3 aoV = (aoBits == 0u) ? vec3(1.0)
                 : vec3(float(aoBits & 0xFFu), float((aoBits >> 8) & 0xFFu), float((aoBits >> 16) & 0xFFu)) / 255.0;
        result.ao = (aoEnabled == 1) ? clamp(bu * aoV.x + bv * aoV.y + bw * aoV.z, 0.0, 1.0) : 1.0;

        vec3 normal;
        if (matProps.flatShading) {
            normal = normalize(cross(e1, e2)); // face normal
        } else {
            vec3 N0 = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 3, 6)).xyz;
            vec3 N1 = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 4, 6)).xyz;
            vec3 N2 = texture(trianglesTexture, tiledTexel(trianglesTexture, int(triIdx), 5, 6)).xyz;
            if (abs(den) < 1e-12) normal = normalize(cross(e1, e2));
            else normal = normalize(bu * N0 + bv * N1 + bw * N2);
        }
```

Also set `result.ao = 1.0;` in the miss branch (the `else` at line 528-536), adding it alongside the other defaults:

```glsl
        result.tint = vec3(0.0);
        result.tintAlpha = 0.0;
        result.ao = 1.0;
```

- [ ] **Step 4: Thread baked AO through calculatePBR and delete the ray-based AO**

In `raytrace_tlas_blas.fs`, change the `calculatePBR` signature (line 294) to accept the baked AO:

```glsl
vec3 calculatePBR(vec3 hitPos, vec3 normal, vec3 viewDir, vec3 albedo, float roughness, float metallic, float bakedAO, inout uint seed) {
```

Replace the AO computation at line 349-350:

```glsl
    // Calculate ambient occlusion
    float ao = calculateAmbientOcclusion(hitPos, normal, seed);
```

with:

```glsl
    // Baked ambient occlusion (precomputed per-vertex, interpolated at the hit).
    float ao = bakedAO;
```

- [ ] **Step 5: Update the calculatePBR call site(s)**

Run: `grep -n "calculatePBR(" MatterSurfaceLib/shaders/raytrace_tlas_blas.fs`
For each call (other than the definition), insert the hit's baked AO as the new second-to-last argument before `seed`. The call passes shading inputs derived from a `HitResult` (commonly named `hit`); pass that hit's `.ao`. Example transform:

```glsl
// before:
vec3 lit = calculatePBR(hit.position, hit.normal, rayDir, albedo, roughness, metallic, seed);
// after:
vec3 lit = calculatePBR(hit.position, hit.normal, rayDir, albedo, roughness, metallic, hit.ao, seed);
```

If a call site lacks a `HitResult` in scope, pass `1.0` (unoccluded) — but the primary shading call has the hit available; use `.ao` there.

- [ ] **Step 6: Delete the now-unused calculateAmbientOcclusion function**

Remove the entire `calculateAmbientOcclusion` function (lines 260-291). Confirm nothing else references it:

Run: `grep -n "calculateAmbientOcclusion" MatterSurfaceLib/shaders/*.fs MatterSurfaceLib/shaders/*.glsl`
Expected: no matches after deletion.

- [ ] **Step 7: Rebuild shaders and the app**

Run: `cd MatterSurfaceLib && make shaders 2>&1 | tail -5 && make WSL_LINUX=1 2>&1 | tail -5`
Expected: `raytrace_tlas_blas_processed.fs` regenerates with no preprocessor error; app links. (GLSL compile errors surface at runtime when the shader loads, so also do Step 8.)

- [ ] **Step 8: Runtime shader-compile smoke check**

This requires a GL context, which the headless build can't provide. Defer the actual visual/runtime verification to Task 6 (manual). For now confirm the processed shader contains the new code:

Run: `grep -c "bakedAO\|result.ao\|aoBits" MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs`
Expected: a non-zero count (the edits were inlined by the preprocessor).

- [ ] **Step 9: Commit**

```bash
cd MatterSurfaceLib && git add shaders/bvh_tlas_common.glsl shaders/raytrace_tlas_blas.fs
git commit -m "feat: read baked per-vertex AO in shader, delete per-pixel AO rays"
```

---

## Task 5: Wire the bake into the rebuild flow

**Files:**
- Modify: `MatterSurfaceLib/include/cluster.h` (add members + setter declaration)
- Modify: `MatterSurfaceLib/src/cluster.cpp` (include header, bake at Phase-3 hook)
- Modify: `MatterSurfaceLib/main.cpp` (~after 674, call the setter)

- [ ] **Step 1: Declare the AO baker on Cluster**

In `include/cluster.h`, add the include near the top:

```cpp
#include "vertex_ao.h"  // AoGrid, AoParams
```

In the `Cluster` class public section, declare:

```cpp
    // Enables post-meshing per-vertex AO baking against `occ` (borrowed, must
    // outlive the cluster). Pass occ=nullptr to disable. `grid` maps cluster-local
    // positions to occupancy slots; see AoGrid.
    void set_ao_baker(const Occupancy* occ, AoGrid grid, AoParams params);
```

In the private section, add members:

```cpp
    const Occupancy* ao_occ_ = nullptr;
    AoGrid    ao_grid_{};
    AoParams  ao_params_{};
```

If `cluster.h` does not already include `occupancy.h`, `vertex_ao.h` pulls it in transitively (it includes `occupancy.h`).

- [ ] **Step 2: Define the setter and call the bake at the hook**

In `src/cluster.cpp`, ensure `#include "../include/vertex_ao.h"` is present near the other includes (it provides `bake_vertex_ao`; `AoGrid`/`AoParams` come via `cluster.h`).

Add the setter definition (near the other small Cluster setters, e.g. after `set_mesh_worker_count`):

```cpp
void Cluster::set_ao_baker(const Occupancy* occ, AoGrid grid, AoParams params) {
    ao_occ_ = occ;
    ao_grid_ = grid;
    ao_params_ = params;
}
```

In `rebuild_dirty_cells`, modify the Phase-3 drain loop (lines 323-327) to bake before committing:

```cpp
    // PHASE 3 - DRAIN (serial, fixed job order): bake AO, then commit GL/BLAS.
    uint32_t committed_groups = 0;
    for (size_t i = 0; i < jobs.size(); ++i) {
        if (ao_occ_) {
            for (auto& g : results[i].groups) {
                bake_vertex_ao(g.triangles, g.triangle_normals, *ao_occ_, ao_grid_, ao_params_);
            }
        }
        jobs[i].cell->commit_cell_meshes(results[i], blas_manager_);
        committed_groups += static_cast<uint32_t>(results[i].groups.size());
    }
```

(Confirm the field name for the per-cell mesh groups is `results[i].groups` and that each element exposes `.triangles` / `.triangle_normals`; this matches `GroupMeshResult` in `include/mesh_worker_pool.h:25`.)

- [ ] **Step 3: Wire occupancy + grid from main.cpp**

In `main.cpp`, after `scene_occ_` is populated (the loop ending ~line 674) and after `test_cluster_` exists, call the setter. Add `#include "include/vertex_ao.h"` near the other includes (top of file ~line 35). Then, in scene setup right after the occupancy fill:

```cpp
    // Bake per-vertex AO against the occupancy field. Grid origin 0 / lattice
    // spacing: emitted particle positions are slot_position(c) = c*spacing in
    // cluster-local space, so round(pos/spacing) recovers the slot.
    test_cluster_->set_ao_baker(&scene_occ_,
                                AoGrid{ lattice.spacing(), make_float3(0.0f, 0.0f, 0.0f) },
                                AoParams{ /*radius*/ 1.5f, /*strength*/ 1.0f });
```

Use whatever local name the lattice has at that scope for `spacing()` (the same object passed to `cull_interior`/`emit_all` at lines 728-729).

- [ ] **Step 4: Verify coordinate alignment with a one-shot debug assertion**

Add a temporary sanity check immediately after the setter call to confirm the grid maps a known occupied slot back to itself (catches any recentering offset mismatch). This is the one place the design's coordinate assumption is validated:

```cpp
#ifndef NDEBUG
    {
        // Pick any occupied slot and confirm round(slot_position/spacing) recovers it.
        bool checked = false;
        scene_occ_.for_each([&](SlotCoord c, const SlotData&) {
            if (checked) return;
            Vector3 wp = lattice.slot_position(c);
            SlotCoord back{ (int)lroundf(wp.x / lattice.spacing()),
                            (int)lroundf(wp.y / lattice.spacing()),
                            (int)lroundf(wp.z / lattice.spacing()) };
            if (back.x != c.x || back.y != c.y || back.z != c.z)
                printf("[AO] WARN slot (%d,%d,%d) -> pos (%.3f,%.3f,%.3f) -> (%d,%d,%d) MISMATCH\n",
                       c.x, c.y, c.z, wp.x, wp.y, wp.z, back.x, back.y, back.z);
            else
                printf("[AO] grid alignment ok for slot (%d,%d,%d)\n", c.x, c.y, c.z);
            checked = true;
        });
    }
#endif
```

If this logs MISMATCH at runtime, the `AoGrid.origin` must be set to the offset between `slot_position` output and the particle emission frame (inspect `cell_origin_offset` usage in `regenerate_surface_`). Resolve before proceeding.

- [ ] **Step 5: Build**

Run: `cd MatterSurfaceLib && make WSL_LINUX=1 2>&1 | tail -8`
Expected: links with no errors.

- [ ] **Step 6: Commit**

```bash
cd MatterSurfaceLib && git add include/cluster.h src/cluster.cpp main.cpp
git commit -m "feat: bake per-vertex AO in the rebuild drain phase before BLAS commit"
```

---

## Task 6: End-to-end visual + performance verification (manual)

**Files:** none (verification only)

This task cannot be unit-tested (needs a GL context and human judgment). It is performed by the user.

- [ ] **Step 1: Build the platform binary the user runs**

Run (WSL → Windows native, the build the user launches):
`cd MatterSurfaceLib && rm -f build/windows-native/obj/main.o && TARGET=windows-native make 2>&1 | tail -8`
Expected: `matter_surface_lib.exe` rebuilt (main.o recompiled — the Makefile has no header dependency tracking, so force it).

- [ ] **Step 2: Grid-alignment check**

Ask the user to launch and confirm the console shows `[AO] grid alignment ok ...` (not MISMATCH). If MISMATCH, return to Task 5 Step 4.

- [ ] **Step 3: Visual check**

Ask the user to compare AO on/off via the existing `aoEnabled` toggle (ImGui "Ambient Occlusion" checkbox):
- AO on: crevices/contact points between cubes darken smoothly; exposed faces stay bright; no black triangles, no harsh seams.
- AO off: flat, no contact darkening.
Confirm baked AO looks at least as good as the previous ray-traced AO (smoother, less noisy expected).

- [ ] **Step 4: Performance check**

Ask the user to run with `MSL_GPU_PROFILE=1` and confirm the frame time dropped vs. the pre-change ~354ms baseline (the AO rays are gone). Record the new `GPU Raytrace Pass` number.

- [ ] **Step 5: Tune knobs if needed**

If AO is too dark/light or reaches too far, adjust the `AoParams{ radius, strength }` literals in `main.cpp` (Task 5 Step 3) and rebuild. Commit any tuning change:

```bash
cd MatterSurfaceLib && git add main.cpp
git commit -m "tune: AO radius/strength defaults from visual check"
```

- [ ] **Step 6: Remove the temporary alignment assertion (optional)**

Once alignment is confirmed stable, the `#ifndef NDEBUG` block from Task 5 Step 4 can stay (it's debug-only and cheap) or be removed. If removed:

```bash
cd MatterSurfaceLib && git add main.cpp
git commit -m "chore: drop one-shot AO grid-alignment debug check"
```

---

## Self-Review Notes

- **Spec coverage:** isolated post-mesh stage (Task 2/5), occupancy-hemisphere math (Task 2), AO in free slot with zero growth (Task 3), shader read path + retire rays (Task 4), dirty-rebuild ride-along with seam-staleness accepted (Task 5 — re-bakes only dirty cells' own vertices, no dilation), tests (Task 2), manual visual/perf (Task 6). All spec sections map to tasks.
- **Default-1.0 safety:** `TriEx.ao*` default to 1.0; the packer always writes `row5.w`; the shader treats `aoBits==0` as unoccluded — three independent guards against the "black mesh" failure mode.
- **Type consistency:** `bake_vertex_ao(const vector<Tri>&, vector<TriEx>&, const Occupancy&, const AoGrid&, const AoParams&)`, `pack_ao_w(float,float,float)`, `Cluster::set_ao_baker(const Occupancy*, AoGrid, AoParams)`, `calculatePBR(..., float bakedAO, inout uint seed)`, `HitResult.ao`, `Triangle.ao` — names consistent across tasks.
- **Coordinate risk** is isolated to `AoGrid` construction (Task 5 Step 3) and explicitly verified (Task 5 Step 4 / Task 6 Step 2).
