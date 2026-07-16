# Indexed Mesh Format — Stage 1 (Runtime/GPU) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace triangle soup with indexed meshes as the engine's runtime format — welded once at part load, drawn via `vkCmdDrawIndexedIndirect`, ray-traced via indexed BLASes — cutting GBuffer vertex work ~3-6× (measured 25ms GBuffer pass, geometry-bound).

**Architecture:** The QEM bake already produces indexed meshes internally and unwelds them at the API boundary, so shared corners in the soup are bit-identical floats. Stage 1 welds them back (exact-bit key, no epsilon) in `build_raster_mesh_data`, then carries `(unique vertices + uint32 indices)` through `RasterMeshData → VkScenePart → GPU buffers`. Index values are **part-local** (offsets into the part's own vertex range); the per-draw `vertexOffset` field and per-record RT base addresses supply the global rebase, so buffer compaction never rewrites index contents. Disk format is untouched — Stage 2 (separate plan) moves welding into the bake and bumps the artifact format.

**Tech Stack:** C++17, Vulkan 1.3 (`vkCmdDrawIndexedIndirect`, `VK_INDEX_TYPE_UINT32`, indexed `VkAccelerationStructureGeometryTrianglesDataKHR`), GLSL 460 compute + ray tracing, GNU make.

## Global Constraints

- Worktree: `/mnt/d/Shared With Desktop/AI/matter-engine-cpp/.claude/worktrees/gpu-timers-hud`, branch `worktree-gpu-timers-hud`. Run everything from there.
- **No glslc on Linux/WSL.** Shader edits (`shaders_vk/*.comp`, `*.glsl`, `*.rchit`) only compile via Jack's Windows MSYS2 `make -C MatterViewer windows`, which regenerates `embedded_spirv.h`. Tasks 4-5 edit shaders; GPU-executing tests are EXPECTED RED from Task 4 until the Task 6 Windows gate. Do not "fix" that redness on Linux.
- **Never run test suites in parallel** (each 12-15GB; parallel runs OOM-crash WSL2). Chain sequentially.
- Trim per-task test scope: run only the suites named in the task; full sweep is the Task 6 gate only.
- GL/gpu test suites need `GALLIUM_DRIVER=d3d12`.
- Validation shots use the Demo world (`MATTER_WORLD=demo`).
- Purge non-ELF `.o` from `MatterEngine3/tests/build` before Linux test runs if Windows builds touched the shared tree (`find MatterEngine3/tests/build -name '*.o' | xargs file | grep -v ELF`).
- Engine format only: disk `.part`/`.flat.part` artifacts, `part_asset_v2.*`, `BLASManager` registration, and bake code are OUT OF SCOPE (Stage 2 plan).
- Index type is `uint32_t` everywhere. No 16-bit optimization (YAGNI).

## Preamble — commit the pending GPU-timer work first

The worktree holds uncommitted, user-validated GPU-timestamp HUD work (7 files). Before Task 1:

```bash
git add MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/vk_scene_renderer.h \
        MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp \
        MatterViewer/main.cpp MatterViewer/ui.cpp MatterViewer/ui.h
git commit -m "feat(vk): per-pass GPU timestamp zones in HUD (drain-to-drain, EMA-smoothed)"
```

---

### Task 1: Indexed `RasterMeshData` + exact-bit weld

**Files:**
- Modify: `MatterEngine3/src/render/raster_mesh.h`
- Modify: `MatterEngine3/src/render/raster_mesh.cpp`
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp` (existing raster-mesh tests at ~556-567)

**Interfaces:**
- Produces: `RasterMeshData` gains `std::vector<uint32_t> indices;`. After this task, `vertices/normals/...` hold **unique** vertices (`vertex_count` = unique count) and `indices` holds `3 × tri_count` part-local indices, triangle order preserved. New helper `RasterMeshData expand_indexed(const RasterMeshData&)` reproduces the old soup layout (used by GL path in Task 2 and by tests).
- Consumes: nothing new; `build_raster_mesh_data(const Tri*, const TriEx*, int, float)` signature unchanged.

- [ ] **Step 1: Write the failing tests** — append to the raster-mesh section of `viewer_logic_tests.cpp` (follow the file's existing CHECK macro style):

```cpp
static void test_indexed_weld() {
    // Two triangles sharing an edge, bit-identical shared corners, same TriEx.
    Tri t[2]{}; TriEx ex[2]{};
    float3 a = make_float3(0,0,0), b = make_float3(1,0,0),
           c = make_float3(0,1,0), d = make_float3(1,1,0);
    t[0].vertex0 = a; t[0].vertex1 = b; t[0].vertex2 = c;
    t[1].vertex0 = b; t[1].vertex1 = d; t[1].vertex2 = c;
    for (int i = 0; i < 2; ++i) {
        ex[i].N0 = ex[i].N1 = ex[i].N2 = make_float3(0,0,1);
        ex[i].tint = make_float4(1,1,1,1);
        ex[i].ao0 = ex[i].ao1 = ex[i].ao2 = 1.0f;
        ex[i].uv0 = ex[i].uv1 = ex[i].uv2 = make_float2(0.0f);
        ex[i].materialId = 3;
    }
    auto m = viewer::build_raster_mesh_data(t, ex, 2);
    CHECK(m.indices.size() == 6, "indexed: 2 tris -> 6 indices");
    CHECK(m.vertex_count == 4, "indexed: shared edge welds 6 corners to 4 verts");
    for (uint32_t idx : m.indices)
        CHECK(idx < (uint32_t)m.vertex_count, "indexed: indices in range");

    // Same geometry, different material per tri: shared corners MUST NOT weld.
    ex[1].materialId = 4;
    auto s = viewer::build_raster_mesh_data(t, ex, 2);
    CHECK(s.vertex_count == 6, "indexed: material seam splits verts");

    // Expansion round-trips to the legacy soup layout.
    auto soup = viewer::expand_indexed(m);
    CHECK(soup.vertex_count == 6 && soup.indices.empty(),
          "expand_indexed: soup layout");
    CHECK(soup.vertices.size() == 18 && soup.material_ids.size() == 6,
          "expand_indexed: channel sizes");
    // First corner of tri 1 is 'b' = (1,0,0).
    CHECK(soup.vertices[9] == 1.0f && soup.vertices[10] == 0.0f,
          "expand_indexed: triangle order preserved");
}
```
Register `test_indexed_weld()` in the file's test runner list alongside the existing raster-mesh tests. Also update the two existing tests at ~556-567: `build_raster_mesh_data(t, ex, 2)` there asserts soup counts (`vertex_count == 6`); change those assertions to `indices.size() == 6` plus welded `vertex_count` (compute the expected weld count from that test's geometry — corners are distinct there unless shared, inspect and assert accordingly), and route any per-corner channel checks through `expand_indexed`.

- [ ] **Step 2: Run to verify failure**

```bash
make -C MatterEngine3/tests run-viewer-logic
```
Expected: compile FAIL (`indices` not a member / `expand_indexed` undeclared).

- [ ] **Step 3: Implement** — in `raster_mesh.h`, extend the struct and declare the helper:

```cpp
struct RasterMeshData {
    std::vector<float>         vertices;
    std::vector<float>         normals;
    std::vector<unsigned char> colors;
    std::vector<float>         texcoords;
    std::vector<float>         surface_uvs;
    std::vector<uint32_t>      material_ids;
    std::vector<float>         baked_ao;
    // Unique-vertex count. `indices` triangulates them (3 per tri, part-local).
    // Empty `indices` = legacy soup layout (only produced by expand_indexed).
    int vertex_count = 0;
    std::vector<uint32_t>      indices;
};

RasterMeshData build_raster_mesh_data(const Tri* tris, const TriEx* triex, int tri_count,
                                      float default_mat_id = -1.0f);
// Unweld an indexed mesh back to soup (3 corners per triangle, indices empty).
// Legacy shim for the GL path; new code should consume indices directly.
RasterMeshData expand_indexed(const RasterMeshData& indexed);
```

In `raster_mesh.cpp`, rework `build_raster_mesh_data`: keep the existing per-triangle attribute derivation loop exactly as-is, but instead of `push_back`ing 3 corners, build a 52-byte exact-bit key per corner and dedup through a hash map:

```cpp
#include <cstring>
#include <unordered_map>

namespace {
// Exact-bit weld key. The QEM unweld emits bit-identical floats for shared
// corners, so no epsilon: equal bits weld, anything else stays split.
struct VertexKey {
    float px, py, pz, nx, ny, nz;   // position, normal
    float tc0, tc1;                 // texcoords (mat_id, ao)
    float su, sv;                   // surface uv
    float ao;                       // baked_ao
    unsigned char rgba[4];
    uint32_t material_id;
};
static_assert(sizeof(VertexKey) == 52, "no padding: memcmp/hash over raw bytes");

struct VertexKeyHash {
    size_t operator()(const VertexKey& k) const {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&k);
        size_t h = 1469598103934665603ull;               // FNV-1a
        for (size_t i = 0; i < sizeof(VertexKey); ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }
};
struct VertexKeyEq {
    bool operator()(const VertexKey& a, const VertexKey& b) const {
        return std::memcmp(&a, &b, sizeof(VertexKey)) == 0;
    }
};

void append_vertex(viewer::RasterMeshData& d, const VertexKey& k) {
    d.vertices.insert(d.vertices.end(), {k.px, k.py, k.pz});
    d.normals.insert(d.normals.end(), {k.nx, k.ny, k.nz});
    d.colors.insert(d.colors.end(), {k.rgba[0], k.rgba[1], k.rgba[2], k.rgba[3]});
    d.texcoords.insert(d.texcoords.end(), {k.tc0, k.tc1});
    d.surface_uvs.insert(d.surface_uvs.end(), {k.su, k.sv});
    d.material_ids.push_back(k.material_id);
    d.baked_ao.push_back(k.ao);
}
} // namespace
```

In the per-corner loop (replacing the old `for (int v = 0; v < 3; ++v)` push block):

```cpp
        for (int v = 0; v < 3; ++v) {
            VertexKey key{};
            key.px = vs[v].x; key.py = vs[v].y; key.pz = vs[v].z;
            key.nx = ns[v].x; key.ny = ns[v].y; key.nz = ns[v].z;
            key.tc0 = mat_id; key.tc1 = aos[v];
            key.su = uvs[v].x; key.sv = uvs[v].y;
            key.ao = aos[v];
            std::memcpy(key.rgba, rgba, 4);
            key.material_id = material_id;
            auto [it, inserted] = weld.try_emplace(key, (uint32_t)d.material_ids.size());
            if (inserted) append_vertex(d, key);
            d.indices.push_back(it->second);
        }
```
with `std::unordered_map<VertexKey, uint32_t, VertexKeyHash, VertexKeyEq> weld;` declared before the triangle loop (reserve `tri_count * 2`), and after the loop `d.vertex_count = (int)d.material_ids.size();`. Drop the old `d.vertex_count = tri_count * 3` and adjust the `reserve`s accordingly.

`expand_indexed`:

```cpp
RasterMeshData expand_indexed(const RasterMeshData& in) {
    if (in.indices.empty()) return in;   // already soup
    RasterMeshData out;
    out.vertex_count = (int)in.indices.size();
    out.vertices.reserve(in.indices.size() * 3);
    for (uint32_t idx : in.indices) {
        const size_t p = (size_t)idx * 3, c = (size_t)idx * 4, uv = (size_t)idx * 2;
        out.vertices.insert(out.vertices.end(),
                            {in.vertices[p], in.vertices[p+1], in.vertices[p+2]});
        out.normals.insert(out.normals.end(),
                           {in.normals[p], in.normals[p+1], in.normals[p+2]});
        out.colors.insert(out.colors.end(),
                          {in.colors[c], in.colors[c+1], in.colors[c+2], in.colors[c+3]});
        out.texcoords.insert(out.texcoords.end(), {in.texcoords[uv], in.texcoords[uv+1]});
        out.surface_uvs.insert(out.surface_uvs.end(),
                               {in.surface_uvs[uv], in.surface_uvs[uv+1]});
        out.material_ids.push_back(in.material_ids[idx]);
        out.baked_ao.push_back(in.baked_ao[idx]);
    }
    return out;
}
```

- [ ] **Step 4: Run to verify pass**

```bash
make -C MatterEngine3/tests run-viewer-logic
```
Expected: PASS including `test_indexed_weld`.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/raster_mesh.h MatterEngine3/src/render/raster_mesh.cpp \
        MatterEngine3/tests/viewer_logic_tests.cpp
git commit -m "feat(mesh): weld triangle soup to indexed RasterMeshData at build"
```

---

### Task 2: Keep the GL legacy path green (soup expansion shim)

**Files:**
- Modify: `MatterEngine3/src/render/gpu_culler.cpp` (~line 345-352, the lod_mesh_data concatenation)
- Test: `MatterEngine3/tests/gpu_cull_tests.cpp` (helpers `make_one_tri_mesh` ~186, `md2a/md2b` ~264-278, and ~1077/1153), `MatterEngine3/tests/release_part_tests.cpp` (`make_one_tri_mesh` ~72)

**Interfaces:**
- Consumes: `expand_indexed` from Task 1.
- Produces: GL GPU-driven path behavior unchanged (still `glMultiDrawArraysIndirect` over soup); test mesh factories now emit indexed meshes matching the Task 1 invariant.

The GL culler concatenates `lp->lod_mesh_data` into one soup vertex buffer and records per-mesh `first_vertex/vertex_count` ranges (gpu_culler.cpp:345-352, consumed at ~469-470). The GL path is legacy (Vulkan is the shipping path); it gets an expansion shim, not an indexed rewrite.

- [ ] **Step 1: Update test factories to the indexed invariant** — every hand-built `RasterMeshData` in the two test files currently fills 3 soup corners. For each factory (e.g. `make_one_tri_mesh`), after the existing channel fills add `md.indices = {0u, 1u, 2u};` (vertex_count stays 3 — one triangle has no shared corners, so unique==corner count and the ranges the tests assert are unchanged). For `md2a`/`md2b` and the ~1077/1153 sites do the same per triangle (`indices = {0,1,2}` for 3-vert meshes; for any N-triangle mesh, `0..3N-1`).

- [ ] **Step 2: Run to verify current failure mode**

```bash
GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-gpu-culler
```
Expected before the shim: PASS or FAIL depending on how the culler treats non-empty `indices` (it ignores them and uploads unique vertices as if they were soup — for 1-tri meshes counts coincide, so multi-tri weld cases in real worlds would silently corrupt). Regardless of local pass/fail, apply Step 3 — correctness demands the shim.

- [ ] **Step 3: Implement the shim** — in `gpu_culler.cpp` at the concatenation loop (~350):

```cpp
    for (const auto& md_src : lp->lod_mesh_data) {
        const RasterMeshData md_expanded = expand_indexed(md_src);
        const RasterMeshData& md = md_expanded;
        // ... existing body unchanged (reads md.vertex_count, md.vertices, ...)
```
(keep the rest of the loop body byte-for-byte; only the binding of `md` changes). Add `#include "raster_mesh.h"` if not already present.

- [ ] **Step 4: Run to verify pass**

```bash
GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-gpu-culler
make -C MatterEngine3/tests run-release-part
```
Expected: PASS both.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/gpu_culler.cpp MatterEngine3/tests/gpu_cull_tests.cpp \
        MatterEngine3/tests/release_part_tests.cpp
git commit -m "feat(mesh): GL legacy path expands indexed meshes at upload"
```

---

### Task 3: Indexed `VkScenePart` assembly

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h` (`VkSceneLod` ~97-101, `VkScenePart` ~119-125, `vk_scene_detail::RtGeometrySelection` ~128-133)
- Modify: `MatterEngine3/src/matter_engine.cpp` (`load_part_for_vk_scene` ~2943-3085)
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp` (`ensure_part` validation ~2319-2321, RT lod records ~2357-2380, rebase ~2425, `select_rt_instance_geometry` emit ~684)
- Test: `MatterEngine3/tests/vk_scene_renderer_tests.cpp` (or the suite covering `select_scene_cluster_lod` / `ensure_part` — locate the existing `VkSceneLod`-constructing tests and update)

**Interfaces:**
- Produces:
```cpp
struct VkSceneLod {
    uint32_t first_index = 0;   // into VkScenePart::indices (part-local until ensure_part rebases to global)
    uint32_t index_count = 0;   // 3 × triangle count
    float threshold = 0.0f;
};
struct VkScenePart {
    uint64_t part_hash = 0;
    std::vector<VkSceneCluster> clusters;
    std::vector<VkRasterVertex> vertices;   // unique vertices, all LOD meshes concatenated
    std::vector<uint32_t> indices;          // PART-LOCAL values (offsets into `vertices`)
};
struct RtGeometrySelection {
    uint32_t cluster_index = 0;
    uint32_t lod_index = 0;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
};
```
- Invariant established here and relied on by Tasks 4-5: **index VALUES are part-local** (already include the owning mesh's vertex offset within the part). Global vertex rebase comes from `vertexOffset`/base addresses, never from rewriting index contents.
- Consumes: `RasterMeshData.indices` from Task 1.

- [ ] **Step 1: Write the failing test** — in the suite that already builds `VkScenePart` fixtures for `ensure_part`/LOD-selection (find the existing fixture; extend it):

```cpp
// Two-LOD part: LOD0 = 2 welded tris (4 verts, 6 idx), LOD1 = 1 tri (3 verts, 3 idx).
// Assembled part: 7 vertices; LOD1 indices are part-local (offset by 4).
viewer::VkScenePart part;
part.part_hash = 0x1DEAF00Dull;
part.vertices.resize(7);
part.indices = {0,1,2, 1,3,2,   /* LOD1, mesh base 4: */ 4,5,6};
viewer::VkSceneCluster cluster;
cluster.aabb_min = {-1,-1,-1}; cluster.aabb_max = {1,1,1}; cluster.radius = 1.7f;
cluster.lods.push_back({0u, 6u, 2.0f});
cluster.lods.push_back({6u, 3u, 0.0f});
part.clusters.push_back(cluster);
std::string err;
CHECK(renderer.ensure_part(part, err) >= 0, "indexed part registers");
```
Plus a validation-rejection case: `part.indices = {0,1,99}` (out-of-range index) must make `ensure_part` fail with non-empty `err`.

- [ ] **Step 2: Run to verify failure**

```bash
make -C MatterEngine3/tests run-vk-scene-renderer
```
Expected: compile FAIL (`VkSceneLod` has no `first_index`; brace-init arity).

- [ ] **Step 3: Implement**

(a) Header struct changes as in **Produces** above (replace fields in place; keep comments explaining part-local indices).

(b) `matter_engine.cpp` `load_part_for_vk_scene`: the per-mesh vertex loop (~2990-3039) stays, but it now iterates **unique** vertices, and per-vertex channel reads index by `vi` exactly as today (channels are parallel to unique vertices after Task 1 — no change to the body). After the vertex loop for mesh `mi`, append the mesh's indices rebased by the mesh's vertex offset, and record the mesh's index offset:

```cpp
    std::vector<uint32_t> mesh_index_offsets(loaded.lod_mesh_data.size(), UINT32_MAX);
    // inside the mesh loop, after vertices are appended:
    mesh_index_offsets[mi] = static_cast<uint32_t>(part.indices.size());
    for (uint32_t idx : mesh.indices)
        part.indices.push_back(mesh_offsets[mi] + idx);   // part-local value
```
Guard: `if (mesh.indices.empty()) continue;` alongside the existing `vertex_count <= 0` skip (~2986). Cluster LOD emission (~3059-3061 and ~3076-3078) becomes:

```cpp
    cluster.lods.push_back({mesh_index_offsets[mesh_index],
                            static_cast<uint32_t>(mesh.indices.size()),
                            source.thresholds[li]});
```
(same shape at the whole-part fallback site, and the `mesh_offsets[...] == UINT32_MAX` guards extend to `mesh_index_offsets`).

(c) `vk_scene_renderer.cpp` `ensure_part`:
- Validation (~2319-2321) becomes range checks on indices: `lod.first_index/index_count` against `part.indices.size()`, `index_count % 3 == 0`, and every referenced `part.indices[i] < part.vertices.size()` (single pass over the part's index array once, not per LOD).
- RT lod records (~2371-2378): `rt_lod.first_index = lod.first_index; rt_lod.index_count = lod.index_count; rt_lod.primitive_count = lod.index_count / 3;` and the opacity scan iterates `part.vertices[part.indices[lod.first_index + k]]`.
- Rebase (~2425): `for (auto& lod : lods) lod.first_index += index_base;` where `index_base` is the part's offset in the renderer's global index staging (mirror of the existing `vertex_base` bookkeeping — this task adds the CPU-side `index_staging_` bookkeeping and `PartRecord` fields `index_start/index_count`, but NOT yet the GPU buffer; Task 4 uploads it). Follow the exact pattern of the existing vertex staging append and `PartRecord.vertex_start/vertex_count` fields. Note `PartRecord.vertex_start` must NO LONGER be folded into lod offsets (indices are part-local): keep `vertex_start` in the record for Task 4's `vertexOffset`.
- `select_rt_instance_geometry` emit (~684): `{cluster_index, lod_index, lod.first_index, lod.index_count}`.
- Release/compaction (~3648-3674): mirror the vertex compaction for the index staging — shift the index array contents down, `lod.first_index = index_delta + (lod.first_index - old_index_start)`; index **values** are part-local and untouched. Vertex compaction keeps adjusting `PartRecord.vertex_start` only.

- [ ] **Step 4: Run to verify pass**

```bash
make -C MatterEngine3/tests run-vk-scene-renderer
make -C MatterEngine3/tests run-release-part
```
Expected: PASS. (Draw/GPU paths still soup-driven and broken-in-waiting is fine; these suites cover CPU bookkeeping.)

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp \
        MatterEngine3/src/matter_engine.cpp MatterEngine3/tests/vk_scene_renderer_tests.cpp
git commit -m "feat(vk): assemble VkScenePart with part-local index buffers"
```

---

### Task 4: Indexed indirect draws — DrawCommand layout, cull.comp, index buffer, draw sites

**Files:**
- Modify: `MatterEngine3/src/render/vk_draw_command.h`
- Modify: `MatterEngine3/shaders_vk/cull.comp` (struct ~42-47, stats ~183)
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h` (static asserts ~86-95, `FrameResources`/members for the index buffer, `RasterRecord` wiring)
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp` (command staging fill ~3898-3917, index buffer creation/upload mirroring the vertex buffer path ~1790/4131-4135, `record_raster` bind+draw ~392-430)
- Test: `MatterEngine3/tests/vk_scene_renderer_tests.cpp` + `MatterEngine3/tests/gpu_cull_tests.cpp` (any `DrawCommand{...}` literals)

**Interfaces:**
- Produces:
```cpp
struct DrawCommand {                 // VkDrawIndexedIndirectCommand-compatible
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;            // GLOBAL (part index_start + lod.first_index)
    int32_t  vertex_offset;          // part's global vertex_start
    uint32_t first_instance;
};
```
- Consumes: Task 3's `PartRecord.index_start/vertex_start` and global `lod.first_index`.
- **Shader gate:** after this task, embedded SPIR-V is stale on Linux. GPU-executing suites go red until Task 6's Windows rebuild. CPU/logic suites must stay green.

- [ ] **Step 1: Update the failing tests first** — every `DrawCommand` literal/assert in the two test files gets the 5-field layout; expected commands in cull tests change `vertex_count/first_vertex` → `index_count/first_index` and gain `vertex_offset`. Static asserts to update in `vk_draw_command.h`:

```cpp
struct DrawCommand {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t  vertex_offset;
    uint32_t first_instance;
};
static_assert(std::is_standard_layout<DrawCommand>::value,
              "indirect command must be standard layout");
static_assert(sizeof(DrawCommand) == 5 * sizeof(uint32_t),
              "VkDrawIndexedIndirectCommand-compatible layout");

inline bool operator==(const DrawCommand& a, const DrawCommand& b) {
    return a.index_count == b.index_count &&
           a.instance_count == b.instance_count &&
           a.first_index == b.first_index &&
           a.vertex_offset == b.vertex_offset &&
           a.first_instance == b.first_instance;
}
```
and in `vk_scene_renderer.h` ~86-95 replace the four offset asserts with:

```cpp
static_assert(sizeof(DrawCommand) == sizeof(VkDrawIndexedIndirectCommand),
              "DrawCommand must match VkDrawIndexedIndirectCommand");
static_assert(offsetof(DrawCommand, index_count) ==
              offsetof(VkDrawIndexedIndirectCommand, indexCount));
static_assert(offsetof(DrawCommand, instance_count) ==
              offsetof(VkDrawIndexedIndirectCommand, instanceCount));
static_assert(offsetof(DrawCommand, first_index) ==
              offsetof(VkDrawIndexedIndirectCommand, firstIndex));
static_assert(offsetof(DrawCommand, vertex_offset) ==
              offsetof(VkDrawIndexedIndirectCommand, vertexOffset));
static_assert(offsetof(DrawCommand, first_instance) ==
              offsetof(VkDrawIndexedIndirectCommand, firstInstance));
```

- [ ] **Step 2: Run to verify failure**

```bash
make -C MatterEngine3/tests run-vk-scene-renderer
```
Expected: compile FAIL at every stale `DrawCommand` use — this enumerates the sites Step 3 must fix. Fix them all.

- [ ] **Step 3: Implement**

(a) `cull.comp` struct (~42-47) and stats line (~183):

```glsl
struct DrawCommand {
    uint index_count;
    uint instance_count;
    uint first_index;
    int  vertex_offset;
    uint first_instance;
};
...
    atomicAdd(triangles, commands[bucket].index_count / 3u);
```
(`reserve_transform_slot` reads `first_instance` by name — no change needed.)

(b) Command staging fill (~3915-3917):

```cpp
                command.index_count = lods[lod].index_count;
                command.first_index = lods[lod].first_index;      // already global (Task 3)
                command.vertex_offset =
                    static_cast<int32_t>(parts_[cluster.part_slot].vertex_start);
```
(preserve the existing surrounding logic for `first_instance` and empty-part guards; the old `if (parts_[...].vertex_count != 0)` rebase block at ~3917 that added vertex bases into `first_vertex` is deleted — that's what `vertex_offset` now does.)

(c) Index buffer: mirror the vertex-buffer path 1:1 — `ensure_index_buffer(VkDeviceSize, std::string&)` next to `ensure_vertex_buffer` (~1790) with usage
`VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`, upload from the Task 3 `index_staging_` in the same block that uploads `vertex_staging_` (~4120-4135, add `uploaded_index_count_`), and reset alongside `uploaded_vertex_count_` (~6143, ~5584 guard gains `|| uploaded_index_count_ == 0`).

(d) `record_raster`: add `VkBuffer index_buffer` to `RasterRecord`, fill it where the record is initialized, and in the GBuffer pass after `vkCmdBindVertexBuffers` (~404-406):

```cpp
    vkCmdBindIndexBuffer(command_buffer, record.index_buffer, 0,
                         VK_INDEX_TYPE_UINT32);
```
then replace both `vkCmdDrawIndirect` calls in the draw-range loop with:

```cpp
                vkCmdDrawIndexedIndirect(command_buffer, record.indirect_buffer,
                                         static_cast<VkDeviceSize>(first) *
                                             sizeof(DrawCommand),
                                         count, sizeof(DrawCommand));
```
(the `sizeof(DrawCommand)` stride is now 20 — correct automatically).

(e) Grep for any remaining `vertex_count / 3` triangle math on the CPU side of commands/stats and convert to `index_count / 3`.

- [ ] **Step 4: Run to verify** — CPU suites green, GPU suites expected red:

```bash
make -C MatterEngine3 -j8 && make -C MatterEngine3/tests run-vk-scene-renderer
```
Expected: build PASS; suite PASS for CPU-side tests. If the suite executes pipelines against stale embedded SPIR-V, mark failures as the documented Task 6 gate — do NOT patch around them.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_draw_command.h MatterEngine3/shaders_vk/cull.comp \
        MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp \
        MatterEngine3/tests/vk_scene_renderer_tests.cpp MatterEngine3/tests/gpu_cull_tests.cpp
git commit -m "feat(vk): indexed indirect draws (DrawCommand -> VkDrawIndexedIndirectCommand)"
```

---

### Task 5: Indexed ray tracing — BLAS geometry, RT part records, hit-shader fetch

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp` (per-part RT geometry ~2340-2380, BLAS build ~4500-4524, record fill ~4640-4683)
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h` (per-part RT resources ~207-220: add `rt_index` buffer + `index_address`)
- Modify: `MatterEngine3/src/render/vk_gi_contract.h` (`GpuRtPartRecord` ~63-104)
- Modify: `MatterEngine3/shaders_vk/rt_surface_common.glsl` (record struct ~23-31, fetch ~118-160)
- Test: `MatterEngine3/tests/vk_scene_renderer_tests.cpp` (record-layout asserts / any `GpuRtPartRecord` fixtures)

**Interfaces:**
- Produces (C++ and GLSL mirrors must stay bit-identical):
```cpp
typedef struct alignas(16) GpuRtPartRecord {
    uint64_t vertex_address;     // part-base VkRasterVertex buffer address
    uint64_t index_address;      // this BLAS's first index (base + first_index*4)
    uint32_t vertex_stride;      // 72
    uint32_t vertex_count;       // part unique-vertex count
    uint32_t primitive_count;    // index_count / 3 for this BLAS
    uint32_t valid;
    uint32_t pad0, pad1, pad2, pad3;
} GpuRtPartRecord;               // 48 bytes: "three vec4 records"
```
- Consumes: part-local index invariant (Task 3): BLAS `vertexData` = part base, `indexData` = per-LOD offset — indices resolve against the part base with no vertexOffset concept needed.

- [ ] **Step 1: Update the failing asserts/tests** — `vk_gi_contract.h` asserts become `sizeof(GpuRtPartRecord) == 48` / "three vec4 records"; fix any test fixture that brace-initializes the record.

- [ ] **Step 2: Run to verify failure**

```bash
make -C MatterEngine3/tests run-vk-scene-renderer
```
Expected: compile FAIL on record initializers/asserts, enumerating every site.

- [ ] **Step 3: Implement**

(a) Per-part RT resources: alongside the existing `rt_geometry` vertex upload (~2340-2360, `record.vertex_count = part.vertices.size()`), create and upload a per-part `rt_index` buffer holding `part.indices` verbatim (part-local values), usage
`VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT`, mirroring exactly how `rt_geometry` is created/uploaded/destroyed (including the release path). Store `index_address` next to the existing `vertex_address` (h:~213).

(b) BLAS build (~4500-4524):

```cpp
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = part.rt_geometry->address;   // part base, no LOD offset
        triangles.vertexStride = sizeof(VkRasterVertex);
        triangles.maxVertex = part.vertex_count - 1;
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress =
            part.rt_index->address +
            static_cast<VkDeviceSize>(lod.first_index) * sizeof(uint32_t);
        ...
        range.primitiveCount = lod.index_count / 3;
```
(`lod` here is the `RtLodRecord` renamed in Task 3: `first_index/index_count/primitive_count`.)

(c) Record fill (~4665-4671):

```cpp
        GpuRtPartRecord record{};
        record.vertex_address = part.rt_geometry->address;    // part base
        record.index_address =
            part.rt_index->address +
            static_cast<uint64_t>(lod.first_index) * sizeof(uint32_t);
        record.vertex_stride = sizeof(viewer::VkRasterVertex);
        record.vertex_count = part.vertex_count;
        record.primitive_count = lod.primitive_count;
        record.valid = 1u;
```

(d) `rt_surface_common.glsl`:

```glsl
struct GpuRtPartRecord {
    uvec2 vertex_address;
    uvec2 index_address;
    uint vertex_stride;
    uint vertex_count;
    uint primitive_count;
    uint valid;
    uint pad0; uint pad1; uint pad2; uint pad3;
};

layout(buffer_reference, buffer_reference_align = 4) readonly buffer
RtIndexBuffer {
    uint indices[];
};
```
and in `load_rt_surface`:

```glsl
    uint part_slot = gl_InstanceCustomIndexEXT;
    GpuRtPartRecord part = rt_parts[part_slot];
    if (part.valid == 0u || all(equal(part.vertex_address, uvec2(0u))) ||
        all(equal(part.index_address, uvec2(0u))) ||
        part.vertex_stride != 72u || gl_PrimitiveID >= part.primitive_count) {
        atomicAdd(invalid_part_records, 1u);
        return invalid_rt_surface();
    }
    RtIndexBuffer index_buffer = RtIndexBuffer(part.index_address);
    uint tri = gl_PrimitiveID * 3u;
    uint i0 = index_buffer.indices[tri];
    uint i1 = index_buffer.indices[tri + 1u];
    uint i2 = index_buffer.indices[tri + 2u];
    if (max(i0, max(i1, i2)) >= part.vertex_count) {
        atomicAdd(invalid_part_records, 1u);
        return invalid_rt_surface();
    }
    RtVertexBuffer geometry = RtVertexBuffer(part.vertex_address);
    RtRasterVertex v0 = rt_load_vertex(geometry, i0, part.vertex_stride);
    RtRasterVertex v1 = rt_load_vertex(geometry, i1, part.vertex_stride);
    RtRasterVertex v2 = rt_load_vertex(geometry, i2, part.vertex_stride);
```
(barycentric attribute math below is unchanged).

- [ ] **Step 4: Run to verify** — CPU-side suites:

```bash
make -C MatterEngine3 -j8 && make -C MatterEngine3/tests run-vk-scene-renderer
```
Expected: build PASS; CPU asserts PASS; GPU-executing paths remain gated on Task 6.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/vk_scene_renderer.h \
        MatterEngine3/src/render/vk_gi_contract.h MatterEngine3/shaders_vk/rt_surface_common.glsl \
        MatterEngine3/tests/vk_scene_renderer_tests.cpp
git commit -m "feat(rt): indexed BLAS geometry + index-buffer fetch in hit shading"
```

---

### Task 6: Windows shader gate, full validation, perf measurement

**Files:** none created — build, test, measure.

- [ ] **Step 1: Jack rebuilds shaders + viewer on Windows** (regenerates `embedded_spirv.h` — the only glslc):

```bash
make -C MatterViewer windows
```
Because struct/header layouts changed, do a **clean** Windows rebuild (clear all objs first, per project policy on struct changes).

- [ ] **Step 2: Full Linux test sweep, sequential (never parallel):**

```bash
make -C MatterEngine3 -j8
make -C MatterEngine3/tests run-viewer-logic
make -C MatterEngine3/tests run-vk-scene-renderer
make -C MatterEngine3/tests run-release-part
GALLIUM_DRIVER=d3d12 make -C MatterEngine3/tests run-gpu-culler
make -C MatterEngine3/tests run-partstore
make -C MatterEngine3/tests run-composition
```
Expected: ALL PASS (purge non-ELF `.o` from `tests/build` first if the Windows build shared the tree).

- [ ] **Step 3: Visual + perf validation on Demo world** (Jack, live viewer, `MATTER_WORLD=demo`): confirm no missing/flickering geometry in raster AND RT lighting (leaf translucency, shadows), then read the HUD zones. Baseline (this morning, worst-case fullscreen no-DLSS): GBuf 25.0, RT 25.7, total 77.6. Success = GBuf materially down (predicted ~3-6× less vertex work; watch `Ras tris` stay equal), RT unchanged or better, no `invalid_part_records` HUD/console spew, `(other)` residual still ~0.

- [ ] **Step 4: Record results + wrap up** — note before/after zone numbers in the final commit message:

```bash
git commit --allow-empty -m "perf(vk): indexed mesh format validated — GBuf <before>ms -> <after>ms (demo world)"
```

**Rollback line:** every task is a separate commit on `worktree-gpu-timers-hud`; `git revert` of Tasks 4-5 plus a Windows rebuild restores soup draws without touching Tasks 1-3.

---

## Self-Review Notes

- **Coverage:** producers (weld at `build_raster_mesh_data`), GL consumer (shim), Vulkan assembly, indirect-draw plumbing, RT BLAS + hit fetch, tests, shader gate, perf gate — all soup consumers found by grep are covered. Bake/serialization explicitly deferred to Stage 2.
- **Type consistency:** `VkSceneLod{first_index,index_count,threshold}` (Task 3) matches Task 4's command fill and Task 5's BLAS/record math; `DrawCommand` 5-field layout consistent across vk_draw_command.h, cull.comp, and asserts; `GpuRtPartRecord` 48-byte layout identical in C++ and GLSL.
- **Known risks:** (1) weld ratio on foliage may be < 6× (material/normal seams) — the win shrinks but never inverts, since index_count == old vertex_count and unique verts ≤ soup verts. (2) Stale-SPIR-V window between Tasks 4-6 is deliberate; do not run the viewer against a half-rebuilt exe. (3) `ClusterMeta.lod_mesh_idx` in cull.comp is unused by this plan's changes — leave it.
