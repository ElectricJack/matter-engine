# Raster MVP (Phase 1 of Raster Switch) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make forward instanced rasterization the MatterEngine3 viewer's default renderer, drawing the existing flat-part LOD ladders via raylib `DrawMeshInstanced` with flat ambient + unshadowed sun lighting; the ray tracer stays available behind `MATTER_RT=1`.

**Architecture:** A GL-free `raster_mesh` module converts BLAS triangle data (Tri + TriEx) into vertex arrays using standard raylib Mesh channels (normals ← shading normals, colors ← tint, texcoords ← (materialId, vertexAO)). PartStore stores those CPU arrays per LOD level. A `RasterComposer` groups resolved instances into (part, level) batches (GL-free, unit-tested), lazily uploads Meshes, and issues one `DrawMeshInstanced` per batch inside `BeginMode3D` with a new forward shader.

**Tech Stack:** C++17, raylib/rlgl OpenGL 3.3, GLSL 330. Spec: `docs/superpowers/specs/2026-07-02-raster-switch-design.md` (this plan = Phase 1 "Raster MVP" only; probe lighting, clusters, ground are later plans).

## Global Constraints

- MatterSurfaceLib source is read-only; ADDING new shader files under `MatterSurfaceLib/shaders/` is allowed (additive; the viewer's `shaders/` dir is copied/symlinked from there by the Makefile).
- All engine/logic code must be GL-free and unit-testable; GL calls only in viewer draw-path files, never invoked by tests (tests run without `InitWindow`).
- Tests use the existing bare-`CHECK` harness pattern of `MatterEngine3/tests/viewer_logic_tests.cpp`.
- After any engine/viewer change that lands: rebuild Linux viewer AND `make windows` (stale `viewer.exe` silently ships old engine). Header changes → clean the Windows objs first (`rm -rf build/windows`).
- Frustum culling is explicitly OUT of this phase (resolver already distance-culls; per-cluster culling arrives in Phase 3).
- Commit after every task; never commit unrelated staged files (`git add` specific paths only).

## Key existing interfaces (verbatim from the codebase)

```cpp
// MatterSurfaceLib/include/bvh.h:15
struct Tri { float3 vertex0, vertex1, vertex2, centroid; };            // 64B aligned, unions with __m128
// bvh.h:27 — tint.w = blend strength vs material albedo; (1,1,1,0) = neutral
struct TriEx { float2 uv0,uv1,uv2; float3 N0,N1,N2; int materialId; float4 tint;
               float ao0 = 1.0f, ao1 = 1.0f, ao2 = 1.0f; };

// MatterSurfaceLib/include/blas_manager.hpp:54,142
const BLASManager::BLASEntry* BLASManager::get_entry(BLASHandle) const;
//   entry->triangles : std::vector<Tri>;  entry->tri_extra : std::vector<TriEx> (may be empty)

// MatterEngine3/viewer/part_store.h:19
struct LoadedPart { std::vector<BLASHandle> lod_blas; float bound_radius;
                    std::vector<float> thresholds;
                    std::vector<part_asset::ChildInstance> children; };
// part_asset::ChildInstance has: uint64_t child_resolved_hash; float transform[16] (row-major)

// MatterEngine3/viewer/sector_resolver.h:14
struct ResolvedInstance { uint64_t part_hash; int lod_level; float transform[16]; }; // row-major

// MatterSurfaceLib/include/material_registry.h:41
#define MATERIAL_FLOATS_PER_DEF 12   // [0..2] albedo, [3] rough, [4] metal, [5] emission
void MaterialRegistryPackForGPU(float* out);  int MaterialRegistryCount();

// raylib: DrawMeshInstanced(Mesh, Material, const Matrix*, int) — instance mat4 arrives in
// vertex attribute "instanceTransform" (rlgl.h:1006, bound to location 9). LoadShader
// auto-resolves shader.locs[SHADER_LOC_VERTEX_INSTANCE_TX] from that attribute name.
// raylib Matrix field DECLARATION order is m0,m4,m8,m12,m1,... == row-major memory, so a
// row-major float[16] memcpy's directly into Matrix (translation t[3],t[7],t[11] → m12,m13,m14).
```

---

### Task 1: GL-free raster mesh data builder

**Files:**
- Create: `MatterEngine3/viewer/raster_mesh.h`, `MatterEngine3/viewer/raster_mesh.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp` (append tests), `MatterEngine3/tests/Makefile` (add `../viewer/raster_mesh.cpp` to the viewer-logic target's C++ source list — the variable that already contains `../viewer/part_store.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  namespace viewer {
  struct RasterMeshData {                       // CPU-only; uploaded later by RasterComposer
      std::vector<float>         vertices;      // 3 floats/vert, 3 verts/tri (non-indexed)
      std::vector<float>         normals;       // 3 floats/vert (TriEx N0/N1/N2, else geometric)
      std::vector<unsigned char> colors;        // RGBA8/vert from TriEx tint ((255,255,255,0) when absent)
      std::vector<float>         texcoords;     // 2 floats/vert: (materialId, vertexAO); (-1,1) when absent
      int vertex_count = 0;
  };
  RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count);
  Matrix row_major_to_matrix(const float t[16]);
  }
  ```

- [ ] **Step 1: Write the failing tests** — append to `viewer_logic_tests.cpp` (include `"raster_mesh.h"` at top):

```cpp
static bool test_raster_mesh_data() {
    Tri t[2] = {};
    t[0].vertex0 = make_float3(0,0,0); t[0].vertex1 = make_float3(1,0,0); t[0].vertex2 = make_float3(0,1,0);
    t[1].vertex0 = make_float3(0,0,1); t[1].vertex1 = make_float3(1,0,1); t[1].vertex2 = make_float3(0,1,1);
    TriEx ex[2] = {};
    for (int i = 0; i < 2; ++i) {
        ex[i].N0 = ex[i].N1 = ex[i].N2 = make_float3(0,0,1);
        ex[i].materialId = 7; ex[i].tint = make_float4(0.5f, 0.25f, 1.0f, 1.0f);
        ex[i].ao0 = 0.5f; ex[i].ao1 = 1.0f; ex[i].ao2 = 0.0f;
    }
    auto d = viewer::build_raster_mesh_data(t, ex, 2);
    CHECK(d.vertex_count == 6, "6 verts from 2 tris");
    CHECK(d.vertices.size() == 18 && d.normals.size() == 18, "array sizes");
    CHECK(d.colors.size() == 24 && d.texcoords.size() == 12, "color/uv sizes");
    CHECK(d.vertices[3] == 1.0f && d.vertices[4] == 0.0f, "v1 position");
    CHECK(d.normals[2] == 1.0f, "N0.z passthrough");
    CHECK(d.colors[0] == 127 || d.colors[0] == 128, "tint r quantized");
    CHECK(d.colors[3] == 255, "tint alpha");
    CHECK(d.texcoords[0] == 7.0f, "materialId in u");
    CHECK(d.texcoords[1] == 0.5f && d.texcoords[5] == 0.0f, "per-vertex AO in v");

    auto plain = viewer::build_raster_mesh_data(t, nullptr, 2);   // no TriEx: geometric fallback
    CHECK(plain.vertex_count == 6, "plain verts");
    CHECK(plain.normals[2] == 1.0f, "geometric normal +z");
    CHECK(plain.texcoords[0] == -1.0f && plain.texcoords[1] == 1.0f, "sentinel mat, AO=1");
    CHECK(plain.colors[3] == 0, "neutral tint alpha 0");

    float rm[16] = {1,0,0, 5,  0,1,0, 6,  0,0,1, 7,  0,0,0,1};    // row-major translate(5,6,7)
    Matrix m = viewer::row_major_to_matrix(rm);
    CHECK(m.m12 == 5.0f && m.m13 == 6.0f && m.m14 == 7.0f, "translation lands in m12..m14");
    return true;
}
```
Register `RUN(test_raster_mesh_data);` alongside the existing test invocations (match the file's existing runner pattern).

- [ ] **Step 2: Run to verify failure**

Run: `cd "MatterEngine3/tests" && make run-viewer-logic` (use the file's existing target name for viewer_logic_tests)
Expected: compile FAILURE — `raster_mesh.h: No such file or directory`

- [ ] **Step 3: Implement**

`raster_mesh.h`:
```cpp
#ifndef VIEWER_RASTER_MESH_H
#define VIEWER_RASTER_MESH_H
#include "raylib.h"
#include "bvh.h"           // Tri, TriEx (MatterSurfaceLib)
#include <vector>

namespace viewer {

// CPU-side vertex arrays for one LOD level, raylib-Mesh channel layout.
// TriEx maps onto standard channels: normals <- N0/N1/N2, colors <- tint RGBA,
// texcoords <- (materialId, per-vertex AO). GL-free: upload happens in RasterComposer.
struct RasterMeshData {
    std::vector<float>         vertices;
    std::vector<float>         normals;
    std::vector<unsigned char> colors;
    std::vector<float>         texcoords;
    int vertex_count = 0;
};

RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count);

// Row-major float[16] -> raylib Matrix. raylib's field declaration order
// (m0,m4,m8,m12,m1,...) IS row-major memory, so this is a straight copy.
Matrix row_major_to_matrix(const float t[16]);

} // namespace viewer
#endif
```

`raster_mesh.cpp`:
```cpp
#include "raster_mesh.h"
#include <cstring>

namespace viewer {

static unsigned char to_u8(float v) {
    float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return (unsigned char)(c * 255.0f + 0.5f);
}

RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count) {
    RasterMeshData d;
    d.vertex_count = tri_count * 3;
    d.vertices.reserve(d.vertex_count * 3);  d.normals.reserve(d.vertex_count * 3);
    d.colors.reserve(d.vertex_count * 4);    d.texcoords.reserve(d.vertex_count * 2);

    for (int i = 0; i < tri_count; ++i) {
        const Tri& t = tris[i];
        const float3 vs[3] = { t.vertex0, t.vertex1, t.vertex2 };

        float3 ns[3];
        if (triex) { ns[0] = triex[i].N0; ns[1] = triex[i].N1; ns[2] = triex[i].N2; }
        else {
            float3 gn = normalize(cross(t.vertex1 - t.vertex0, t.vertex2 - t.vertex0));
            ns[0] = ns[1] = ns[2] = gn;
        }
        const float aos[3] = { triex ? triex[i].ao0 : 1.0f,
                               triex ? triex[i].ao1 : 1.0f,
                               triex ? triex[i].ao2 : 1.0f };
        const float mat_id = triex ? (float)triex[i].materialId : -1.0f;
        unsigned char rgba[4] = { 255, 255, 255, 0 };            // neutral tint
        if (triex) {
            rgba[0] = to_u8(triex[i].tint.x); rgba[1] = to_u8(triex[i].tint.y);
            rgba[2] = to_u8(triex[i].tint.z); rgba[3] = to_u8(triex[i].tint.w);
        }
        for (int v = 0; v < 3; ++v) {
            d.vertices.push_back(vs[v].x); d.vertices.push_back(vs[v].y); d.vertices.push_back(vs[v].z);
            d.normals.push_back(ns[v].x);  d.normals.push_back(ns[v].y);  d.normals.push_back(ns[v].z);
            d.colors.push_back(rgba[0]); d.colors.push_back(rgba[1]);
            d.colors.push_back(rgba[2]); d.colors.push_back(rgba[3]);
            d.texcoords.push_back(mat_id); d.texcoords.push_back(aos[v]);
        }
    }
    return d;
}

Matrix row_major_to_matrix(const float t[16]) {
    Matrix m;
    std::memcpy(&m, t, sizeof(Matrix));
    return m;
}

} // namespace viewer
```

Makefile: add `../viewer/raster_mesh.cpp` to the viewer-logic target's source list in `MatterEngine3/tests/Makefile`.

- [ ] **Step 4: Run to verify pass**

Run: `cd "MatterEngine3/tests" && make run-viewer-logic`
Expected: all existing tests still OK + new test passes.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/viewer/raster_mesh.h MatterEngine3/viewer/raster_mesh.cpp \
        MatterEngine3/tests/viewer_logic_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat: GL-free raster mesh data builder (TriEx -> raylib Mesh channels)"
```

---

### Task 2: PartStore keeps CPU raster arrays per LOD level

**Files:**
- Modify: `MatterEngine3/viewer/part_store.h` (LoadedPart), `MatterEngine3/viewer/part_store.cpp` (both `load_flat` and the compositional fallback path)
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Consumes: `build_raster_mesh_data` (Task 1); `blas_.get_entry(handle)->triangles / ->tri_extra`
- Produces: `LoadedPart::lod_mesh_data` — `std::vector<RasterMeshData>`, parallel to `lod_blas` (same size, same level order). Task 4 relies on this exact member name.

- [ ] **Step 1: Write the failing test** — extend the existing flat-tree test in `viewer_logic_tests.cpp` (the one asserting the flat tree loads with empty children):

```cpp
CHECK(tree->lod_mesh_data.size() == tree->lod_blas.size(), "raster data per LOD level");
CHECK(tree->lod_mesh_data[0].vertex_count > 0, "LOD0 raster verts present");
```
Also extend the synthetic compositional fixture test (quad parent/child) with the same two CHECKs on the parent part.

- [ ] **Step 2: Run to verify failure** — `make run-viewer-logic` → compile FAIL: `no member named 'lod_mesh_data'`.

- [ ] **Step 3: Implement**

`part_store.h`: add `#include "raster_mesh.h"` and inside `LoadedPart`:
```cpp
    std::vector<RasterMeshData> lod_mesh_data;  // parallel to lod_blas (CPU-only; GL upload is lazy)
```
`part_store.cpp`: in BOTH load paths, immediately after each level's BLAS handle is registered/pushed into `lp.lod_blas`, append:
```cpp
    if (const auto* e = blas_.get_entry(lp.lod_blas.back())) {
        const TriEx* ex = (e->tri_extra.size() == e->triangles.size() && !e->tri_extra.empty())
                              ? e->tri_extra.data() : nullptr;
        lp.lod_mesh_data.push_back(
            build_raster_mesh_data(e->triangles.data(), ex, (int)e->triangles.size()));
    } else {
        lp.lod_mesh_data.push_back({});
    }
```
(If a path registers all levels in a loop, put this inside that loop — one entry per level, order preserved.)

- [ ] **Step 4: Run to verify pass** — `make run-viewer-logic` → OK. Also run the other suites touched by part_store linkage if any fail to compile: `make` in `MatterEngine3/tests` for the composition target.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/viewer/part_store.h MatterEngine3/viewer/part_store.cpp \
        MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat: PartStore carries CPU raster vertex arrays per LOD level"
```

---

### Task 3: Forward raster shaders

**Files:**
- Create: `MatterSurfaceLib/shaders/raster.vs`, `MatterSurfaceLib/shaders/raster.fs` (additive to MSL — the viewer `shaders/` dir is a symlink/copy of this dir; new files ride along automatically, including into the Windows `win-shaders` copy)

**Interfaces:**
- Produces: shader pair loaded by Task 4 via `LoadShader("shaders/raster.vs", "shaders/raster.fs")`. Uniforms Task 4 sets: `materialTable` (float[768]), `materialCount` (int), `sunDir` (vec3, normalized, pointing FROM sun), `sunColor` (vec3), `ambientColor` (vec3). Vertex attributes are raylib defaults + `instanceTransform`.

- [ ] **Step 1: Write `raster.vs`**

```glsl
#version 330
// Forward instanced vertex shader. raylib binds: vertexPosition/vertexNormal/
// vertexColor/vertexTexCoord + per-instance mat4 'instanceTransform' (divisor 1),
// and uploads 'mvp' = view*projection (model is identity for instanced draws).
in vec3 vertexPosition;
in vec2 vertexTexCoord;    // x = materialId (-1 sentinel), y = baked vertex AO
in vec3 vertexNormal;
in vec4 vertexColor;       // TriEx tint; a = blend strength vs material albedo
in mat4 instanceTransform;

uniform mat4 mvp;

out vec3 fragNormal;       // world space
out vec4 fragTint;
out vec2 fragMatAO;

void main() {
    mat4 model = instanceTransform;
    vec4 world = model * vec4(vertexPosition, 1.0);
    // inverse-transpose-free normal: assumes rigid+uniform-scale placements (true for
    // current worlds); revisit if non-uniform instance scales appear.
    fragNormal = normalize(mat3(model) * vertexNormal);
    fragTint   = vertexColor;
    fragMatAO  = vertexTexCoord;
    gl_Position = mvp * world;
}
```

- [ ] **Step 2: Write `raster.fs`**

```glsl
#version 330
// Phase-1 lighting: flat sky ambient x baked vertex AO + unshadowed sun N.L,
// material albedo from the shared 64x12 material table, TriEx tint blend,
// emission add. Probe-volume lighting replaces ambient/sun terms in Phase 2.
in vec3 fragNormal;
in vec4 fragTint;
in vec2 fragMatAO;

uniform float materialTable[64 * 12];   // [0..2] albedo, [3] rough, [4] metal, [5] emission
uniform int   materialCount;
uniform vec3  sunDir;                   // normalized, points FROM the sun toward the scene
uniform vec3  sunColor;
uniform vec3  ambientColor;

out vec4 finalColor;

void main() {
    int mid = int(max(fragMatAO.x, 0.0) + 0.5);
    if (mid >= materialCount) mid = 0;
    int b = mid * 12;
    vec3  albedo   = vec3(materialTable[b], materialTable[b+1], materialTable[b+2]);
    float emission = materialTable[b+5];

    albedo = mix(albedo, fragTint.rgb, fragTint.a);

    vec3  N   = normalize(fragNormal);
    float ndl = max(dot(N, -sunDir), 0.0);
    float ao  = clamp(fragMatAO.y, 0.0, 1.0);

    vec3 color = albedo * (ambientColor * ao + sunColor * ndl) + albedo * emission;
    color = color / (color + vec3(1.0));          // Reinhard
    color = pow(color, vec3(1.0 / 2.2));          // gamma
    finalColor = vec4(color, 1.0);
}
```

- [ ] **Step 3: Sanity-check the shader pair compiles** — deferred to Task 4's smoke run (`LoadShader` logs GLSL errors to stdout; a failed load returns id 0 and Task 4's init() errors out). No standalone step possible without a GL context.

- [ ] **Step 4: Commit**

```bash
git add MatterSurfaceLib/shaders/raster.vs MatterSurfaceLib/shaders/raster.fs
git commit -m "feat: forward instanced raster shader pair (phase-1 lighting)"
```

---

### Task 4: RasterComposer — batches (GL-free) + instanced draw (GL)

**Files:**
- Create: `MatterEngine3/viewer/raster_composer.h`, `MatterEngine3/viewer/raster_composer.cpp`
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp`, `MatterEngine3/tests/Makefile` (add `../viewer/raster_composer.cpp` next to `raster_mesh.cpp`)

**Interfaces:**
- Consumes: `ResolvedInstance`, `PartStore::get_or_load`, `LoadedPart::lod_mesh_data/children`, `row_major_to_matrix`, `MaterialRegistryPackForGPU/Count`
- Produces:
  ```cpp
  namespace viewer {
  struct RasterBatch { uint64_t part_hash; int level; std::vector<Matrix> transforms; };
  class RasterComposer {
  public:
      ~RasterComposer();                                   // unloads cached Meshes + shader
      bool init(std::string& err);                         // GL: LoadShader("shaders/raster.vs", ...)
      // GL-free, static: recursive child expansion (depth<=8, 200k cap), level clamped
      // to lod_mesh_data.size()-1, geometry-less assemblies recurse without emitting.
      static std::vector<RasterBatch> build_batches(
          const std::vector<ResolvedInstance>& resolved, PartStore& store);
      // GL: lazy-upload meshes, BeginMode3D, one DrawMeshInstanced per batch. Returns drawn tris.
      int draw(const std::vector<RasterBatch>& batches, PartStore& store, const Camera3D& cam);
  private:
      Mesh* ensure_mesh(uint64_t hash, int level, PartStore& store);   // upload-once cache
      Shader   shader_{};
      Material material_{};
      std::map<uint64_t, Mesh> mesh_cache_;                // key = (hash<<4)|level  (levels < 16)
      bool ready_ = false;
      int  loc_sun_dir_ = -1, loc_sun_color_ = -1, loc_ambient_ = -1,
           loc_mat_table_ = -1, loc_mat_count_ = -1;
  };
  }
  ```

- [ ] **Step 1: Write the failing test** — in the synthetic parent/child quad fixture of `viewer_logic_tests.cpp` (parent `0xBBBB0000BBBB0001` places 2 translated children `0xAAAA0000AAAA0001`), append:

```cpp
    viewer::ResolvedInstance r{};
    r.part_hash = parent_hash; r.lod_level = 0;
    float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(r.transform, ident, sizeof ident);
    auto batches = viewer::RasterComposer::build_batches({r}, store);
    CHECK(batches.size() == 2, "two (hash,level) batches");            // parent + child groups
    size_t total = 0; size_t child_batch_n = 0;
    for (const auto& b : batches) {
        total += b.transforms.size();
        if (b.part_hash == child_hash) child_batch_n = b.transforms.size();
    }
    CHECK(total == 3, "3 instances total");
    CHECK(child_batch_n == 2, "children grouped into one batch");
    // second child sits at x=+20 (fixture translation): translation is in m12
    bool found20 = false;
    for (const auto& b : batches)
        if (b.part_hash == child_hash)
            for (const auto& m : b.transforms) if (m.m12 == 20.0f) found20 = true;
    CHECK(found20, "child world transform applied");
```

- [ ] **Step 2: Run to verify failure** — `make run-viewer-logic` → compile FAIL: `raster_composer.h` missing.

- [ ] **Step 3: Implement** — `raster_composer.cpp` core (header per the interface block above):

```cpp
#include "raster_composer.h"
#include "material_registry.h"
#include "rlgl.h"
#include <cstring>
#include <functional>

namespace viewer {

static void mul16(const float* a, const float* b, float* out) {   // row-major, as world_composer.cpp:9
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j];
            out[i*4+j] = s;
        }
}

std::vector<RasterBatch> RasterComposer::build_batches(
        const std::vector<ResolvedInstance>& resolved, PartStore& store) {
    const int kMaxDepth = 8; const size_t kMaxInstances = 200000;
    std::map<uint64_t, RasterBatch> acc;      // key = (hash<<4)|level
    size_t emitted = 0;

    std::function<void(uint64_t, const float*, int, int)> emit =
        [&](uint64_t hash, const float* world, int lod, int depth) {
            if (depth > kMaxDepth || emitted >= kMaxInstances) return;
            const LoadedPart* lp = store.get_or_load(hash);
            if (!lp) return;
            if (!lp->lod_mesh_data.empty()) {
                int lv = lod < 0 ? 0 : lod;
                if (lv >= (int)lp->lod_mesh_data.size()) lv = (int)lp->lod_mesh_data.size() - 1;
                uint64_t key = (hash << 4) | (uint64_t)lv;
                auto& b = acc[key];
                b.part_hash = hash; b.level = lv;
                b.transforms.push_back(row_major_to_matrix(world));
                ++emitted;
            }
            for (const auto& c : lp->children) {
                float cw[16]; mul16(world, c.transform, cw);
                emit(c.child_resolved_hash, cw, 0, depth + 1);
            }
        };
    for (const auto& r : resolved) emit(r.part_hash, r.transform, r.lod_level, 0);

    std::vector<RasterBatch> out;
    out.reserve(acc.size());
    for (auto& kv : acc) out.push_back(std::move(kv.second));
    return out;
}

bool RasterComposer::init(std::string& err) {
    shader_ = LoadShader("shaders/raster.vs", "shaders/raster.fs");
    if (shader_.id == 0) { err = "raster shader failed to load"; return false; }
    if (shader_.locs[SHADER_LOC_VERTEX_INSTANCE_TX] == -1)      // defensive; raylib auto-resolves
        shader_.locs[SHADER_LOC_VERTEX_INSTANCE_TX] = GetShaderLocationAttrib(shader_, "instanceTransform");
    loc_sun_dir_   = GetShaderLocation(shader_, "sunDir");
    loc_sun_color_ = GetShaderLocation(shader_, "sunColor");
    loc_ambient_   = GetShaderLocation(shader_, "ambientColor");
    loc_mat_table_ = GetShaderLocation(shader_, "materialTable");
    loc_mat_count_ = GetShaderLocation(shader_, "materialCount");
    material_ = LoadMaterialDefault();
    material_.shader = shader_;
    ready_ = true;
    return true;
}

Mesh* RasterComposer::ensure_mesh(uint64_t hash, int level, PartStore& store) {
    uint64_t key = (hash << 4) | (uint64_t)level;
    auto it = mesh_cache_.find(key);
    if (it != mesh_cache_.end()) return &it->second;
    const LoadedPart* lp = store.get_or_load(hash);
    if (!lp || level >= (int)lp->lod_mesh_data.size()) return nullptr;
    const RasterMeshData& d = lp->lod_mesh_data[level];
    if (d.vertex_count == 0) return nullptr;

    Mesh m{};                                  // non-indexed; raylib copies nothing on UploadMesh
    m.vertexCount   = d.vertex_count;
    m.triangleCount = d.vertex_count / 3;
    m.vertices  = const_cast<float*>(d.vertices.data());
    m.normals   = const_cast<float*>(d.normals.data());
    m.colors    = const_cast<unsigned char*>(d.colors.data());
    m.texcoords = const_cast<float*>(d.texcoords.data());
    UploadMesh(&m, false);
    // detach CPU pointers: PartStore owns them; UnloadMesh must not free them
    m.vertices = nullptr; m.normals = nullptr; m.colors = nullptr; m.texcoords = nullptr;
    return &(mesh_cache_[key] = m);
}

int RasterComposer::draw(const std::vector<RasterBatch>& batches, PartStore& store,
                         const Camera3D& cam) {
    if (!ready_) return 0;
    float table[64 * MATERIAL_FLOATS_PER_DEF] = {0};
    MaterialRegistryPackForGPU(table);
    int count = MaterialRegistryCount();
    SetShaderValueV(shader_, loc_mat_table_, table, SHADER_UNIFORM_FLOAT, count * MATERIAL_FLOATS_PER_DEF);
    SetShaderValue(shader_, loc_mat_count_, &count, SHADER_UNIFORM_INT);
    Vector3 sun_dir = Vector3Normalize((Vector3){ -0.45f, -0.80f, -0.35f });   // phase-1 fixed sun
    Vector3 sun_col = (Vector3){ 2.2f, 2.05f, 1.8f };
    Vector3 ambient = (Vector3){ 0.38f, 0.43f, 0.52f };
    SetShaderValue(shader_, loc_sun_dir_,   &sun_dir, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_sun_color_, &sun_col, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_ambient_,   &ambient, SHADER_UNIFORM_VEC3);

    int tris = 0;
    BeginMode3D(cam);
    rlDisableBackfaceCulling();   // mesh-session winding is not guaranteed consistent
    for (const auto& b : batches) {
        Mesh* m = ensure_mesh(b.part_hash, b.level, store);
        if (!m || b.transforms.empty()) continue;
        DrawMeshInstanced(*m, material_, b.transforms.data(), (int)b.transforms.size());
        tris += m->triangleCount * (int)b.transforms.size();
    }
    rlEnableBackfaceCulling();
    EndMode3D();
    return tris;
}

RasterComposer::~RasterComposer() {
    for (auto& kv : mesh_cache_) UnloadMesh(kv.second);
    if (ready_) UnloadShader(shader_);   // material_ holds the same shader; don't double-free via UnloadMaterial
}

} // namespace viewer
```
Include `raymath.h` (for `Vector3Normalize`) or normalize inline. Add `../viewer/raster_composer.cpp` to the tests Makefile viewer-logic source list.

- [ ] **Step 4: Run to verify pass** — `make run-viewer-logic` → OK (only `build_batches` executes in tests; no GL).

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/viewer/raster_composer.h MatterEngine3/viewer/raster_composer.cpp \
        MatterEngine3/tests/viewer_logic_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat: RasterComposer with GL-free batching + instanced draw"
```

---

### Task 5: Viewer integration — raster by default, MATTER_RT fallback

**Files:**
- Modify: `MatterEngine3/viewer/main.cpp`, `MatterEngine3/viewer/ui.h` (ViewerStats + debug panel), `MatterEngine3/viewer/Makefile` (add `raster_mesh.cpp raster_composer.cpp` to `VIEWER_SRC`, line 29)

**Interfaces:**
- Consumes: `RasterComposer::init/build_batches/draw`, existing `Renderer` (camera + traced path), resolver/composer wiring in `main.cpp`.

- [ ] **Step 1: Wire main.cpp**

```cpp
// near the top of main(), after Renderer init:
const bool use_rt = getenv("MATTER_RT") != nullptr;
```
1. Add `#include "raster_composer.h"`; declare `std::unique_ptr<RasterComposer> raster;` next to `composer`; in `connect_sequence` after `composer = ...`: `raster = std::make_unique<RasterComposer>(); if (!use_rt) { std::string rerr; if (!raster->init(rerr)) { printf("raster: %s\n", rerr.c_str()); return false; } }` (init AFTER InitWindow — already guaranteed; RasterComposer is recreated on reload, dropping stale mesh caches).
2. Gate the warm-up block (`composer->compose(...); renderer.warm_up(...)`) behind `if (use_rt)` — raster mode must NOT pay the ~60s compile.
3. In the frame loop, replace the unconditional compose+draw with:
```cpp
        int active = 0;
        std::vector<RasterBatch> batches;
        if (use_rt) {
            active = composer->compose(state, resolver, lods, cam);
        } else {
            auto resolved = resolver.resolve(state, lods, cam);
            batches = RasterComposer::build_batches(resolved, store ? *store : /*unreachable*/ *store);
            for (const auto& b : batches) active += (int)b.transforms.size();
        }
        ...
        BeginDrawing();
            ClearBackground((Color){ 96, 118, 143, 255 });   // placeholder sky; probe phase replaces
            if (use_rt) renderer.draw(store->blas(), composer->tlas());
            else {
                stats.raster_tris    = raster->draw(batches, *store, renderer.camera());
                stats.raster_batches = (int)batches.size();
            }
            ui.begin_frame(); ...
```
(`resolver.resolve` is already virtual-dispatched the same way `compose` obtains it — reuse the existing `SectorResolver& resolver` selection.)

- [ ] **Step 2: HUD stats** — `ui.h`: add to `ViewerStats`: `int raster_batches = 0; int raster_tris = 0;`. In the debug-panel drawing code add: `ImGui::Text("Raster: %d batches / %d tris", stats.raster_batches, stats.raster_tris);`

- [ ] **Step 3: Build + smoke** —
Run: `cd "MatterEngine3/viewer" && make 2>&1 | tail -3`
Expected: clean link.
Run headless smoke: `MATTER_SCREENSHOT=raster_smoke.png ./viewer 2>&1 | grep -E "raster|FLAT|screenshot"`
Expected: NO warm-up line, `screenshot written` within seconds, no `raster shader failed`. View `raster_smoke.png`: tree visible, lit, on the placeholder sky — silhouette matches the traced reference from memory.
Then RT fallback still works: `MATTER_RT=1 MATTER_SCREENSHOT=rt_check.png ./viewer` → traced image as before (warm-up runs).

- [ ] **Step 4: Commit**

```bash
git add MatterEngine3/viewer/main.cpp MatterEngine3/viewer/ui.h MatterEngine3/viewer/Makefile
git commit -m "feat: rasterization is the viewer default; MATTER_RT=1 keeps traced path"
```

---

### Task 6: Verification, Windows build, docs

**Files:**
- Modify: `MatterEngine3/docs/rendering.md`
- No code changes expected (fix-forward if verification fails).

- [ ] **Step 1: A/B screenshots** — same camera both paths:
```bash
cd "MatterEngine3/viewer"
MATTER_CAM="20,16,34,0,9,0" MATTER_SCREENSHOT=ab_raster.png ./viewer
MATTER_RT=1 MATTER_CAM="20,16,34,0,9,0" MATTER_SCREENSHOT=ab_rt.png ./viewer
```
Compare visually: silhouettes and material colors must match; lighting differs by design (flat ambient vs traced GI). Record both HUD frame-time readouts from the images.

- [ ] **Step 2: Full test sweep** — `cd MatterEngine3/tests && make` (all suites; expect all pass; example_world segfault remains pre-existing/known).

- [ ] **Step 3: Windows build** — headers changed → `cd "MatterEngine3/viewer" && rm -rf build/windows && make windows` → exit 0, `viewer.exe` timestamp fresh. Ask the user to smoke-run it on Windows (GUI apps are user-launched per workflow).

- [ ] **Step 4: Update `MatterEngine3/docs/rendering.md`** — rewrite the "Headline fact" section: rasterization is now the default path (instanced DrawMeshInstanced of flat-part ladders, phase-1 flat lighting); the traced path remains behind `MATTER_RT=1` as reference/baker; note the warm-up cost only applies to `MATTER_RT=1`. Keep the traced-path documentation intact below, retitled "Reference ray tracer (MATTER_RT=1)".

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/docs/rendering.md
git commit -m "docs: rendering.md reflects raster-default viewer"
```

---

## Self-review notes (run after drafting — issues already fixed inline)

- Spec coverage: this plan implements spec Phase 1 only (raster MVP + `MATTER_RT` toggle + HUD + fallback-part drawing via child expansion). Probe volume/light list (Phase 2), clusters + v3 format (Phase 3), ground (Phase 4) are explicitly separate plans.
- Type consistency: `lod_mesh_data` (Task 2) consumed by Task 4's `ensure_mesh`/`build_batches`; `RasterBatch`/`build_batches` signatures match between Tasks 4 and 5; `raster_batches/raster_tris` stats fields match Tasks 5 steps 1–2.
- Known judgment calls: no frustum culling (global constraint), fixed sun/ambient constants in `draw()` (Phase 2 replaces with the light list), normal transform assumes uniform scale (noted in raster.vs comment).
