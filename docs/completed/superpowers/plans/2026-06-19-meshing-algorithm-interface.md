# Meshing Algorithm Interface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make per-merge-group mesh generation pluggable behind a `MeshingAlgorithm` interface selected by material, ship a marching-cubes implementation that preserves current behavior, and add an oriented-cube implementation for sand-like materials.

**Architecture:** `Cell::build_group_mesh` keeps its algorithm-agnostic setup (resolve the group's particle subset, bounds, blend params, clip set), packs them into a `MeshContext`, looks up the group's algorithm from its representative material, and calls `MeshingAlgorithm::generate(ctx)`. Each algorithm returns a fully-formed `GroupMeshResult` (raylib `Mesh` + `Tri`/`TriEx` arrays, already material/tint-tagged). Downstream commit/BLAS/TLAS code is unchanged.

**Tech Stack:** C++14 (dispatch + algorithms) and C (material registry, surface core), raylib `Mesh`, the existing per-worker `SurfaceScratch` threading model, headless `make`-based unit tests under `MatterSurfaceLib/tests/`.

---

## Reference: existing types (do not redefine)

- `Particle` (`include/particle.h`): `{ Vector3 position; float radius; int materialId; }`
- `Bounds` (`include/surface.h`): `{ Vector3 center; Vector3 size; int divisionPow; }`
- `CellBounds` (`include/mesh_simplifier.hpp`): `{ Vector3 min_bound; Vector3 max_bound; }` (used by `simplify_mesh` with `lock_boundary`)
- `GroupMeshResult` (`include/mesh_worker_pool.h`): `{ uint32_t group_id; Mesh mesh; std::vector<Tri> triangles; std::vector<TriEx> triangle_normals; }`
- `Tri` (`include/bvh.h`): `{ float3 vertex0, vertex1, vertex2, centroid; }`
- `TriEx` (`include/bvh.h`): `{ float2 uv0,uv1,uv2; float3 N0,N1,N2; int materialId; float4 tint; }`
- `float3`/`float4`/`make_float3`/`make_float4`/`normalize`/`cross` are header-inline in `include/precomp.h`.
- `MaterialDef` + `MaterialRegistryGet` (`include/material_registry.h`).
- `convert_mesh_to_triangles` + `unload_cpu_mesh`: currently defined in `src/cell.cpp` (Task 3 moves them to a shared util).

---

## Task 1: Add material-driven algorithm selection field

**Files:**
- Modify: `MatterSurfaceLib/include/material_registry.h`
- Modify: `MatterSurfaceLib/src/material_registry.c`
- Test: `MatterSurfaceLib/tests/material_registry_tests.cpp`

- [ ] **Step 1: Write the failing test**

Append these checks inside `main()` in `MatterSurfaceLib/tests/material_registry_tests.cpp`, just before the final `if (failures == 0)` line:

```cpp
    // Meshing algorithm defaults to 0 (marching cubes) for existing materials.
    CHECK(MaterialMeshingAlgorithm(0) == 0, "material 0 should default to marching cubes (0)");
    CHECK(MaterialMeshingAlgorithm(3) == 0, "material 3 should default to marching cubes (0)");
    // Out-of-range id returns the default material's algorithm (0), never crashes.
    CHECK(MaterialMeshingAlgorithm(99999) == 0, "out-of-range id must return default algorithm 0");
    // Sand (new material id 13) selects the oriented-cube algorithm (1).
    CHECK(MaterialMeshingAlgorithm(13) == 1, "sand(13) should select oriented cubes (1)");
    CHECK(MaterialRegistryCount() >= 14, "expected at least 14 materials after adding sand");
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-reg`
Expected: FAIL to compile (`MaterialMeshingAlgorithm` undeclared), or link error.

- [ ] **Step 3: Add the field and accessor declaration**

In `MatterSurfaceLib/include/material_registry.h`, add a field to the end of the `MaterialDef` struct (after `mergeGroup`):

```c
    int   mergeGroup;     // particles whose materials share a mergeGroup blend together
    int   meshingAlgorithm; // 0 = marching cubes (default), 1 = oriented cubes; selects the mesher
} MaterialDef;
```

And declare the accessor next to `MaterialMergeGroup`:

```c
// Merge group for a material id (the SDF grouping key).
int MaterialMergeGroup(int materialId);

// Meshing algorithm for a material id (MeshAlgorithm enum value; 0 = marching cubes).
int MaterialMeshingAlgorithm(int materialId);
```

Note: `MATERIAL_FLOATS_PER_DEF` and `MaterialRegistryPackForGPU` are unchanged — meshing
algorithm is a CPU-only meshing decision and is not uploaded to the GPU.

- [ ] **Step 4: Add the field value to every material and implement the accessor**

In `MatterSurfaceLib/src/material_registry.c`, add a trailing `, 0` (or `, 1` for sand) to each initializer and add a sand material. Replace the `g_materials` array and `g_default` with:

```c
static const MaterialDef g_materials[] = {
    /* 0 */ {{0.8f,0.2f,0.2f}, 0.2f,  0.6f, 0.1f, 0.0f, 1.0f,  1, GROUP_RED, 0},
    /* 1 */ {{0.2f,0.3f,0.8f}, 0.7f,  0.1f, 0.0f, 0.0f, 1.0f,  0, GROUP_BLUE, 0},
    /* 2 */ {{0.3f,0.7f,0.3f}, 0.9f,  0.0f, 0.0f, 0.0f, 1.0f,  1, GROUP_GROUND, 0},
    /* 3 */ {{0.8f,0.7f,0.3f}, 0.05f, 1.0f, 0.0f, 0.0f, 1.0f,  0, GROUP_METAL, 0},
    /* 4 */ {{0.9f,0.9f,0.9f}, 0.01f, 0.15f,0.0f, 0.5f, 1.5f,  0, GROUP_GLASS, 0},
    /* 5 */ {{1.0f,0.9f,0.7f}, 1.0f,  0.0f, 5.0f, 0.0f, 1.0f,  1, GROUP_LIGHT, 0},
    /* 6 */ {{0.2f,0.9f,0.3f}, 0.005f,0.15f,0.0f, 0.5f, 1.52f, 0, GROUP_GREENGLASS, 0},
    /* 7 */ {{0.2f,0.4f,0.8f}, 0.0f,  0.1f, 0.0f, 1.0f, 1.33f, 0, GROUP_WATER, 0},
    /* 8 */ {{0.55f,0.52f,0.5f},0.85f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0}, // stone_light
    /* 9 */ {{0.32f,0.30f,0.29f},0.9f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0}, // stone_dark
    /* 10 */ {{0.50f,0.48f,0.46f},0.55f,0.30f,0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0}, // stone_mica_low
    /* 11 */ {{0.55f,0.53f,0.50f},0.35f,0.65f,0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0}, // stone_mica_mid
    /* 12 */ {{0.62f,0.59f,0.54f},0.22f,0.90f,0.0f, 0.0f, 1.0f, 1, GROUP_STONE, 0}, // stone_pyrite_fleck
    /* 13 */ {{0.76f,0.70f,0.50f},0.95f,0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_SAND, 1}, // sand -> oriented cubes
};

static const MaterialDef g_default =
    {{0.6f,0.6f,0.6f}, 0.1f, 0.8f, 0.0f, 0.0f, 1.0f, 1, -1, 0};
```

Add `GROUP_SAND` to the merge-group enum at the top of the file:

```c
enum {
    GROUP_RED = 0, GROUP_BLUE = 1, GROUP_GROUND = 2, GROUP_METAL = 3,
    GROUP_GLASS = 4, GROUP_LIGHT = 5, GROUP_GREENGLASS = 6, GROUP_WATER = 7,
    GROUP_STONE = 8, GROUP_SAND = 9
};
```

Implement the accessor next to `MaterialMergeGroup`:

```c
int MaterialMeshingAlgorithm(int materialId) {
    return MaterialRegistryGet(materialId)->meshingAlgorithm;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-reg`
Expected: PASS — `All material_registry tests passed`.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/material_registry.h MatterSurfaceLib/src/material_registry.c MatterSurfaceLib/tests/material_registry_tests.cpp
git commit -m "feat: add material meshingAlgorithm field + sand material"
```

---

## Task 2: Define the MeshingAlgorithm interface + MeshContext

**Files:**
- Create: `MatterSurfaceLib/include/meshing_algorithm.h`

This task is a header-only definition; it is compile-checked by Task 3's test which includes it.

- [ ] **Step 1: Create the interface header**

Create `MatterSurfaceLib/include/meshing_algorithm.h`:

```cpp
#ifndef MESHING_ALGORITHM_H
#define MESHING_ALGORITHM_H

#include "surface.h"            // Particle, Bounds, SurfaceScratch
#include "bvh.h"                // float4 (via precomp.h)
#include "mesh_simplifier.hpp"  // CellBounds
#include "mesh_worker_pool.h"   // GroupMeshResult
#include <vector>
#include <cstdint>

// Which mesher turns a merge group's particles into geometry. Stored on the
// material (MaterialDef.meshingAlgorithm) and resolved per merge group.
enum class MeshAlgorithm { MarchingCubes = 0, OrientedCubes = 1 };

// Everything an algorithm might need to mesh one merge group. Built by
// Cell::build_group_mesh after it resolves the group's particle subset and
// meshing parameters. References/pointers borrow data owned by the caller and
// are valid only for the duration of the generate() call. Algorithms ignore the
// fields they do not use (e.g. OrientedCubes ignores blend/clip/carve/scratch).
struct MeshContext {
    const std::vector<Particle>& particles;       // resolved group particles (post cull/vis-clamp)
    const std::vector<float4>&   particle_tints;  // parallel to particles
    float  max_radius;                            // max effective radius in the set

    Bounds     bounds;        // center, size, divisionPow
    CellBounds cell_bounds;   // min/max bound for boundary locking
    float      voxel;         // actual_size / (gridSize - 1)

    // Isosurface params (marching cubes uses; cubes ignore)
    float blend_width;
    const Particle* clip;   int clip_count;
    const Particle* carve;  int carve_count;
    float carve_blend;
    float simplification_ratio;

    SurfaceScratch* scratch;  // per-worker scratch (MC uses spatial hash; cubes ignore)

    uint32_t group_id;
};

// Abstract mesher. Implementations must be GL-free (CPU only) and reentrant on
// the supplied scratch, so they run on worker threads.
class MeshingAlgorithm {
public:
    virtual ~MeshingAlgorithm() = default;
    virtual GroupMeshResult generate(const MeshContext& ctx) const = 0;
};

// Returns the process-wide singleton for an algorithm. Defined in
// meshing_algorithm.cpp (Task 4).
const MeshingAlgorithm& GetMeshingAlgorithm(MeshAlgorithm algo);

#endif // MESHING_ALGORITHM_H
```

- [ ] **Step 2: Commit**

```bash
git add MatterSurfaceLib/include/meshing_algorithm.h
git commit -m "feat: add MeshingAlgorithm interface and MeshContext"
```

---

## Task 3: Implement OrientedCubeAlgorithm

**Files:**
- Create: `MatterSurfaceLib/include/oriented_cube_algorithm.h`
- Create: `MatterSurfaceLib/src/oriented_cube_algorithm.cpp`
- Test: `MatterSurfaceLib/tests/oriented_cube_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile`

- [ ] **Step 1: Write the failing test**

Create `MatterSurfaceLib/tests/oriented_cube_tests.cpp`:

```cpp
#include "oriented_cube_algorithm.h"
#include "raylib.h"   // MemFree
#include <cstdio>
#include <cmath>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

static MeshContext make_ctx(const std::vector<Particle>& ps, const std::vector<float4>& tints) {
    MeshContext ctx{ps, tints, 1.0f,
                    Bounds{}, CellBounds{}, 1.0f,
                    0.0f, nullptr, 0, nullptr, 0, 0.0f, 1.0f,
                    nullptr, 7u};
    return ctx;
}

int main() {
    OrientedCubeAlgorithm algo;

    Particle p; p.position = Vector3{2.0f, -1.0f, 0.5f}; p.radius = 0.5f; p.materialId = 13;
    std::vector<Particle> ps{p};
    std::vector<float4> tints{make_float4(1.0f, 1.0f, 1.0f, 0.0f)};

    GroupMeshResult r = algo.generate(make_ctx(ps, tints));

    CHECK(r.group_id == 7u, "group_id passes through");
    CHECK(r.mesh.vertexCount == 24, "one cube => 24 vertices");
    CHECK(r.mesh.triangleCount == 12, "one cube => 12 triangles");
    CHECK(r.triangles.size() == 12, "one cube => 12 Tri");
    CHECK(r.triangle_normals.size() == 12, "one cube => 12 TriEx");

    bool tagged = true, unit_normals = true, flat = true;
    for (size_t t = 0; t < r.triangle_normals.size(); ++t) {
        const TriEx& ex = r.triangle_normals[t];
        if (ex.materialId != 13) tagged = false;
        float len = sqrtf(ex.N0.x*ex.N0.x + ex.N0.y*ex.N0.y + ex.N0.z*ex.N0.z);
        if (fabsf(len - 1.0f) > 1e-3f) unit_normals = false;
        if (ex.N0.x != ex.N1.x || ex.N0.y != ex.N1.y || ex.N0.z != ex.N1.z ||
            ex.N0.x != ex.N2.x || ex.N0.y != ex.N2.y || ex.N0.z != ex.N2.z) flat = false;
    }
    CHECK(tagged, "all triangles tagged with source material 13");
    CHECK(unit_normals, "all face normals are unit length");
    CHECK(flat, "each triangle's three vertex normals equal its face normal");

    // Vertices are within the cube's circumradius of the particle center.
    float circum = p.radius * 1.7321f + 1e-3f; // half-edge*sqrt(3)
    bool in_range = true;
    for (int v = 0; v < r.mesh.vertexCount; ++v) {
        float dx = r.mesh.vertices[v*3+0] - p.position.x;
        float dy = r.mesh.vertices[v*3+1] - p.position.y;
        float dz = r.mesh.vertices[v*3+2] - p.position.z;
        if (sqrtf(dx*dx+dy*dy+dz*dz) > circum) in_range = false;
    }
    CHECK(in_range, "all vertices within cube circumradius of center");

    // Determinism: a second build of the same particle is byte-identical.
    GroupMeshResult r2 = algo.generate(make_ctx(ps, tints));
    bool deterministic = (r2.mesh.vertexCount == r.mesh.vertexCount);
    for (int i = 0; deterministic && i < r.mesh.vertexCount*3; ++i) {
        if (r.mesh.vertices[i] != r2.mesh.vertices[i]) deterministic = false;
    }
    CHECK(deterministic, "orientation is stable across re-meshes (deterministic)");

    // Empty group yields an empty result (vertexCount 0 => commit skips it).
    std::vector<Particle> none; std::vector<float4> none_t;
    GroupMeshResult empty = algo.generate(make_ctx(none, none_t));
    CHECK(empty.mesh.vertexCount == 0, "empty group => empty mesh");

    MemFree(r.mesh.vertices);  MemFree(r.mesh.normals);  MemFree(r.mesh.colors);  MemFree(r.mesh.indices);
    MemFree(r2.mesh.vertices); MemFree(r2.mesh.normals); MemFree(r2.mesh.colors); MemFree(r2.mesh.indices);

    if (failures == 0) printf("All oriented_cube tests passed\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Add a test target to the tests Makefile**

In `MatterSurfaceLib/tests/Makefile`, add `run-cube` to the `.PHONY` line, and append this target at the end of the file:

```make
# Oriented-cube meshing algorithm unit tests (headless, GL-free; needs only the
# material registry for albedo and raylib for MemAlloc/MemFree).
CUBE_TARGET = oriented_cube_tests
CUBE_CPP = oriented_cube_tests.cpp ../src/oriented_cube_algorithm.cpp
CUBE_C   = ../src/material_registry.c
CUBE_C_OBJ = material_registry.o

$(CUBE_TARGET): $(CUBE_CPP) $(CUBE_C)
	gcc -c $(CUBE_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(CUBE_CPP) $(CUBE_C_OBJ) -o $(CUBE_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(CUBE_C_OBJ)

run-cube: $(CUBE_TARGET)
	./$(CUBE_TARGET)
```

Update the `.PHONY` line to include `run-cube`:

```make
.PHONY: clean run run-simp run-blas run-cell run-cont run-reg run-tint run-cull run-par run-cube
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-cube`
Expected: FAIL to compile — `oriented_cube_algorithm.h` / `OrientedCubeAlgorithm` not found.

- [ ] **Step 4: Create the algorithm header**

Create `MatterSurfaceLib/include/oriented_cube_algorithm.h`:

```cpp
#ifndef ORIENTED_CUBE_ALGORITHM_H
#define ORIENTED_CUBE_ALGORITHM_H

#include "meshing_algorithm.h"

// Renders each particle in the group as a deterministically oriented cube
// (edge = 2*radius*sizeScale). No SDF, no grid, no scratch: pure per-particle
// geometry. Orientation is seeded from the particle's quantized position so it
// is stable across re-meshes. sizeScale / rotation jitter are read from the
// MSL_CUBE_SIZE_SCALE / MSL_CUBE_ROT_JITTER env vars (defaults 1.0).
class OrientedCubeAlgorithm : public MeshingAlgorithm {
public:
    GroupMeshResult generate(const MeshContext& ctx) const override;
};

#endif // ORIENTED_CUBE_ALGORITHM_H
```

- [ ] **Step 5: Implement the algorithm**

Create `MatterSurfaceLib/src/oriented_cube_algorithm.cpp`:

```cpp
#include "oriented_cube_algorithm.h"
#include "material_registry.h"
#include "raylib.h"     // Mesh, RL_MALLOC, Vector3
#include <cmath>
#include <cstdlib>      // getenv, atof
#include <cstdint>

namespace {

uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

// Quantize position to a stable integer grid and hash into a nonzero seed.
uint32_t seed_from_pos(Vector3 p, float voxel) {
    float q = (voxel > 0.0f) ? voxel : 1.0f;
    int xi = (int)floorf(p.x / q);
    int yi = (int)floorf(p.y / q);
    int zi = (int)floorf(p.z / q);
    uint32_t h = hash_u32((uint32_t)xi * 73856093u ^ (uint32_t)yi * 19349663u ^ (uint32_t)zi * 83492791u);
    return h ? h : 1u;
}

float next_unit(uint32_t& s) {  // xorshift32 -> [0,1)
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return (s & 0xffffffu) / (float)0x1000000;
}

// Uniform random unit quaternion (Shoemake), blended toward identity by jitter.
float4 seeded_quat(uint32_t seed, float jitter) {
    uint32_t s = seed;
    float u1 = next_unit(s), u2 = next_unit(s), u3 = next_unit(s);
    float s1 = sqrtf(1.0f - u1), s2 = sqrtf(u1);
    const float TWO_PI = 6.28318531f;
    float4 q;
    q.x = s1 * sinf(TWO_PI * u2);
    q.y = s1 * cosf(TWO_PI * u2);
    q.z = s2 * sinf(TWO_PI * u3);
    q.w = s2 * cosf(TWO_PI * u3);
    float j = jitter < 0.0f ? 0.0f : (jitter > 1.0f ? 1.0f : jitter);
    q.x *= j; q.y *= j; q.z *= j; q.w = q.w * j + (1.0f - j); // nlerp toward (0,0,0,1)
    float inv = 1.0f / sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    q.x *= inv; q.y *= inv; q.z *= inv; q.w *= inv;
    return q;
}

Vector3 rotate(float4 q, Vector3 v) {
    Vector3 u = { q.x, q.y, q.z };
    Vector3 t = { u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x };
    t.x += q.w*v.x; t.y += q.w*v.y; t.z += q.w*v.z;
    Vector3 c = { u.y*t.z - u.z*t.y, u.z*t.x - u.x*t.z, u.x*t.y - u.y*t.x };
    return Vector3{ v.x + 2.0f*c.x, v.y + 2.0f*c.y, v.z + 2.0f*c.z };
}

// Local unit-cube faces at half-extent 1: outward normal + 4 CCW corners each.
const float FN[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
const float FV[6][4][3] = {
    {{1,-1,-1},{1,1,-1},{1,1,1},{1,-1,1}},     // +X
    {{-1,-1,1},{-1,1,1},{-1,1,-1},{-1,-1,-1}}, // -X
    {{-1,1,-1},{-1,1,1},{1,1,1},{1,1,-1}},     // +Y
    {{-1,-1,1},{-1,-1,-1},{1,-1,-1},{1,-1,1}}, // -Y
    {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}},     // +Z
    {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}}, // -Z
};

} // namespace

GroupMeshResult OrientedCubeAlgorithm::generate(const MeshContext& ctx) const {
    GroupMeshResult result;
    result.group_id = ctx.group_id;

    const int n = (int)ctx.particles.size();
    if (n == 0) return result;

    float sizeScale = 1.0f;
    if (const char* e = getenv("MSL_CUBE_SIZE_SCALE")) { float v = (float)atof(e); if (v > 0.0f) sizeScale = v; }
    float jitter = 1.0f;
    if (const char* e = getenv("MSL_CUBE_ROT_JITTER")) { float v = (float)atof(e); if (v >= 0.0f) jitter = v; }

    const int VPC = 24, TPC = 12;
    int vertexCount = n * VPC;
    int triangleCount = n * TPC;

    Mesh mesh = {};
    mesh.vertexCount = vertexCount;
    mesh.triangleCount = triangleCount;
    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    mesh.normals  = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    mesh.colors   = (unsigned char*)RL_MALLOC(vertexCount * 4 * sizeof(unsigned char));
    mesh.indices  = (unsigned short*)RL_MALLOC(triangleCount * 3 * sizeof(unsigned short));

    result.triangles.reserve(triangleCount);
    result.triangle_normals.reserve(triangleCount);

    for (int i = 0; i < n; ++i) {
        const Particle& p = ctx.particles[i];
        float h = p.radius * sizeScale;  // half-edge (edge = 2*radius*sizeScale)
        float4 q = seeded_quat(seed_from_pos(p.position, ctx.voxel), jitter);

        const MaterialDef* md = MaterialRegistryGet(p.materialId);
        float4 tnt = ctx.particle_tints[i];
        float a = tnt.w;
        unsigned char cr = (unsigned char)(255.0f * (md->albedo[0]*(1.0f-a) + tnt.x*a));
        unsigned char cg = (unsigned char)(255.0f * (md->albedo[1]*(1.0f-a) + tnt.y*a));
        unsigned char cb = (unsigned char)(255.0f * (md->albedo[2]*(1.0f-a) + tnt.z*a));

        int vbase = i * VPC;
        for (int f = 0; f < 6; ++f) {
            Vector3 ln = { FN[f][0], FN[f][1], FN[f][2] };
            Vector3 wn = rotate(q, ln); // unit-length in, unit-length out
            float3 fn3 = make_float3(wn.x, wn.y, wn.z);

            Vector3 cw[4];
            for (int k = 0; k < 4; ++k) {
                Vector3 lc = { FV[f][k][0]*h, FV[f][k][1]*h, FV[f][k][2]*h };
                Vector3 r = rotate(q, lc);
                cw[k] = Vector3{ p.position.x + r.x, p.position.y + r.y, p.position.z + r.z };
                int vi = vbase + f*4 + k;
                mesh.vertices[vi*3+0] = cw[k].x; mesh.vertices[vi*3+1] = cw[k].y; mesh.vertices[vi*3+2] = cw[k].z;
                mesh.normals[vi*3+0] = wn.x; mesh.normals[vi*3+1] = wn.y; mesh.normals[vi*3+2] = wn.z;
                mesh.colors[vi*4+0] = cr; mesh.colors[vi*4+1] = cg; mesh.colors[vi*4+2] = cb; mesh.colors[vi*4+3] = 255;
            }

            int tribase = (i*TPC + f*2);
            unsigned short i0 = (unsigned short)(vbase + f*4);
            unsigned short i1 = i0+1, i2 = i0+2, i3 = i0+3;
            mesh.indices[tribase*3+0]=i0; mesh.indices[tribase*3+1]=i1; mesh.indices[tribase*3+2]=i2;
            mesh.indices[tribase*3+3]=i0; mesh.indices[tribase*3+4]=i2; mesh.indices[tribase*3+5]=i3;

            const int tri_idx[2][3] = {{0,1,2},{0,2,3}};
            for (int t = 0; t < 2; ++t) {
                Vector3 va = cw[tri_idx[t][0]], vb = cw[tri_idx[t][1]], vc = cw[tri_idx[t][2]];
                Tri tri;
                tri.vertex0 = make_float3(va.x, va.y, va.z);
                tri.vertex1 = make_float3(vb.x, vb.y, vb.z);
                tri.vertex2 = make_float3(vc.x, vc.y, vc.z);
                tri.centroid = make_float3((va.x+vb.x+vc.x)/3.0f, (va.y+vb.y+vc.y)/3.0f, (va.z+vb.z+vc.z)/3.0f);
                result.triangles.push_back(tri);

                TriEx ex{};
                ex.N0 = ex.N1 = ex.N2 = fn3;
                ex.materialId = p.materialId;
                ex.tint = tnt;
                result.triangle_normals.push_back(ex);
            }
        }
    }

    result.mesh = mesh;
    return result;
}
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-cube`
Expected: PASS — `All oriented_cube tests passed`.

- [ ] **Step 7: Commit**

```bash
git add MatterSurfaceLib/include/oriented_cube_algorithm.h MatterSurfaceLib/src/oriented_cube_algorithm.cpp MatterSurfaceLib/tests/oriented_cube_tests.cpp MatterSurfaceLib/tests/Makefile
git commit -m "feat: add OrientedCubeAlgorithm with headless tests"
```

---

## Task 4: Extract shared mesh-build helpers

Move `convert_mesh_to_triangles` and `unload_cpu_mesh` out of `cell.cpp` into a shared
unit so the marching-cubes wrapper (Task 5) can reuse them without depending on `cell.cpp`.

**Files:**
- Create: `MatterSurfaceLib/include/mesh_build_utils.h`
- Create: `MatterSurfaceLib/src/mesh_build_utils.cpp`
- Modify: `MatterSurfaceLib/src/cell.cpp`

- [ ] **Step 1: Create the header**

Create `MatterSurfaceLib/include/mesh_build_utils.h`:

```cpp
#ifndef MESH_BUILD_UTILS_H
#define MESH_BUILD_UTILS_H

#include "raylib.h"   // Mesh
#include "bvh.h"      // Tri, TriEx
#include <vector>

// Convert a raylib Mesh's triangles into BVH Tri structs. When out_triex is
// non-null, fills per-triangle TriEx with the mesh's per-vertex normals (or the
// face normal when the mesh has none). materialId/tint are left default and must
// be tagged by the caller.
std::vector<Tri> convert_mesh_to_triangles(const Mesh& mesh, std::vector<TriEx>* out_triex);

// Free a CPU-only mesh's heap arrays WITHOUT any GL call (safe on worker
// threads). Zeroes the Mesh afterward.
void unload_cpu_mesh(Mesh& m);

#endif // MESH_BUILD_UTILS_H
```

- [ ] **Step 2: Create the implementation by moving the two functions**

Create `MatterSurfaceLib/src/mesh_build_utils.cpp` with the two function bodies moved verbatim from `cell.cpp` (the `convert_mesh_to_triangles` body at cell.cpp:157-248 and the `unload_cpu_mesh` body at cell.cpp:314-336). Remove the `static` qualifier from `unload_cpu_mesh`:

```cpp
#include "mesh_build_utils.h"
#include <cstdio>

std::vector<Tri> convert_mesh_to_triangles(const Mesh& mesh, std::vector<TriEx>* out_triex) {
    // ... move the exact body from cell.cpp:158-247 here unchanged ...
}

void unload_cpu_mesh(Mesh& m) {
    MemFree(m.vboId);
    MemFree(m.vertices);
    MemFree(m.texcoords);
    MemFree(m.normals);
    MemFree(m.colors);
    MemFree(m.tangents);
    MemFree(m.texcoords2);
    MemFree(m.indices);
    MemFree(m.animVertices);
    MemFree(m.animNormals);
    MemFree(m.boneWeights);
    MemFree(m.boneIds);
    MemFree(m.boneMatrices);
    m = Mesh{};
}
```

- [ ] **Step 3: Update cell.cpp to use the shared header**

In `MatterSurfaceLib/src/cell.cpp`: add `#include "mesh_build_utils.h"` near the other includes (after line 8), and DELETE the now-moved definitions: the `convert_mesh_to_triangles` function (cell.cpp:157-248) and the `static void unload_cpu_mesh(Mesh& m)` function (cell.cpp:314-336). All existing call sites in `cell.cpp` keep working through the header declaration.

- [ ] **Step 4: Update the tests Makefile to compile the new unit where cell.cpp is used**

The `cell_bounds_tests` and `parallel_mesh_tests` targets compile `../src/cell.cpp`, which now references the moved helpers. Add `../src/mesh_build_utils.cpp` to both `CELL_CPP` and `PAR_CPP` in `MatterSurfaceLib/tests/Makefile`:

```make
CELL_CPP = cell_bounds_tests.cpp ../src/cell.cpp ../src/mesh_simplifier.cpp \
           ../src/blas_manager.cpp ../src/bvh.cpp ../src/bvh_analyzer.cpp \
           ../src/mesh_build_utils.cpp
```

```make
PAR_CPP = parallel_mesh_tests.cpp ../src/cell.cpp ../src/mesh_simplifier.cpp \
          ../src/blas_manager.cpp ../src/bvh.cpp ../src/bvh_analyzer.cpp \
          ../src/mesh_worker_pool.cpp ../src/mesh_build_utils.cpp
```

- [ ] **Step 5: Run the affected tests to verify no behavior change**

Run: `cd MatterSurfaceLib/tests && make run-cell && make run-par`
Expected: both PASS exactly as before the extraction.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/mesh_build_utils.h MatterSurfaceLib/src/mesh_build_utils.cpp MatterSurfaceLib/src/cell.cpp MatterSurfaceLib/tests/Makefile
git commit -m "refactor: extract convert_mesh_to_triangles/unload_cpu_mesh into mesh_build_utils"
```

---

## Task 5: Implement MarchingCubesAlgorithm + the registry

**Files:**
- Create: `MatterSurfaceLib/include/marching_cubes_algorithm.h`
- Create: `MatterSurfaceLib/src/marching_cubes_algorithm.cpp`
- Create: `MatterSurfaceLib/src/meshing_algorithm.cpp`

- [ ] **Step 1: Create the algorithm header**

Create `MatterSurfaceLib/include/marching_cubes_algorithm.h`:

```cpp
#ifndef MARCHING_CUBES_ALGORITHM_H
#define MARCHING_CUBES_ALGORITHM_H

#include "meshing_algorithm.h"

// The existing isosurface mesher: smooth-min SDF union over the group's
// particles, marching cubes extraction, optional simplification, analytic
// gradient normals, then per-triangle nearest-particle material/tint tagging.
class MarchingCubesAlgorithm : public MeshingAlgorithm {
public:
    GroupMeshResult generate(const MeshContext& ctx) const override;
};

#endif // MARCHING_CUBES_ALGORITHM_H
```

- [ ] **Step 2: Implement by moving the generation body from build_group_mesh**

Create `MatterSurfaceLib/src/marching_cubes_algorithm.cpp`. The body is the existing
generation code from `cell.cpp:409-462`, reading inputs from `ctx`:

```cpp
#include "marching_cubes_algorithm.h"
#include "mesh_build_utils.h"
#include "mesh_simplifier.hpp"   // simplify_mesh, SimplifyOptions
#include <cstdio>
extern "C" {
#include "spatial_hash.h"        // sh_query_radius_nearest
}

GroupMeshResult MarchingCubesAlgorithm::generate(const MeshContext& ctx) const {
    GroupMeshResult result;
    result.group_id = ctx.group_id;

    if (ctx.particles.empty()) return result;

    Particle* particles = const_cast<Particle*>(ctx.particles.data());
    int particleCount = (int)ctx.particles.size();
    Particle* clip = const_cast<Particle*>(ctx.clip);
    Particle* carve = const_cast<Particle*>(ctx.carve);

    Mesh mesh = GenerateMeshWithScratch(ctx.scratch, particles, ctx.max_radius, particleCount,
                             ctx.bounds, ctx.blend_width, clip, ctx.clip_count,
                             carve, ctx.carve_count, ctx.carve_blend);

    if (ctx.simplification_ratio < 1.0f && mesh.vertexCount > 0 && mesh.triangleCount > 0) {
        SimplifyOptions so;
        so.target_ratio = ctx.simplification_ratio;
        so.lock_boundary = true;
        CellBounds cb = ctx.cell_bounds;
        Mesh simplified = simplify_mesh(mesh, so, &cb);
        if (simplified.vertexCount > 0 && simplified.triangleCount > 0) {
            ComputeSurfaceNormalsWithScratch(ctx.scratch, &simplified, particles, ctx.max_radius,
                                  particleCount, ctx.blend_width, clip, ctx.clip_count,
                                  carve, ctx.carve_count, ctx.carve_blend);
            unload_cpu_mesh(mesh);
            mesh = simplified;
        } else {
            unload_cpu_mesh(simplified);
        }
    }

    if (mesh.vertexCount <= 0) return result;

    std::vector<TriEx> triangle_normals;
    std::vector<Tri> triangles = convert_mesh_to_triangles(mesh, &triangle_normals);

    SpatialHash* tri_hash = SurfaceScratchHash(ctx.scratch);
    float tri_search = ctx.max_radius * 2.5f + ctx.blend_width * 4.0f;
    for (size_t t = 0; t < triangle_normals.size() && t < triangles.size(); ++t) {
        const float3& c = triangles[t].centroid;
        int bestIdx = 0;
        Particle* nearest = NULL;
        int nfound = tri_hash
            ? sh_query_radius_nearest(tri_hash, c.x, c.y, c.z, tri_search, (void**)&nearest, 1)
            : 0;
        if (nfound > 0 && nearest) {
            bestIdx = (int)(nearest - particles);
        }
        triangle_normals[t].materialId = particles[bestIdx].materialId;
        triangle_normals[t].tint = ctx.particle_tints[bestIdx];
    }

    result.mesh = mesh;
    result.triangles = std::move(triangles);
    result.triangle_normals = std::move(triangle_normals);
    return result;
}
```

- [ ] **Step 3: Create the registry**

Create `MatterSurfaceLib/src/meshing_algorithm.cpp`:

```cpp
#include "meshing_algorithm.h"
#include "marching_cubes_algorithm.h"
#include "oriented_cube_algorithm.h"

const MeshingAlgorithm& GetMeshingAlgorithm(MeshAlgorithm algo) {
    static const MarchingCubesAlgorithm marching_cubes;
    static const OrientedCubeAlgorithm  oriented_cubes;
    switch (algo) {
        case MeshAlgorithm::OrientedCubes: return oriented_cubes;
        case MeshAlgorithm::MarchingCubes:
        default:                           return marching_cubes;
    }
}
```

- [ ] **Step 4: Verify it compiles (covered by Task 6 wiring + build)**

There is no isolated unit test for the marching-cubes wrapper; its behavior is
verified through the existing `cell_bounds_tests` / `parallel_mesh_tests` once
dispatch is wired in Task 6. Compilation is verified at the end of Task 6's build step.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/marching_cubes_algorithm.h MatterSurfaceLib/src/marching_cubes_algorithm.cpp MatterSurfaceLib/src/meshing_algorithm.cpp
git commit -m "feat: add MarchingCubesAlgorithm wrapper and algorithm registry"
```

---

## Task 6: Wire dispatch into Cell::build_group_mesh and the build

**Files:**
- Modify: `MatterSurfaceLib/src/cell.cpp`
- Modify: `MatterSurfaceLib/Makefile`
- Modify: `MatterSurfaceLib/tests/Makefile`
- Test: `MatterSurfaceLib/tests/parallel_mesh_tests.cpp`

- [ ] **Step 1: Write the failing test (cube-material path through the cell)**

Open `MatterSurfaceLib/tests/parallel_mesh_tests.cpp` and read its existing helpers for
building a `Cell` and calling `build_cell_meshes` (it already exercises that path). Add a
new test function that builds a cell whose particles use material id 13 (sand → oriented
cubes) and asserts the group produced 12 triangles per particle. Add, after the existing
tests and before the summary in `main()`:

```cpp
static int test_oriented_cube_material_path() {
    int fails = 0;
    // One sand particle (material 13) in a single cell. Expect a cube: 12 tris.
    Cell cell(Vector3{0,0,0}, 0, 1.0f);
    std::vector<StaticParticle> particles;
    StaticParticle sp{};
    sp.position = Vector3{0.5f, 0.5f, 0.5f};
    sp.radius = 0.3f;
    sp.materialId = 13;
    sp.tint = Vector4{1,1,1,0};
    sp.detail_size = 0.0f;
    particles.push_back(sp);
    cell.add_particle_index(0, 13);

    SurfaceScratch* scratch = CreateSurfaceScratch();
    CellMeshResult res = cell.build_cell_meshes(particles, scratch,
                                                1.0f, 1.0f, 6, 0.0f, nullptr, 0);
    DestroySurfaceScratch(scratch);

    if (res.groups.size() != 1) { printf("FAIL: expected 1 group, got %zu\n", res.groups.size()); return ++fails; }
    const GroupMeshResult& g = res.groups[0];
    if (g.triangle_normals.size() != 12) { printf("FAIL: expected 12 cube tris, got %zu\n", g.triangle_normals.size()); ++fails; }
    bool tagged = true;
    for (const TriEx& ex : g.triangle_normals) if (ex.materialId != 13) tagged = false;
    if (!tagged) { printf("FAIL: cube tris not tagged material 13\n"); ++fails; }
    if (fails == 0) printf("oriented-cube material path OK\n");
    return fails;
}
```

Call it from `main()` alongside the other tests (add its return to the failure tally,
matching the file's existing pattern). If the file's helpers differ from the fields above
(e.g. `StaticParticle` member names), adapt to the actual struct in `include/cluster.h`.

- [ ] **Step 2: Run test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-par`
Expected: FAIL — currently every group is meshed by marching cubes, so a sand particle
produces an isosurface blob (triangle count != 12), not a 12-triangle cube.

- [ ] **Step 3: Replace the generation body in build_group_mesh with dispatch**

In `MatterSurfaceLib/src/cell.cpp`, add includes near the top (after line 8):

```cpp
#include "meshing_algorithm.h"
```

Then in `Cell::build_group_mesh`, KEEP the setup (cell.cpp:343-407, which builds
`particles`, `particle_tints`, `bounds`, `blend_width`, `carve_blend`, `clip`, etc.) and
REPLACE the generation body (the current cell.cpp:409-463, from the `GenerateMeshWithScratch`
call through `return result;`) with context construction + dispatch:

```cpp
    MeshContext ctx{
        particles,
        particle_tints,
        max_radius,
        bounds,
        CellBounds{ min_bound, max_bound },   // min_bound/max_bound are Vector3 members of Cell
        voxel,
        blend_width,
        clipPtr, clipCount,
        const_cast<Particle*>(carveParticles), carveCount,
        carve_blend,
        simplification_ratio,
        scratch,
        group_id
    };

    MeshAlgorithm algo = (MeshAlgorithm)MaterialMeshingAlgorithm(particles[0].materialId);
    return GetMeshingAlgorithm(algo).generate(ctx);
```

Notes for the implementer:
- `particle_tints`, `max_radius`, `clipPtr`, `clipCount`, `carve_blend`, `blend_width`,
  `voxel`, and `bounds` are all already computed in the existing setup (cell.cpp:368-407).
- `CellBounds` is `{ Vector3 min_bound; Vector3 max_bound; }`, and `Cell::min_bound` /
  `Cell::max_bound` are already `Vector3`, so `CellBounds{ min_bound, max_bound }` is a
  direct copy (this mirrors the existing cell.cpp:414-415 assignment).
- `convert_mesh_to_triangles`, the `simplify_mesh` block, and the spatial-hash tagging loop
  now live in `MarchingCubesAlgorithm::generate` (Task 5) and must be removed from
  `build_group_mesh` as part of this replacement.

- [ ] **Step 4: Add the new sources to the main Makefile**

In `MatterSurfaceLib/Makefile`, add the three new C++ sources to `SRC` (line 129) and their
objects to `OBJ` (line 130):

Append to `SRC`: `src/meshing_algorithm.cpp src/marching_cubes_algorithm.cpp src/oriented_cube_algorithm.cpp src/mesh_build_utils.cpp`

Append to `OBJ`: `$(OBJ_DIR)/meshing_algorithm.o $(OBJ_DIR)/marching_cubes_algorithm.o $(OBJ_DIR)/oriented_cube_algorithm.o $(OBJ_DIR)/mesh_build_utils.o`

Add build rules near the other `$(OBJ_DIR)/*.o` C++ rules:

```make
$(OBJ_DIR)/meshing_algorithm.o: src/meshing_algorithm.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/marching_cubes_algorithm.o: src/marching_cubes_algorithm.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/oriented_cube_algorithm.o: src/oriented_cube_algorithm.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@

$(OBJ_DIR)/mesh_build_utils.o: src/mesh_build_utils.cpp
	$(CXX) -c $< $(CXXFLAGS) -o $@
```

- [ ] **Step 5: Add the new sources to the parallel/cell test targets**

In `MatterSurfaceLib/tests/Makefile`, the `cell_bounds_tests` and `parallel_mesh_tests`
targets compile `cell.cpp`, which now dispatches into the algorithm classes. Append the
new C++ sources to both `CELL_CPP` and `PAR_CPP` (alongside the `mesh_build_utils.cpp`
added in Task 4):

```make
CELL_CPP = cell_bounds_tests.cpp ../src/cell.cpp ../src/mesh_simplifier.cpp \
           ../src/blas_manager.cpp ../src/bvh.cpp ../src/bvh_analyzer.cpp \
           ../src/mesh_build_utils.cpp ../src/meshing_algorithm.cpp \
           ../src/marching_cubes_algorithm.cpp ../src/oriented_cube_algorithm.cpp
```

```make
PAR_CPP = parallel_mesh_tests.cpp ../src/cell.cpp ../src/mesh_simplifier.cpp \
          ../src/blas_manager.cpp ../src/bvh.cpp ../src/bvh_analyzer.cpp \
          ../src/mesh_worker_pool.cpp ../src/mesh_build_utils.cpp \
          ../src/meshing_algorithm.cpp ../src/marching_cubes_algorithm.cpp \
          ../src/oriented_cube_algorithm.cpp
```

- [ ] **Step 6: Run the cube-path test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-par`
Expected: PASS, including `oriented-cube material path OK`.

- [ ] **Step 7: Run the marching-cubes regression to verify no behavior change**

Run: `cd MatterSurfaceLib/tests && make run-cell && make run-cont`
Expected: both PASS — existing all-marching-cubes materials are unchanged.

- [ ] **Step 8: Build the full app to verify Makefile wiring**

Run: `cd MatterSurfaceLib && make WSL_LINUX=1`
Expected: links successfully, prints `Built executable for linux`.

- [ ] **Step 9: Commit**

```bash
git add MatterSurfaceLib/src/cell.cpp MatterSurfaceLib/Makefile MatterSurfaceLib/tests/Makefile MatterSurfaceLib/tests/parallel_mesh_tests.cpp
git commit -m "feat: dispatch per-merge-group meshing via material-selected algorithm"
```

---

## Task 7: Full test sweep

**Files:** none (verification only)

- [ ] **Step 1: Run the whole headless suite**

Run: `cd MatterSurfaceLib/tests && make run-reg && make run-cube && make run-cell && make run-cont && make run-par && make run-simp && make run-blas && make run-tint && make run-cull`
Expected: every target prints its pass line; no `FAIL:`.

- [ ] **Step 2: Run the top-level build/test script**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && ./build-all.sh test`
Expected: builds all projects and runs headless suites without failure. If `build-all.sh`
does not already invoke the MatterSurfaceLib test targets, that is pre-existing behavior —
do not expand its scope here.

- [ ] **Step 3: Manual visual check (cannot be automated headless)**

The oriented-cube look can only be confirmed in the GL app, which needs a display. Ask the
user to run the app (sand material 13) and confirm sand particles render as raytraced cubes.
State explicitly that the cube appearance was verified by tests for geometry/normals/tagging
but the final rendered look was confirmed by the user, not automated.

---

## Self-Review Notes

- **Spec coverage:** material-driven selection (Task 1), one-algorithm-per-merge-group via
  representative material (Task 6 dispatch), `MeshingAlgorithm` interface + `MeshContext`
  (Task 2), MC wrapper preserving behavior (Tasks 4-5, verified Task 6 steps 7-8),
  oriented-cube algorithm with deterministic orientation + face normals through the same
  BLAS path (Task 3), env-var params (Task 3 impl). All spec sections map to a task.
- **Out-of-scope honored:** no watertightness handling, no per-particle selection, no merge-
  group splitting, no `MaterialDef`/UI exposure of cube params (env-vars only).
- **Type consistency:** `GroupMeshResult`, `MeshContext`, `MeshAlgorithm`,
  `MaterialMeshingAlgorithm`, `convert_mesh_to_triangles`, `unload_cpu_mesh`,
  `GetMeshingAlgorithm` are used with identical signatures across tasks.
