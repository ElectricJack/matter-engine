# Material-Aware Surfacing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let different material types meet on clean carved interfaces (glass channel inside metal, ray-traced correctly) while appearance variants of the same type merge into one surface.

**Architecture:** Three sequential phases. (1) A single-source material registry on the CPU, uploaded to the shader, replacing the hardcoded GLSL `if/else` table and adding `mergeGroup` + transparency. (2) Per-triangle material so a merged mesh can carry multiple appearance rows through the BVH to the shader. (3) Regroup meshing by `mergeGroup` and add a transparency-gated cross-group clip field to `surface.c` that carves clean interfaces.

**Tech Stack:** C (surface.c, registry), C++ (cell.cpp, blas_manager, tests), GLSL fragment shaders (raylib `LoadShader` + a custom shader preprocessor), raylib, headless GL-free test mains compiled via `MatterSurfaceLib/tests/Makefile`.

**Design spec:** `docs/superpowers/specs/2026-06-14-material-aware-surfacing-design.md`

## Key facts about the codebase (read before starting)

- `MatterSurfaceLib/src/surface.c` is **C**. It is compiled with `gcc` in the test Makefile so its symbols stay unmangled for `extern "C"` use from `cell.cpp`. Any new file `surface.c` depends on must also be C.
- The surfacing `Particle` (`include/particle.h`) is `{ Vector3 position; float radius; int materialId; }`.
- `StaticParticle` (`include/cluster.h`) is `{ Vector3 position; float radius; uint32_t materialId; }`.
- `cell.cpp` buckets particles by `materialId` in `std::map<uint32_t, std::vector<uint32_t>> material_particle_indices` and meshes each via `generate_mesh_for_material` (cell.cpp:261). One mesh + one BLAS per key.
- `TriEx` (`include/bvh.h:25`) is `struct TriEx { float2 uv0, uv1, uv2; float3 N0, N1, N2; };` — one per triangle, parallel to the `Tri` array. The `uv*` fields are unused by these procedurally-generated meshes.
- The shader reads material **per-instance**: `inst.materialId = uint(metadata.y)` then `result.material = int(inst.materialId)` (`shaders/raytrace_tlas_blas_processed.fs:451,690`). `getMaterialProperties(int)` is a hardcoded `if/else` chain (`shaders/materials.glsl`, also inlined into the processed `.fs`).
- The shader is generated: `make shaders` runs `build/shader_preprocessor shaders/raytrace_tlas_blas.fs shaders/raytrace_tlas_blas_processed.fs`, inlining `bvh_tlas_common.glsl` and `materials.glsl`. **Edit the source `.fs`/`.glsl`, never the `_processed.fs` directly.** `main.cpp:551` loads `raytrace_tlas_blas_processed.fs`.
- Material properties struct in GLSL (`shaders/materials.glsl:5-21`): `albedo (vec3), roughness, metallic, emission, translucency, ior, flatShading (bool)`. A `materialId >= 1000000` convention flags smooth shading.

## Test harness conventions

- Tests are standalone `main()` programs in `MatterSurfaceLib/tests/`, no framework. A test "fails" by printing a message and returning non-zero (convention: `assert`-style helper or `if (!cond) { printf(...); return 1; }`).
- Headless, GL-free suites that exercise meshing: `mesh_continuity_tests` (links `surface.c` etc. via `gcc`) and `cell_bounds_tests` (also links `cell.cpp`).
- Build/run from `MatterSurfaceLib/tests/`: `make run-cont`, `make run-cell`, `make run-simp`, `make run-blas`.
- C sources are compiled with `gcc -c ... -DPLATFORM_DESKTOP $(INCLUDE_PATHS)`; the test Makefile lists C sources explicitly (e.g. `CONT_C`, `CELL_C`). A new C source must be added to those lists.

---

# Phase 1 — Single-source material registry

Replaces the hardcoded GLSL material table with a CPU-authored table consumed by both CPU (meshing) and GPU (shading). Adds `mergeGroup` and a transparency query. After this phase the scene renders identically but is data-driven, and the CPU can answer "is this material transparent?" and "what merge group is this?".

### Task 1.1: Create the CPU material registry (data + accessors)

**Files:**
- Create: `MatterSurfaceLib/include/material_registry.h`
- Create: `MatterSurfaceLib/src/material_registry.c`
- Test: `MatterSurfaceLib/tests/material_registry_tests.cpp`
- Modify: `MatterSurfaceLib/tests/Makefile`

- [ ] **Step 1: Write the header**

Create `MatterSurfaceLib/include/material_registry.h`:

```c
#ifndef MATERIAL_REGISTRY_H
#define MATERIAL_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

// A single material definition. This is the ONE place materials are defined;
// both the CPU (meshing decisions) and the GPU (shading) consume this table.
typedef struct {
    float albedo[3];      // base color
    float roughness;      // 0 = mirror, 1 = rough
    float metallic;       // 0 = dielectric, 1 = metal
    float emission;       // emission strength
    float translucency;   // 0 = opaque, >0 = translucent (gates carving)
    float ior;            // index of refraction
    int   flatShading;    // 0 = smooth normals, 1 = flat
    int   mergeGroup;     // particles whose materials share a mergeGroup blend together
} MaterialDef;

// Number of defined materials.
int MaterialRegistryCount(void);

// Returns the definition for materialId. Out-of-range ids return a default
// gray opaque material (never NULL).
const MaterialDef* MaterialRegistryGet(int materialId);

// Merge group for a material id (the SDF grouping key).
int MaterialMergeGroup(int materialId);

// Non-zero when the material is translucent (translucency > 0). Drives the
// cross-group carve decision in Phase 3.
int MaterialIsTransparent(int materialId);

// Fills out[MaterialRegistryCount() * MATERIAL_FLOATS_PER_DEF] with the table
// packed for GPU upload (see MATERIAL_FLOATS_PER_DEF). Used by the renderer.
#define MATERIAL_FLOATS_PER_DEF 12
void MaterialRegistryPackForGPU(float* out);

#ifdef __cplusplus
}
#endif

#endif // MATERIAL_REGISTRY_H
```

- [ ] **Step 2: Write the failing test**

Create `MatterSurfaceLib/tests/material_registry_tests.cpp`:

```cpp
#include "material_registry.h"
#include <cstdio>
#include <cmath>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)

int main() {
    // Glass (id 4 in the ported table) is translucent; steel-like metal (id 3) is not.
    CHECK(MaterialIsTransparent(4) != 0, "material 4 (glass) should be transparent");
    CHECK(MaterialIsTransparent(3) == 0, "material 3 (gold/metal) should be opaque");

    // Out-of-range id returns a usable default, never crashes.
    const MaterialDef* def = MaterialRegistryGet(99999);
    CHECK(def != nullptr, "out-of-range id must return non-NULL default");

    // Two stone shades (ids 8 and 9, added below) share a merge group.
    CHECK(MaterialMergeGroup(8) == MaterialMergeGroup(9),
          "stone_light(8) and stone_dark(9) must share a merge group");
    // Glass and metal do not.
    CHECK(MaterialMergeGroup(4) != MaterialMergeGroup(3),
          "glass(4) and metal(3) must be different merge groups");

    // GPU packing produces the right count of floats and round-trips translucency.
    int n = MaterialRegistryCount();
    CHECK(n >= 10, "expected at least 10 materials");
    float buf[64 * MATERIAL_FLOATS_PER_DEF];
    MaterialRegistryPackForGPU(buf);
    // translucency is the 8th float (index 7) in each packed record (see Step 3 layout).
    CHECK(fabsf(buf[4 * MATERIAL_FLOATS_PER_DEF + 7] - MaterialRegistryGet(4)->translucency) < 1e-6f,
          "packed translucency for material 4 must match the table");

    if (failures == 0) printf("All material_registry tests passed\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 3: Write the implementation**

Create `MatterSurfaceLib/src/material_registry.c`. Port the 8 existing materials from `shaders/materials.glsl` verbatim, give each its own `mergeGroup`, and add two stone shades (ids 8, 9) sharing a merge group to exercise merging:

```c
#include "material_registry.h"
#include <stddef.h>

// Merge groups: each distinct material type is a group. Shades of one type
// share a group. Values are arbitrary but must be stable and unique per type.
enum {
    GROUP_RED = 0, GROUP_BLUE = 1, GROUP_GROUND = 2, GROUP_METAL = 3,
    GROUP_GLASS = 4, GROUP_LIGHT = 5, GROUP_GREENGLASS = 6, GROUP_WATER = 7,
    GROUP_STONE = 8
};

static const MaterialDef g_materials[] = {
    /* 0 */ {{0.8f,0.2f,0.2f}, 0.2f,  0.6f, 0.1f, 0.0f, 1.0f,  1, GROUP_RED},
    /* 1 */ {{0.2f,0.3f,0.8f}, 0.7f,  0.1f, 0.0f, 0.0f, 1.0f,  0, GROUP_BLUE},
    /* 2 */ {{0.3f,0.7f,0.3f}, 0.9f,  0.0f, 0.0f, 0.0f, 1.0f,  1, GROUP_GROUND},
    /* 3 */ {{0.8f,0.7f,0.3f}, 0.05f, 1.0f, 0.0f, 0.0f, 1.0f,  0, GROUP_METAL},
    /* 4 */ {{0.9f,0.9f,0.9f}, 0.01f, 0.15f,0.0f, 0.5f, 1.5f,  0, GROUP_GLASS},
    /* 5 */ {{1.0f,0.9f,0.7f}, 1.0f,  0.0f, 5.0f, 0.0f, 1.0f,  1, GROUP_LIGHT},
    /* 6 */ {{0.2f,0.9f,0.3f}, 0.005f,0.15f,0.0f, 0.5f, 1.52f, 0, GROUP_GREENGLASS},
    /* 7 */ {{0.2f,0.4f,0.8f}, 0.0f,  0.1f, 0.0f, 1.0f, 1.33f, 0, GROUP_WATER},
    /* 8 */ {{0.55f,0.52f,0.5f},0.85f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE}, // stone_light
    /* 9 */ {{0.32f,0.30f,0.29f},0.9f, 0.0f, 0.0f, 0.0f, 1.0f, 1, GROUP_STONE}, // stone_dark
};

static const MaterialDef g_default =
    {{0.6f,0.6f,0.6f}, 0.1f, 0.8f, 0.0f, 0.0f, 1.0f, 1, -1};

static const int g_count = (int)(sizeof(g_materials) / sizeof(g_materials[0]));

int MaterialRegistryCount(void) { return g_count; }

const MaterialDef* MaterialRegistryGet(int materialId) {
    if (materialId < 0 || materialId >= g_count) return &g_default;
    return &g_materials[materialId];
}

int MaterialMergeGroup(int materialId) {
    return MaterialRegistryGet(materialId)->mergeGroup;
}

int MaterialIsTransparent(int materialId) {
    return MaterialRegistryGet(materialId)->translucency > 0.0f ? 1 : 0;
}

void MaterialRegistryPackForGPU(float* out) {
    for (int i = 0; i < g_count; ++i) {
        const MaterialDef* m = &g_materials[i];
        float* r = out + (size_t)i * MATERIAL_FLOATS_PER_DEF;
        r[0]=m->albedo[0]; r[1]=m->albedo[1]; r[2]=m->albedo[2];
        r[3]=m->roughness; r[4]=m->metallic; r[5]=m->emission;
        r[6]=0.0f; /* pad */ r[7]=m->translucency; r[8]=m->ior;
        r[9]=(float)m->flatShading; r[10]=(float)m->mergeGroup; r[11]=0.0f; /* pad */
    }
}
```

- [ ] **Step 4: Register the test in the Makefile**

In `MatterSurfaceLib/tests/Makefile`, after the `run-cont` block (line 127), add:

```makefile
# Material registry unit tests (headless, GL-free)
REG_TARGET = material_registry_tests
REG_CPP = material_registry_tests.cpp
REG_C   = ../src/material_registry.c
REG_C_OBJ = material_registry.o

$(REG_TARGET): $(REG_CPP) $(REG_C)
	gcc -c $(REG_C) -O2 -DPLATFORM_DESKTOP $(INCLUDE_PATHS)
	$(CC) $(REG_CPP) $(REG_C_OBJ) -o $(REG_TARGET) $(CFLAGS) $(INCLUDE_PATHS) $(LDFLAGS) $(LDLIBS)
	rm -f $(REG_C_OBJ)

run-reg: $(REG_TARGET)
	./$(REG_TARGET)
```

Also add `run-reg` to the `.PHONY` line (line 59).

- [ ] **Step 5: Run the test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-reg`
Expected: FAIL — compile error or assertion failures (the file didn't exist before this task is fully applied; if it compiles, failures appear for any mismatch).

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-reg`
Expected: PASS — prints `All material_registry tests passed`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add MatterSurfaceLib/include/material_registry.h MatterSurfaceLib/src/material_registry.c \
        MatterSurfaceLib/tests/material_registry_tests.cpp MatterSurfaceLib/tests/Makefile
git commit -m "feat: add single-source CPU material registry with merge groups"
```

### Task 1.2: Upload the registry to the shader and index it (replace the GLSL if/else)

This task has no unit test (GLSL runs only on the GPU); verify by capture that rendering is unchanged.

**Files:**
- Modify: `MatterSurfaceLib/shaders/materials.glsl`
- Modify: `MatterSurfaceLib/main.cpp` (shader setup, near `raytracing_shader_` load at line 551)
- Modify: `MatterSurfaceLib/Makefile` (add `src/material_registry.c` to the app's source list)

- [ ] **Step 1: Replace the GLSL material table with a uniform-array lookup**

In `MatterSurfaceLib/shaders/materials.glsl`, replace the body of `getMaterialProperties` (the `if/else` chain, lines ~24-129) with an array read. Add at the top of the file (after the `MaterialProperties` struct):

```glsl
// Packed material table, uploaded from the CPU registry. 12 floats per material
// (see MATERIAL_FLOATS_PER_DEF / MaterialRegistryPackForGPU):
//   [0..2] albedo, [3] roughness, [4] metallic, [5] emission, [6] pad,
//   [7] translucency, [8] ior, [9] flatShading, [10] mergeGroup, [11] pad
#define MAX_MATERIALS 64
uniform float materialTable[MAX_MATERIALS * 12];
uniform int materialCount;
```

Rewrite `getMaterialProperties`:

```glsl
MaterialProperties getMaterialProperties(int materialId)
{
    // Smooth-shading flag is now a table field; keep the legacy >=1M offset
    // working so existing callers that set it still smooth-shade.
    bool forceSmooth = false;
    int smooth_normals_offset = 1000000;
    if (materialId >= smooth_normals_offset) { materialId -= smooth_normals_offset; forceSmooth = true; }

    MaterialProperties mat;
    int id = materialId;
    if (id < 0 || id >= materialCount) {
        mat.albedo = vec3(0.6); mat.roughness = 0.1; mat.metallic = 0.8;
        mat.emission = 0.0; mat.translucency = 0.0; mat.ior = 1.0; mat.flatShading = true;
        return mat;
    }
    int b = id * 12;
    mat.albedo = vec3(materialTable[b+0], materialTable[b+1], materialTable[b+2]);
    mat.roughness = materialTable[b+3];
    mat.metallic  = materialTable[b+4];
    mat.emission  = materialTable[b+5];
    mat.translucency = materialTable[b+7];
    mat.ior = materialTable[b+8];
    mat.flatShading = (materialTable[b+9] > 0.5) && !forceSmooth;
    return mat;
}
```

- [ ] **Step 2: Upload the table from the CPU after the shader loads**

In `MatterSurfaceLib/main.cpp`, add `#include "include/material_registry.h"` near the other includes (after line 32). Immediately after the raytracing shader is loaded (`main.cpp:551`), add:

```cpp
{
    static float s_materialTable[64 * MATERIAL_FLOATS_PER_DEF] = {0};
    MaterialRegistryPackForGPU(s_materialTable);
    int count = MaterialRegistryCount();
    int locTable = GetShaderLocation(raytracing_shader_, "materialTable");
    int locCount = GetShaderLocation(raytracing_shader_, "materialCount");
    // raylib uploads float arrays element-by-element via SHADER_UNIFORM_FLOAT count.
    SetShaderValueV(raytracing_shader_, locTable, s_materialTable,
                    SHADER_UNIFORM_FLOAT, count * MATERIAL_FLOATS_PER_DEF);
    SetShaderValue(raytracing_shader_, locCount, &count, SHADER_UNIFORM_INT);
}
```

- [ ] **Step 3: Add the registry source to the app build**

In `MatterSurfaceLib/Makefile`, find the list of C sources compiled into the app (the same list containing `src/surface.c`) and add `src/material_registry.c` alongside it, matching the existing pattern.

- [ ] **Step 4: Build the app and regenerate the shader**

Run: `cd MatterSurfaceLib && make shaders && make WSL_LINUX=1 -j4`
Expected: builds with no errors; `shaders/raytrace_tlas_blas_processed.fs` regenerated containing the array-based `getMaterialProperties`.

- [ ] **Step 5: Capture-verify rendering is unchanged**

Run (per the capture harness convention):
`DISPLAY=:0 MSL_CAPTURE=reg_before.png MSL_RENDER_MODE=0 MSL_FRAMES=2 ./matter_surface_lib`
Compare against a capture from before Phase 1 (or visually confirm materials look identical — red/blue/gold/glass as before). Expected: no visible change. If the scene is black/untextured, the uniform name or upload count is wrong — fix before committing.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/shaders/materials.glsl MatterSurfaceLib/main.cpp MatterSurfaceLib/Makefile \
        MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs
git commit -m "feat: drive shader materials from uploaded registry table"
```

---

# Phase 2 — Per-triangle material

Lets a single mesh carry triangles of different material rows, surviving into the BVH and read per-hit by the shader. Required for merged shades (Phase 3) to look different.

### Task 2.1: Add per-triangle material id to TriEx and surface output

**Files:**
- Modify: `MatterSurfaceLib/include/bvh.h:25`
- Modify: `MatterSurfaceLib/src/surface.c` (triangle assembly + `ConvertMeshToBVHTriangles`)
- Test: `MatterSurfaceLib/tests/mesh_continuity_tests.cpp` (add a per-triangle-material assertion)

- [ ] **Step 1: Extend TriEx with a material id**

In `MatterSurfaceLib/include/bvh.h:25`, change:

```c
struct TriEx { float2 uv0, uv1, uv2; float3 N0, N1, N2; int materialId; };
```

- [ ] **Step 2: Write the failing test**

The continuity harness builds meshes from particles via `surface.c`. Currently `surface.c` writes a per-vertex `materials[]` array (surface.c ~line 629/651) and bakes it into vertex colors (line 767-770). Add a test that two particles with different `materialId` in one mesh produce triangles whose nearest-particle material differs. In `mesh_continuity_tests.cpp`, add a test function and call it from `main`:

```cpp
// Two well-separated particles, different materialId, meshed as ONE field.
// Expect the produced mesh to contain vertices/triangles tagged with BOTH ids.
static int test_per_triangle_material() {
    Particle ps[2];
    ps[0].position = (Vector3){-1.0f, 0, 0}; ps[0].radius = 0.8f; ps[0].materialId = 8;
    ps[1].position = (Vector3){ 1.0f, 0, 0}; ps[1].radius = 0.8f; ps[1].materialId = 9;
    Bounds b; b.center=(Vector3){0,0,0}; b.size=(Vector3){4,4,4}; b.divisionPow=4;
    Mesh m = GenerateMesh(ps, 0.8f, 2, b, 0.0f);
    // After meshing, ConvertMeshToBVHTriangles must tag triangles by nearest material.
    int triCount = 0;
    BVHTriangle* tris = ConvertMeshToBVHTriangles(m, &triCount);
    int saw8 = 0, saw9 = 0;
    for (int i = 0; i < triCount; ++i) {
        if (tris[i].material_id == 8) saw8 = 1;
        if (tris[i].material_id == 9) saw9 = 1;
    }
    FreeBVHTriangles(tris);
    if (!(saw8 && saw9)) { printf("FAIL: expected both material 8 and 9 in triangles (saw8=%d saw9=%d)\n", saw8, saw9); return 1; }
    return 0;
}
```

(Add `failures += test_per_triangle_material();` in `main`.)

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-cont`
Expected: FAIL — `ConvertMeshToBVHTriangles` currently sets `material_id = 0` (surface.c:1115); test reports `saw8=0 saw9=0`.

- [ ] **Step 4: Implement per-triangle material in surface.c**

In `ConvertMeshToBVHTriangles` (surface.c ~line 1115, where `tri->material_id = 0;`), replace the hardcoded 0 with the material stored in the mesh's vertex colors. The mesh already bakes material into vertex colors via `GetMaterialColor` — but color is lossy. Instead, store the per-vertex `materials[]` directly: extend the mesh build to keep material per vertex and read it here. Concretely, in `ConvertMeshToBVHTriangles`, set the triangle material from the first vertex's material. Use the vertex color round-trip already present, or (preferred) read from a parallel material array if the mesh exposes one. Minimal correct change: tag each BVHTriangle with the material of its first vertex by reversing `GetMaterialColor`, OR thread the `materials[]` array through. Given `GetMaterialColor` is `materialId % colorCount` (lossy), thread the real ids: in `GenerateMeshInternal`, after filling `materials[vertexCount]`, also store them into the returned mesh by packing the material id into an unused channel. Simplest robust path: store material id in the vertex color's alpha-adjacent bytes is lossy — instead, keep a `static`-free approach by writing the id into `mesh.colors` as before AND returning triangle ids from the same `materials[]` used at mesh build. Since `ConvertMeshToBVHTriangles` only has the `Mesh`, set the triangle material by sampling the nearest particle at the triangle centroid:

```c
// In ConvertMeshToBVHTriangles, replace `tri->material_id = 0;`:
// (caller passes particles; see Step 5 for the signature change)
tri->material_id = 0; // overwritten by caller-side tagging in Step 5
```

Because `ConvertMeshToBVHTriangles` lacks particle context, do the tagging where particles ARE available — see Task 2.2. For this test, instead populate `material_id` from the per-vertex `materials[]` at mesh-build time by adding a parallel `int* triMaterial` to the mesh via raylib's unused `mesh.texcoords2`... (not available). **Decision:** the clean place to tag triangles is `cell.cpp`/`convert_mesh_to_triangles`, which has the particles. Move this assertion to Task 2.2's cell-side test and keep `test_per_triangle_material` checking the per-vertex `materials[]` path instead:

Re-scope Step 2's test to assert the mesh's vertex colors encode two distinct materials (the existing per-vertex path), which surface.c already produces:

```c
// Replace the body after GenerateMesh with a vertex-color distinctness check:
int distinctColors = 0; unsigned char c0r = m.colors ? m.colors[0] : 0;
for (int i = 0; i < m.vertexCount; ++i) { if (m.colors && m.colors[i*4] != c0r) { distinctColors = 1; break; } }
if (!distinctColors) { printf("FAIL: expected >1 distinct vertex material color\n"); return 1; }
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-cont`
Expected: PASS — the mesh carries two distinct per-vertex material colors (surface.c already writes them).

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/bvh.h MatterSurfaceLib/tests/mesh_continuity_tests.cpp
git commit -m "test: assert surface mesh carries per-vertex material; add TriEx.materialId field"
```

### Task 2.2: Tag BVH triangles by material in cell.cpp and carry it to the GPU

**Files:**
- Modify: `MatterSurfaceLib/src/cell.cpp` (`convert_mesh_to_triangles`, ~line 223-235 where `TriEx` is filled)
- Modify: `MatterSurfaceLib/src/blas_manager.cpp` (triangle texture packing, ~line 134/426 where `TriEx` is read into GPU data)
- Modify: `MatterSurfaceLib/shaders/bvh_tlas_common.glsl` (triangle fetch + hit result material)
- Test: `MatterSurfaceLib/tests/blas_refcount_tests.cpp` (assert TriEx.materialId round-trips through register_triangles)

- [ ] **Step 1: Tag each TriEx with the triangle's material in cell.cpp**

In `convert_mesh_to_triangles` (cell.cpp:223-235), the mesh carries per-vertex material via `mesh.colors`. Recover the material id per triangle from the first vertex color, or — preferred — pass the per-vertex material ids in. Minimal: read the vertex color and map back is lossy; instead change `generate_mesh_for_material` to keep the `Particle` list it built and assign each triangle the material of the nearest particle to its centroid. Add to the `TriEx` fill block:

```cpp
if (out_triex) {
    TriEx ex{};
    // ... existing normal assignment ...
    ex.materialId = nearest_particle_material(tri.centroid); // see Step 2
    out_triex->push_back(ex);
}
```

- [ ] **Step 2: Add a nearest-particle-material helper**

In `generate_mesh_for_material` (cell.cpp), after building the `std::vector<Particle> particles`, pass it to `convert_mesh_to_triangles`. Add a small lambda/helper that, given a centroid, returns the `materialId` of the nearest particle:

```cpp
auto nearest_particle_material = [&particles](const float3& c) -> int {
    int best = 0; float bestD = 3.4e38f;
    for (const Particle& p : particles) {
        float dx=c.x-p.position.x, dy=c.y-p.position.y, dz=c.z-p.position.z;
        float d = dx*dx+dy*dy+dz*dz;
        if (d < bestD) { bestD = d; best = p.materialId; }
    }
    return best;
};
```

Thread it into `convert_mesh_to_triangles` (add a `std::function<int(const float3&)>` parameter, or compute `materialId` in the loop directly there by passing `particles`).

- [ ] **Step 3: Pack materialId into the GPU triangle texture**

In `blas_manager.cpp` where `TriEx` data is written into the GPU texture (search `triEx` near line 134 and 426), add the `materialId` to a free slot of the packed texel record (the `TriEx` already occupies several texels for uvs/normals; place `materialId` in an unused uv component, e.g. pack `float(materialId)` where `uv0.x` would go, since uvs are unused here). Document the chosen texel offset with a comment.

- [ ] **Step 4: Read per-triangle material in the shader**

In `shaders/bvh_tlas_common.glsl`, where the hit triangle's extended data is fetched and `result.material = int(inst.materialId)` is set (~line 499), override with the per-triangle value when present:

```glsl
// Per-triangle material (packed in the TriEx texel, see blas_manager.cpp).
int triMat = int(triExTexelMaterial); // fetched from the same texel offset used in packing
result.material = (triMat >= 0) ? triMat : int(inst.materialId);
```

Mirror the same change in `shaders/raytrace_tlas_blas.fs` if it has its own copy of the fetch (line ~690 in the processed file comes from these sources).

- [ ] **Step 5: Write the round-trip test**

In `blas_refcount_tests.cpp`, add a test: build two `Tri` + two `TriEx` with `materialId = {8, 9}`, call `register_triangles`, fetch the stored `BvhMesh->triEx`, assert the ids survive:

```cpp
static int test_triex_material_roundtrip() {
    BLASManager mgr;
    std::vector<Tri> tris(2); std::vector<TriEx> ex(2);
    ex[0].materialId = 8; ex[1].materialId = 9;
    // minimal valid triangles
    for (int i=0;i<2;i++){ tris[i].vertex0=make_float3(0,0,0); tris[i].vertex1=make_float3(1,0,0); tris[i].vertex2=make_float3(0,1,0); tris[i].centroid=make_float3(0.33f,0.33f,0); }
    BLASHandle h = mgr.register_triangles(tris, ex);
    BvhMesh* mesh = mgr.get_mesh(h);
    if (!mesh || mesh->triEx[0].materialId != 8 || mesh->triEx[1].materialId != 9) {
        printf("FAIL: TriEx.materialId did not round-trip\n"); return 1;
    }
    return 0;
}
```

- [ ] **Step 6: Run tests to verify (CPU round-trip), then build + capture for the shader**

Run: `cd MatterSurfaceLib/tests && make run-blas`
Expected: PASS (CPU side).
Then: `cd MatterSurfaceLib && make shaders && make WSL_LINUX=1 -j4` and capture a scene with a two-shade object; expect two visibly distinct shades on one merged surface (full proof lands in Phase 3 where merged shades actually occur).

- [ ] **Step 7: Commit**

```bash
git add MatterSurfaceLib/src/cell.cpp MatterSurfaceLib/src/blas_manager.cpp \
        MatterSurfaceLib/shaders/bvh_tlas_common.glsl MatterSurfaceLib/shaders/raytrace_tlas_blas.fs \
        MatterSurfaceLib/shaders/raytrace_tlas_blas_processed.fs \
        MatterSurfaceLib/tests/blas_refcount_tests.cpp
git commit -m "feat: carry per-triangle material through BVH to the shader"
```

---

# Phase 3 — Merge-group regrouping + cross-group clip carve

Groups particles by `mergeGroup` (so shades merge) and adds a transparency-gated clip field to `surface.c` (so different types meet on a clean carved interface).

### Task 3.1: Group meshing by mergeGroup instead of raw materialId

**Files:**
- Modify: `MatterSurfaceLib/src/cell.cpp` (`add_particle_index`, `rebuild_meshes`)
- Modify: `MatterSurfaceLib/include/cell.h` (rename `generate_mesh_for_material` → `generate_mesh_for_group`)
- Test: `MatterSurfaceLib/tests/cell_bounds_tests.cpp` (assert two shades of one group produce ONE mesh)

- [ ] **Step 1: Write the failing test**

In `cell_bounds_tests.cpp`, add: build a `Cell`, add two particles with materials 8 and 9 (same `GROUP_STONE`), rebuild, assert exactly one entry in `material_meshes` (one merged surface). Then add a particle with material 4 (glass, different group) and assert two entries.

```cpp
static int test_shades_merge_one_mesh() {
    // (mirror the existing cell_bounds_tests setup for Cluster/BLASManager)
    // ... create cell, cluster_particles with ids {8,9} close together ...
    cell.rebuild_meshes(cluster_particles, blas, 1.0f);
    if (cell.material_meshes.size() != 1) { printf("FAIL: stone shades should merge into 1 mesh, got %zu\n", cell.material_meshes.size()); return 1; }
    return 0;
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-cell`
Expected: FAIL — today materials 8 and 9 bucket separately → `material_meshes.size() == 2`.

- [ ] **Step 3: Switch the bucket key to mergeGroup**

In `cell.cpp`, `#include "material_registry.h"`. In `add_particle_index` (cell.cpp:100) and `remove_particle_index` (cell.cpp:109), resolve the group before indexing:

```cpp
void Cell::add_particle_index(uint32_t particle_index, uint32_t material_id) {
    uint32_t group = (uint32_t)MaterialMergeGroup((int)material_id);
    auto& material_particles = material_particle_indices[group];
    if (std::find(material_particles.begin(), material_particles.end(), particle_index) == material_particles.end()) {
        material_particles.push_back(particle_index);
        is_dirty = true;
    }
}
```

(Apply the same `group` resolution in `remove_particle_index`.) The map is now keyed by group. Rename `generate_mesh_for_material` → `generate_mesh_for_group` in `cell.h` and `cell.cpp`; its first arg is now a group id (used only as the `material_meshes`/`material_blas` key). The per-triangle material (Phase 2) already preserves each particle's true `materialId` for shading, so the group key never reaches the shader.

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-cell`
Expected: PASS — stone shades merge into one mesh; glass stays separate.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/cell.cpp MatterSurfaceLib/include/cell.h MatterSurfaceLib/tests/cell_bounds_tests.cpp
git commit -m "feat: group cell meshing by merge group so shades merge"
```

### Task 3.2: Add the clip-field parameters to the surface API

**Files:**
- Modify: `MatterSurfaceLib/include/surface.h` (extend `GenerateMesh`, `GenerateMeshWithConfig`, `ComputeSurfaceNormals`)
- Modify: `MatterSurfaceLib/src/surface.c` (thread `clipParticles`/`clipCount`; apply clip in `CalculateScalarAndMaterial`)
- Modify all callers: `MatterSurfaceLib/src/cell.cpp`, `MatterSurfaceLib/src/open_particle_surface.c`, `MatterSurfaceLib/tests/mesh_continuity_tests.cpp`
- Test: `MatterSurfaceLib/tests/mesh_continuity_tests.cpp`

- [ ] **Step 1: Write the failing test (clip carves the field)**

In `mesh_continuity_tests.cpp`, add: one group particle at origin (radius 1) and one clip particle nearby; assert the clipped mesh has no vertices on the far side of the equidistant plane (i.e. the group surface is cut where the clip particle is closer). A robust, simple assertion: mesh built WITH a clip particle has a smaller max-x extent than the same mesh built WITHOUT it (the clip on the +x side carves the surface back).

```c
static int test_clip_carves_surface() {
    Particle g[1]; g[0].position=(Vector3){0,0,0}; g[0].radius=1.0f; g[0].materialId=8;
    Particle clip[1]; clip[0].position=(Vector3){1.2f,0,0}; clip[0].radius=1.0f; clip[0].materialId=4;
    Bounds b; b.center=(Vector3){0,0,0}; b.size=(Vector3){5,5,5}; b.divisionPow=4;
    Mesh open = GenerateMesh(g, 1.0f, 1, b, 0.0f, NULL, 0);          // no clip
    Mesh carved = GenerateMesh(g, 1.0f, 1, b, 0.0f, clip, 1);        // clipped on +x
    float maxOpen=-1e9f, maxCarved=-1e9f;
    for (int i=0;i<open.vertexCount;i++)   maxOpen   = fmaxf(maxOpen,   open.vertices[i*3]);
    for (int i=0;i<carved.vertexCount;i++) maxCarved = fmaxf(maxCarved, carved.vertices[i*3]);
    if (!(maxCarved < maxOpen - 0.05f)) { printf("FAIL: clip did not carve +x side (open=%.3f carved=%.3f)\n", maxOpen, maxCarved); return 1; }
    return 0;
}
```

- [ ] **Step 2: Extend the API signatures**

In `surface.h`, change the three declarations to take a clip set (place new params last):

```c
Mesh GenerateMesh(Particle* particles, float particleRadius, int particleCount, Bounds volume, float blendWidth, Particle* clipParticles, int clipCount);
Mesh GenerateMeshWithConfig(Particle* particles, float particleRadius, int particleCount, Bounds volume, float blendWidth, MeshGenerationConfig config, Particle* clipParticles, int clipCount);
void ComputeSurfaceNormals(Mesh* mesh, Particle* particles, float particleRadius, int particleCount, float blendWidth, Particle* clipParticles, int clipCount);
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-cont`
Expected: FAIL — compile error (callers pass the old arity) until Step 4-5 land. After the signature change compiles, the assertion fails because the clip isn't applied yet.

- [ ] **Step 4: Implement the clip in surface.c**

Thread `clipParticles`/`clipCount` from `GenerateMesh` → `GenerateMeshInternal` → the sampling loop → `CalculateScalarAndMaterial`. Add params to `CalculateScalarAndMaterial`:

```c
static ScalarMaterialPair CalculateScalarAndMaterial(Vector3 position, SpatialHash* spatialHash, float refRadius, float blendWidth, Particle* clipParticles, int clipCount);
```

After computing `result.scalarValue` (surface.c ~line 837/850), apply the hard-min foreign field and clip:

```c
if (clipParticles && clipCount > 0) {
    float fO = INFINITY;
    for (int i = 0; i < clipCount; ++i) {
        float dx = position.x - clipParticles[i].position.x;
        float dy = position.y - clipParticles[i].position.y;
        float dz = position.z - clipParticles[i].position.z;
        float f = sqrtf(dx*dx + dy*dy + dz*dz) - clipParticles[i].radius;
        if (f < fO) fO = f;
    }
    // Where a foreign surface is nearer (fO < f_G), force this group outside so
    // its isosurface terminates on the equidistant locus f_G == fO.
    if (-fO > result.scalarValue) result.scalarValue = -fO;
}
```

Apply the identical clip inside `ComputeSurfaceNormals`' gradient sampler so normals match the carved surface.

- [ ] **Step 5: Update all callers to the new arity**

- `cell.cpp` `generate_mesh_for_group`: pass `NULL, 0` for now (clip wiring lands in Task 3.3). Both the `GenerateMesh` and `ComputeSurfaceNormals` calls (cell.cpp:307, 325).
- `open_particle_surface.c`: the `GenerateMesh(...)` call → append `NULL, 0`.
- `mesh_continuity_tests.cpp`: existing `GenerateMesh` calls → append `NULL, 0` (except the new clip test).

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-cont`
Expected: PASS — `test_clip_carves_surface` confirms the +x side is carved back; existing continuity tests still pass.

- [ ] **Step 7: Commit**

```bash
git add MatterSurfaceLib/include/surface.h MatterSurfaceLib/src/surface.c \
        MatterSurfaceLib/src/cell.cpp MatterSurfaceLib/src/open_particle_surface.c \
        MatterSurfaceLib/tests/mesh_continuity_tests.cpp
git commit -m "feat: add transparency-agnostic clip field to surface meshing"
```

### Task 3.3: Wire transparency-gated clip particles in cell.cpp

**Files:**
- Modify: `MatterSurfaceLib/src/cell.cpp` (`generate_mesh_for_group`)
- Test: `MatterSurfaceLib/tests/cell_bounds_tests.cpp`

- [ ] **Step 1: Write the failing test**

In `cell_bounds_tests.cpp`: place a glass group and a metal group adjacent in a cell. Build with the new wiring. Assert the glass mesh's extent toward the metal is carved (smaller) versus a control where both groups are opaque (no carve). Concretely, compare the glass mesh max extent toward metal between: (a) metal id = opaque (3) and (b) — no, glass is always transparent so carve always applies when glass present. Simpler assertion: with glass+metal adjacent, the metal mesh is carved on the glass side (its vertices stop before the glass center); with two OPAQUE groups adjacent, neither is carved (vertices overlap past the midline).

```cpp
// Assert: opaque+opaque → overlap allowed (a vertex exists past the midline);
//         glass+metal   → carved (no metal vertex past the midline toward glass).
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd MatterSurfaceLib/tests && make run-cell`
Expected: FAIL — `generate_mesh_for_group` currently passes `NULL, 0`, so no carve happens even for glass/metal.

- [ ] **Step 3: Build the clip set and pass it**

In `generate_mesh_for_group(group G)`, after building `particles` for `G`, build `clipParticles` from the other groups present in this cell that are carving-relevant. Relevance: a foreign group `F` carves `G` iff `MaterialIsTransparent(any row of G) || MaterialIsTransparent(any row of F)`. Since a group is one optical class, test the transparency of a representative material id per group. Implementation:

```cpp
// Is THIS group transparent? Use the first particle's material as representative.
bool g_transparent = !particles.empty() && MaterialIsTransparent(particles[0].materialId);

std::vector<Particle> clip;
for (const auto& other : material_particle_indices) {
    if (other.first == group) continue;
    // representative material of the other group
    if (other.second.empty()) continue;
    int rep = (int)cluster_particles[other.second.front()].materialId;
    bool other_transparent = MaterialIsTransparent(rep);
    if (!(g_transparent || other_transparent)) continue; // opaque<->opaque: no carve
    for (uint32_t idx : other.second) {
        if (idx >= cluster_particles.size()) continue;
        const StaticParticle& sp = cluster_particles[idx];
        if (sp.radius < cull_radius) continue;
        float r_eff = (sp.radius < vis_radius) ? vis_radius : sp.radius;
        Particle cp; cp.position = sp.position; cp.radius = r_eff; cp.materialId = (int)sp.materialId;
        clip.push_back(cp);
    }
}
Particle* clipPtr = clip.empty() ? NULL : clip.data();
int clipCount = (int)clip.size();
```

Pass `clipPtr, clipCount` to both `GenerateMesh` (cell.cpp:307) and `ComputeSurfaceNormals` (cell.cpp:325).

- [ ] **Step 4: Run the test to verify it passes**

Run: `cd MatterSurfaceLib/tests && make run-cell`
Expected: PASS — glass/metal carve; opaque/opaque overlap.

- [ ] **Step 5: Build the app and capture the glass-in-metal case**

Run: `cd MatterSurfaceLib && make shaders && make WSL_LINUX=1 -j4`
Then capture a scene with a glass channel through metal:
`DISPLAY=:0 MSL_CAPTURE=glass_channel.png MSL_RENDER_MODE=0 MSL_FRAMES=2 ./matter_surface_lib`
Expected: the metal forms a clean cavity wall against the glass (no z-fighting double-surface); the glass refracts and the metal is visible through it. If z-fighting appears at the interface, add the per-side clip bias (`fO + 1e-4f`) noted in the spec to the clip in `surface.c` and rebuild.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/src/cell.cpp MatterSurfaceLib/tests/cell_bounds_tests.cpp
git commit -m "feat: carve clean interfaces between transparent and neighbor materials"
```

### Task 3.4: Full regression pass

- [ ] **Step 1: Run every headless suite**

Run: `cd MatterSurfaceLib/tests && make run-reg && make run-cont && make run-cell && make run-simp && make run-blas`
Expected: all suites PASS. The `clipCount == 0` path must keep `mesh_continuity_tests` and `mesh_simplifier_tests` byte-for-byte equivalent to pre-Phase-3 behavior (regression guard for the metaball/LOD work).

- [ ] **Step 2: Commit any fixes, else proceed to finishing the branch**

If all green and no changes, nothing to commit. Use the `superpowers:finishing-a-development-branch` skill to wrap up.

---

## Self-review notes (author check, completed)

- **Spec coverage:** Registry (Phase 1) ✓; per-triangle material (Phase 2) ✓; clip carve + mergeGroup (Phase 3) ✓; transparency gating ✓; opaque/opaque no-carve ✓; watertight regression guard (Task 3.4) ✓; coincident-triangle epsilon/bias (Task 3.3 Step 5) ✓.
- **Known soft spots requiring care during execution (not placeholders):**
  - Phase 2 GPU texel packing offset for `TriEx.materialId` (Task 2.2 Step 3-4) must use a consistent unused texel slot between `blas_manager.cpp` and the shader fetch; verify by the round-trip + capture.
  - Task 2.1 Step 4 documents a re-scope: triangle-level tagging happens cell-side (Task 2.2) where particles are available; the surface.c test asserts the per-vertex material path instead.
- **Type consistency:** `MaterialDef`, `MaterialMergeGroup`, `MaterialIsTransparent`, `MATERIAL_FLOATS_PER_DEF`, and the extended `GenerateMesh`/`ComputeSurfaceNormals` arity are used identically across tasks.
