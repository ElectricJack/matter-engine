# GPU-Driven Instancing & Culling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move per-cluster frustum cull, LOD selection, batch build, and draw submission from the CPU to a GL 4.6 compute + indirect-draw pipeline, add previous-frame HiZ occlusion, and prove scaling on a 500k-instance stress fixture.

**Architecture:** Spec: `docs/superpowers/specs/2026-07-03-gpu-instancing-culling-design.md`. The CPU `SectorLodResolver` stays. A new `GpuCuller` owns per-part mesh atlases, a persistent `ClusterMeta` SSBO, per-frame instance/command/transform SSBOs, a cull compute shader, and a HiZ pyramid. `RasterComposer::draw` keeps global uniforms; geometry submission goes through `glMultiDrawArraysIndirect` (one call per active part).

**Tech Stack:** C++17, raylib (GL 3.3 core; 4.6 entry points via its bundled full glad), GLSL 460 compute/vertex, GNU Make, MinGW cross-compile for Windows.

## Deviations from spec (decided during planning; spec amended in the same commit as this plan)

1. **`glMultiDrawArraysIndirect`, not `...ElementsIndirect`** — `RasterMeshData` is non-indexed triangle soup (no IBO anywhere in the raster path). `MeshRange` = `{first_vertex, vertex_count}`; the indirect command is the 16-byte `DrawArraysIndirectCommand`.
2. **GL uploads live in `GpuCuller`, not `PartStore`** — `PartStore` is GL-free today (comment on `lod_mesh_data`: "GL upload is lazy") and is compiled into headless tests. `PartStore` gains only the CPU-side expansion table; atlases + ClusterMeta upload are `GpuCuller`'s job, reading `LoadedPart` data lazily per part_hash exactly like `ensure_mesh` does now.
3. **Fragment shader is runtime-patched, not duplicated** — the GPU path loads `shaders/raster.fs` (MSL-owned, read-only), string-replaces `#version 330` with `#version 460`, and compiles via `LoadShaderFromMemory`. No forked fragment shader to drift.

## Global Constraints

- **GL floor 4.6 for the GPU path.** Fail fast at startup (clear stderr message + exit) when `MATTER_GPU_CULL=1` and the context reports < 4.6 or any needed entry point is null. raylib window stays `GRAPHICS_API_OPENGL_33`; 4.6 functions come from glad, loaded by raylib's `rlLoadExtensions`.
- **`MAX_LOD = 9`.** `MAX_CLUSTERS_PER_INSTANCE` = max cluster count over loaded parts, computed CPU-side each frame from registered parts.
- **Full `mat4` per instance.** Engine transforms are row-major storage of column-vector matrices (translation at `[3],[7],[11]`). SSBO upload TRANSPOSES to GL column-major memory order (`out[c*4+r] = t[r*4+c]`) so GLSL `mat4 * vec4` reproduces `viewer::transform_point`. Getting this wrong is the #1 expected bug; the parity test catches it.
- **Instance allocation P1** (per-part regions, grow ×1.5 on overflow). P2 (compaction) only if Stage-4 memory demands it — do NOT build P2 speculatively.
- **`MATTER_GPU_CULL` env var** gates the new path until Task 12 flips the default.
- **MatterSurfaceLib is read-only** (genuine-bug exception only — surface it, don't slip it in).
- **Run `make windows` from `MatterEngine3/viewer/` after every engine/viewer change.** After header/struct changes: `make clean` first. A stale `viewer.exe` silently ships old engine code.
- Headless tests: `MatterEngine3/tests/`; the tests Makefile does NOT track header deps reliably — `touch` the suite `.cpp` before rebuilding after header changes. GL-context tests are a separate `gpu-tests` target in the viewer Makefile (they need a window; not part of build-all).
- Benchmark: `MatterEngine3/tools/meadow_sweep.sh <stage-label>` appends to `MatterEngine3/docs/perf/meadow_sweep.csv`; commit rows. Viewer env: `MATTER_WORLD=meadow`, `MATTER_CMD_FIFO=<path>` (Linux only), no `MATTER_RT`.
- Commit after every task (or finer). Branch: `feat/gpu-instancing-culling`.

---

### Task 1: Move frustum/matrix math into `raster_cull.h`

Enables reuse by `GpuCuller` (CPU plane building) and headless tests. Pure code motion + one new test.

**Files:**
- Modify: `MatterEngine3/viewer/raster_cull.h`
- Modify: `MatterEngine3/viewer/raster_composer.cpp` (delete the moved static functions: `mul16` lines 20-27, `make_lookat` 53-84, `make_perspective` 90-101, `extract_frustum_planes` 114-137, `camera_frustum_planes` 141-153)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp` (append)

**Interfaces:**
- Produces (all `inline` in namespace `viewer`, exact bodies copied verbatim from `raster_composer.cpp`):
  - `void mul16(const float* a, const float* b, float* out)`
  - `void make_lookat(const float eye[3], const float target[3], const float up[3], float out[16])`
  - `void make_perspective(float fovy_deg, float aspect, float near_z, float far_z, float out[16])`
  - `void extract_frustum_planes(const float vp[16], float planes[6][4])`
  - `void camera_frustum_planes_raw(const float eye[3], const float target[3], const float up[3], float fovy_deg, float aspect, float planes[6][4])` — NEW thin wrapper: same body as `camera_frustum_planes` but takes raw floats instead of `Camera3D` so it compiles GL-free (near 0.05, far 4000.0 constants kept inside). `raster_composer.cpp` keeps a 3-line static `camera_frustum_planes(const Camera3D&, float, float[6][4])` that unpacks Camera3D and calls the raw version.

- [ ] **Step 1: Write the failing test** — append to `MatterEngine3/tests/viewer_logic_tests.cpp` (follow the file's existing test-function + registration pattern; read its `main()` first):

```cpp
static void test_frustum_planes_known_camera() {
    // Camera at origin looking down -Z (engine convention: forward = target-eye).
    float eye[3] = {0, 0, 0}, target[3] = {0, 0, -1}, up[3] = {0, 1, 0};
    float planes[6][4];
    viewer::camera_frustum_planes_raw(eye, target, up, 60.0f, 16.0f/9.0f, planes);
    // A point straight ahead inside the frustum passes all 6 planes.
    float p[3] = {0, 0, -10};
    for (int i = 0; i < 6; ++i) {
        float d = planes[i][0]*p[0] + planes[i][1]*p[1] + planes[i][2]*p[2] + planes[i][3];
        TEST_ASSERT(d >= 0.0f, "inside point passes plane");
    }
    // A point 1000 units behind the camera fails at least one plane.
    float q[3] = {0, 0, 1000};
    bool outside = false;
    for (int i = 0; i < 6; ++i) {
        float d = planes[i][0]*q[0] + planes[i][1]*q[1] + planes[i][2]*q[2] + planes[i][3];
        if (d < 0.0f) outside = true;
    }
    TEST_ASSERT(outside, "behind point fails a plane");
}
```

(Use the suite's actual assert macro/registration — read the file; if it uses plain `assert` + a call list in `main`, follow that.)

- [ ] **Step 2: Run to verify it fails** — `cd MatterEngine3/tests && touch viewer_logic_tests.cpp && make viewer_logic_tests && ./viewer_logic_tests` (exact target name: check the tests `Makefile`; run `grep viewer_logic Makefile` first). Expected: compile FAIL — `camera_frustum_planes_raw` not declared.

- [ ] **Step 3: Move the code** — cut the five static functions from `raster_composer.cpp` into `raster_cull.h` as `inline`, add the `camera_frustum_planes_raw` wrapper, keep a Camera3D-unpacking static in `raster_composer.cpp`. `raster_cull.h` must NOT include `raylib.h` (it currently includes only `part_store.h` + `<cmath>`; keep it that way).

- [ ] **Step 4: Run tests + build viewer** — tests pass; `cd ../viewer && make viewer` links clean; `make windows` links clean.

- [ ] **Step 5: Commit** — `git commit -m "refactor: frustum/matrix helpers to raster_cull.h for GPU culler reuse"`

---

### Task 2: GL 4.6 gate + `MATTER_GPU_CULL` flag

**Files:**
- Create: `MatterEngine3/viewer/gl46.h`
- Modify: `MatterEngine3/viewer/main.cpp` (after `InitWindow`-equivalent init; find where `renderer` init completes)

**Interfaces:**
- Produces: `bool viewer::gl46_available(std::string& why)`; `bool viewer::gpu_cull_requested()` (reads env once, cached).

- [ ] **Step 1: Write `gl46.h`:**

```cpp
#ifndef VIEWER_GL46_H
#define VIEWER_GL46_H
#include "external/glad.h"   // raylib's bundled full glad; loaded by rlLoadExtensions
#include <cstdlib>
#include <string>

namespace viewer {

// True when the live context exposes everything the GPU cull path needs.
// Call AFTER InitWindow (glad must be loaded).
inline bool gl46_available(std::string& why) {
    GLint maj = 0, min = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &maj);
    glGetIntegerv(GL_MINOR_VERSION, &min);
    if (maj * 10 + min < 43) {   // compute/SSBO/indirect floor; 4.6 gives gl_BaseInstance in GLSL
        why = "GL " + std::to_string(maj) + "." + std::to_string(min) + " < 4.3";
        return false;
    }
    if (!glDispatchCompute)           { why = "glDispatchCompute missing";           return false; }
    if (!glMultiDrawArraysIndirect)   { why = "glMultiDrawArraysIndirect missing";   return false; }
    if (!glBindBufferBase)            { why = "glBindBufferBase missing";            return false; }
    if (!glMemoryBarrier)             { why = "glMemoryBarrier missing";             return false; }
    if (maj * 10 + min < 46) {
        // gl_BaseInstance needs GLSL 460; SPIR-V not used. Hard-require 4.6 per spec.
        why = "GL " + std::to_string(maj) + "." + std::to_string(min) + " < 4.6 (gl_BaseInstance)";
        return false;
    }
    return true;
}

inline bool gpu_cull_requested() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("MATTER_GPU_CULL"); v = (e && e[0] == '1') ? 1 : 0; }
    return v == 1;
}

} // namespace viewer
#endif
```

- [ ] **Step 2: Wire into `main.cpp`** — after window/renderer init, before the main loop:

```cpp
    bool gpu_cull = false;
    if (viewer::gpu_cull_requested()) {
        std::string why;
        if (!viewer::gl46_available(why)) {
            fprintf(stderr, "FATAL: MATTER_GPU_CULL=1 but %s. GPU cull path requires GL 4.6.\n", why.c_str());
            return 1;
        }
        gpu_cull = true;
        printf("GPU cull path: enabled (GL 4.6 ok)\n");
    }
```

`gpu_cull` is unused until Task 7 — add `(void)gpu_cull;` to keep -Wall quiet.

- [ ] **Step 3: Verify** — `cd MatterEngine3/viewer && make viewer && MATTER_GPU_CULL=1 MATTER_WORLD=meadow ./viewer` briefly: expect the "GPU cull path: enabled" line (WSLg GL 4.6 driver) or the FATAL line if the driver caps out (report which — if WSLg only gives 4.2, STOP and surface to Jack before proceeding; the whole package assumes a 4.6-capable dev context). `make windows`.

- [ ] **Step 4: Commit** — `git commit -m "feat: GL 4.6 runtime gate + MATTER_GPU_CULL flag"`

---

### Task 3: GPU record types + packing (GL-free, headless-tested)

**Files:**
- Create: `MatterEngine3/viewer/gpu_cull_types.h`
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp` (append)

**Interfaces:**
- Produces (namespace `viewer`, all GL-free):
  - `constexpr int kMaxLod = 9;`
  - `struct GpuClusterMeta` (128 B, static_asserted)
  - `struct GpuInstanceRec` (80 B, static_asserted)
  - `struct DrawArraysCmd { uint32_t count, instance_count, first, base_instance; }` (16 B)
  - `void transpose_to_gl(const float t[16], float out[16])`
  - `GpuClusterMeta pack_cluster(const LoadedCluster& cl, uint32_t part_slot, uint32_t cluster_index)`
  - `GpuClusterMeta pack_whole_part(const LoadedPart& lp, uint32_t part_slot)` — synthetic cluster for clusterless parts

- [ ] **Step 1: Write the header:**

```cpp
#ifndef VIEWER_GPU_CULL_TYPES_H
#define VIEWER_GPU_CULL_TYPES_H
#include "part_store.h"
#include <cstdint>
#include <cstring>

namespace viewer {

constexpr int kMaxLod = 9;   // ratio-2 ladder max rung count (frame-time package Stage 2)

// std430 mirror of cull.comp's ClusterMeta. 128 B; field order must match the GLSL.
struct GpuClusterMeta {
    float aabb_min[3]; float radius;
    float aabb_max[3]; float pad0;
    float thresholds[kMaxLod];        // finest -> coarsest; unused tail = +inf
    uint32_t lod_mesh_idx[kMaxLod];   // index into the part's MeshRange table; tail = 0
    uint32_t lod_count;
    uint32_t part_slot;               // dense GpuCuller slot, NOT part_hash
    uint32_t cluster_index;           // debug
    uint32_t pad1[3];                 // pad struct to 128 (vec4 alignment)
};
static_assert(sizeof(GpuClusterMeta) == 128, "must match std430 layout in cull.comp");

// std430 mirror of cull.comp's GpuInstance. 80 B.
struct GpuInstanceRec {
    float transform[16];      // GL column-major memory order (transpose_to_gl output)
    uint32_t part_slot;
    uint32_t base_lod;        // debug/HUD only; cluster-level selection is authoritative
    uint32_t cluster_start;   // global ClusterMeta index of this part's first cluster
    uint32_t cluster_count;
};
static_assert(sizeof(GpuInstanceRec) == 80, "must match std430 layout in cull.comp");

// glMultiDrawArraysIndirect command layout (GL spec order).
struct DrawArraysCmd { uint32_t count, instance_count, first, base_instance; };
static_assert(sizeof(DrawArraysCmd) == 16, "GL DrawArraysIndirectCommand");

// Engine float[16] is row-major storage of a column-vector matrix.
// GL/std430 mat4 reads memory as columns, so upload the transpose:
// the shader's M * vec4(p,1) then equals viewer::transform_point(t, p).
inline void transpose_to_gl(const float t[16], float out[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[c*4 + r] = t[r*4 + c];
}

inline GpuClusterMeta pack_cluster(const LoadedCluster& cl, uint32_t part_slot,
                                   uint32_t cluster_index) {
    GpuClusterMeta m{};
    for (int i = 0; i < 3; ++i) { m.aabb_min[i] = cl.aabb_min[i]; m.aabb_max[i] = cl.aabb_max[i]; }
    m.radius = cl.radius;
    uint32_t n = (uint32_t)cl.thresholds.size();
    if (n > (uint32_t)kMaxLod) n = kMaxLod;   // spec: MAX_LOD=9 covers the current ladder
    for (uint32_t i = 0; i < (uint32_t)kMaxLod; ++i) {
        m.thresholds[i]  = (i < n) ? cl.thresholds[i] : 3.402823e38f;  // +inf-ish: never selected
        m.lod_mesh_idx[i] = (i < n) ? (uint32_t)cl.lod_mesh[i] : 0;
    }
    m.lod_count = n;
    m.part_slot = part_slot;
    m.cluster_index = cluster_index;
    return m;
}

// Clusterless (whole-part) path: one synthetic cluster covering the part.
// Thresholds come from the part-level ladder, so the cull shader's selection
// formula (identical to lod_select.cpp) reproduces the resolver's pick.
inline GpuClusterMeta pack_whole_part(const LoadedPart& lp, uint32_t part_slot) {
    GpuClusterMeta m{};
    float r = lp.bound_radius;
    for (int i = 0; i < 3; ++i) { m.aabb_min[i] = -r; m.aabb_max[i] = r; }
    m.radius = r;
    uint32_t n = (uint32_t)lp.thresholds.size();
    if (n > (uint32_t)kMaxLod) n = kMaxLod;
    if (n == 0) { n = 1; m.thresholds[0] = 0.0f; m.lod_mesh_idx[0] = 0; }
    else for (uint32_t i = 0; i < (uint32_t)kMaxLod; ++i) {
        m.thresholds[i]   = (i < n) ? lp.thresholds[i] : 3.402823e38f;
        m.lod_mesh_idx[i] = (i < n && i < lp.lod_mesh_data.size()) ? i : (n ? n - 1 : 0);
    }
    m.lod_count = n;
    m.part_slot = part_slot;
    m.cluster_index = 0xFFFFFFFFu;   // marks synthetic
    return m;
}

} // namespace viewer
#endif
```

- [ ] **Step 2: Failing tests** (append to `viewer_logic_tests.cpp`):

```cpp
static void test_transpose_to_gl_roundtrip() {
    // Engine transform: translate(5,6,7) — translation at [3],[7],[11].
    float t[16] = {1,0,0,5, 0,1,0,6, 0,0,1,7, 0,0,0,1};
    float g[16];
    viewer::transpose_to_gl(t, g);
    // GL column-major memory: translation lands in the 4th column = g[12..14].
    TEST_ASSERT(g[12] == 5 && g[13] == 6 && g[14] == 7, "translation in GL column 3");
    // shader (M*v).x for v=(0,0,0,1) = g[12] -> 5, matches transform_point x.
    float ox, oy, oz;
    viewer::transform_point(t, 0, 0, 0, ox, oy, oz);
    TEST_ASSERT(ox == 5 && oy == 6 && oz == 7, "transform_point agrees");
}

static void test_pack_cluster_thresholds() {
    viewer::LoadedCluster cl{};
    cl.aabb_min[0]=-1; cl.aabb_min[1]=-2; cl.aabb_min[2]=-3;
    cl.aabb_max[0]= 1; cl.aabb_max[1]= 2; cl.aabb_max[2]= 3;
    cl.radius = 3.74f;
    cl.thresholds = {0.5f, 0.25f, 0.125f};
    cl.lod_mesh   = {4, 7, 9};
    auto m = viewer::pack_cluster(cl, 2, 5);
    TEST_ASSERT(m.lod_count == 3, "lod_count");
    TEST_ASSERT(m.thresholds[2] == 0.125f && m.lod_mesh_idx[1] == 7, "arrays copied");
    TEST_ASSERT(m.thresholds[3] > 1e38f, "tail thresholds are +inf");
    TEST_ASSERT(m.part_slot == 2 && m.cluster_index == 5, "ids");
}
```

- [ ] **Step 3: Run to fail, then include the header in the test file, run to pass** — same touch/make/run cycle as Task 1.

- [ ] **Step 4: Commit** — `git commit -m "feat: GPU cull record types + packing (GL-free)"`

---

### Task 4: Expansion table (compositional-children flattening, GL-free)

**Files:**
- Modify: `MatterEngine3/viewer/part_store.h` (add `ExpandedNode` + field on `LoadedPart` + free function decl)
- Modify: `MatterEngine3/viewer/part_store.cpp` (build expansion at the end of `get_or_load`; implement free function)
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp` (append)

**Interfaces:**
- Produces:

```cpp
// part_store.h, namespace viewer
struct ExpandedNode {
    uint64_t part_hash;          // node with drawable lod_mesh_data
    float    rel_transform[16];  // engine row-major, relative to instance root
};
// LoadedPart gains: std::vector<ExpandedNode> expansion;
// Free function (testable without PartStore):
// getter(hash) may return nullptr (unloadable child: skip subtree, matching build_batches).
void build_expansion(uint64_t root_hash,
                     const std::function<const LoadedPart*(uint64_t)>& getter,
                     std::vector<ExpandedNode>& out);
```

Semantics: exactly the recursion in `raster_composer.cpp` `emit` (lines 221-277) minus culling — depth ≤ 8, root node included iff `!lp->lod_mesh_data.empty()` (rel = identity), children composed with `viewer::mul16(parent_rel, child.transform, node_rel)` and recursed. For a leaf flat part the table is exactly one identity node. **Important:** `part_asset::ChildInstance` has fields `child_resolved_hash` and `transform` (see `raster_composer.cpp:273-276`).

- [ ] **Step 1: Failing test:**

```cpp
static void test_build_expansion_leaf_and_children() {
    using namespace viewer;
    // Synthetic parts: root (mesh + 1 child), child (mesh, no children).
    LoadedPart root{}, child{};
    root.lod_mesh_data.resize(1);  root.lod_mesh_data[0].vertex_count = 3;
    child.lod_mesh_data.resize(1); child.lod_mesh_data[0].vertex_count = 3;
    part_asset::ChildInstance ci{};
    ci.child_resolved_hash = 42;
    float tr[16] = {1,0,0,10, 0,1,0,0, 0,0,1,0, 0,0,0,1};  // translate x+10
    memcpy(ci.transform, tr, sizeof tr);
    root.children.push_back(ci);
    auto getter = [&](uint64_t h) -> const LoadedPart* {
        if (h == 1) return &root;
        if (h == 42) return &child;
        return nullptr;
    };
    std::vector<ExpandedNode> out;
    build_expansion(1, getter, out);
    TEST_ASSERT(out.size() == 2, "root node + child node");
    TEST_ASSERT(out[0].part_hash == 1 && out[0].rel_transform[3] == 0, "root identity");
    TEST_ASSERT(out[1].part_hash == 42 && out[1].rel_transform[3] == 10, "child offset");
}
```

(Check `part_asset::ChildInstance`'s exact field names in `MatterEngine3/include/part_asset_v2.h` before writing — if `transform` is named differently, adjust test and impl.)

- [ ] **Step 2: Run to fail. Implement:**

```cpp
// part_store.cpp
static void expand_rec(uint64_t hash, const float parent_rel[16], int depth,
                       const std::function<const viewer::LoadedPart*(uint64_t)>& getter,
                       std::vector<viewer::ExpandedNode>& out) {
    if (depth > 8) return;                       // matches build_batches kMaxDepth
    const viewer::LoadedPart* lp = getter(hash);
    if (!lp) return;
    if (!lp->lod_mesh_data.empty()) {
        viewer::ExpandedNode n;
        n.part_hash = hash;
        memcpy(n.rel_transform, parent_rel, sizeof n.rel_transform);
        out.push_back(n);
    }
    for (const auto& c : lp->children) {
        float rel[16];
        viewer::mul16(parent_rel, c.transform, rel);
        expand_rec(c.child_resolved_hash, rel, depth + 1, getter, out);
    }
}

void viewer::build_expansion(uint64_t root_hash,
        const std::function<const LoadedPart*(uint64_t)>& getter,
        std::vector<ExpandedNode>& out) {
    static const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    expand_rec(root_hash, kIdentity, 0, getter, out);
}
```

At the end of `PartStore::get_or_load` (successful load path), populate `lp.expansion` via `build_expansion(part_hash, [this](uint64_t h){ return get_or_load(h); }, lp.expansion);` — note this recursively loads children exactly as `build_batches` does lazily today. Guard against self-reference (hash == root already handled by depth cap).

- [ ] **Step 3: Run tests to pass; build viewer + windows.**
- [ ] **Step 4: Commit** — `git commit -m "feat: per-part compositional expansion table"`

---

### Task 5: `GpuCuller` — buffers, registration, frustum-only compute cull, readback bridge

The core task. GL-heavy; validated by Task 6's context tests.

**Files:**
- Create: `MatterEngine3/viewer/gpu_culler.h`
- Create: `MatterEngine3/viewer/gpu_culler.cpp`
- Create: `MatterEngine3/viewer/shaders_gpu/cull.comp`
- Modify: `MatterEngine3/viewer/Makefile` (add `gpu_culler.cpp` to `VIEWER_SRC`; add `win-shaders`/`shaders` handling for `shaders_gpu/` — it's a real dir next to the binary, no copy needed on Linux; Windows target: add `cp -r shaders_gpu` to `win-shaders` — check how viewer.exe resolves relative paths, mirror the `shaders` copy)

**Interfaces:**
- Produces (`namespace viewer`):

```cpp
class GpuCuller {
public:
    ~GpuCuller();
    bool init(std::string& err);                 // compile cull.comp, create fixed buffers
    // Lazy per-part GPU registration; returns dense part_slot or -1 on failure.
    int  ensure_part(uint64_t part_hash, PartStore& store);
    // Frame: upload instances (expansion applied), seed cmds, dispatch cull.
    // planes/cam from raster_cull.h helpers. Returns false if nothing to draw.
    bool cull(const std::vector<ResolvedInstance>& resolved, PartStore& store,
              const float cam_eye[3], const float planes[6][4], float pixel_budget);
    // Stage-1 bridge: read back cmds + transforms, rebuild RasterBatch list
    // (transforms converted back via raylib Matrix memcpy convention).
    std::vector<RasterBatch> readback_batches(PartStore& store);
    // HUD:
    size_t culled_clusters() const { return stat_culled_; }   // via readback of a counter
    size_t emitted() const { return stat_emitted_; }
    struct PartGpu {                       // exposed for Task 8's draw loop
        uint64_t part_hash; unsigned vao, vbo;
        std::vector<MeshRange> ranges;     // parallel to LoadedPart::lod_mesh_data
        uint32_t cluster_start, cluster_count;   // global ClusterMeta indices
        uint32_t region_base;              // DrawInstance region start (P1)
        uint32_t region_cap;               // per-instance cap for this part
    };
    const std::vector<PartGpu>& parts() const { return parts_; }
    int part_slot_of(uint64_t hash) const;     // -1 if unregistered
private:
    // GL names: ssbo_clusters_, ssbo_instances_, ssbo_cmds_, ssbo_xforms_,
    // ssbo_stats_; program_cull_; CPU mirrors: cmd_template_, parts_, slot_of_.
};
struct MeshRange { uint32_t first_vertex, vertex_count; };
```

- [ ] **Step 1: Write `cull.comp`:**

```glsl
#version 460
// One thread per (instance, local_cluster). Frustum cull + LOD select, emit
// into per-(cluster,lod) DrawArraysIndirect buckets. Math ports raster_cull.h
// verbatim; parity is tested against the CPU path (gpu_cull_tests).
layout(local_size_x = 64) in;

const int MAX_LOD = 9;

struct ClusterMeta {
    vec4  aabb_min_radius;      // xyz min, w radius
    vec4  aabb_max_pad;
    float thresholds[MAX_LOD];
    uint  lod_mesh_idx[MAX_LOD];
    uint  lod_count;
    uint  part_slot;
    uint  cluster_index;
    uint  pad1a; uint pad1b; uint pad1c;
};
struct GpuInstance {
    mat4 transform;             // column-major (pre-transposed on upload)
    uint part_slot; uint base_lod; uint cluster_start; uint cluster_count;
};
struct DrawCmd { uint count; uint instance_count; uint first; uint base_instance; };

layout(std430, binding = 0) readonly buffer ClusterMetas { ClusterMeta clusters[]; };
layout(std430, binding = 1) readonly buffer Instances    { GpuInstance instances[]; };
layout(std430, binding = 2) buffer DrawCmds              { DrawCmd cmds[]; };
layout(std430, binding = 3) writeonly buffer DrawXforms  { mat4 out_xforms[]; };
layout(std430, binding = 4) buffer Stats                 { uint stat_culled; uint stat_emitted; };

uniform vec4  planes[6];
uniform vec3  cam_eye;
uniform float pixel_budget;
uniform uint  instance_count;
uniform uint  max_clusters_per_instance;

void main() {
    uint tid  = gl_GlobalInvocationID.x;
    uint ii   = tid / max_clusters_per_instance;
    uint lc   = tid % max_clusters_per_instance;
    if (ii >= instance_count) return;
    GpuInstance inst = instances[ii];
    if (lc >= inst.cluster_count) return;
    uint ci = inst.cluster_start + lc;
    ClusterMeta cm = clusters[ci];

    // ---- Frustum: transform 8 AABB corners, all-outside-any-plane => culled.
    vec3 bmin = cm.aabb_min_radius.xyz, bmax = cm.aabb_max_pad.xyz;
    vec3 w[8];
    for (int i = 0; i < 8; ++i) {
        vec3 p = vec3((i & 4) != 0 ? bmax.x : bmin.x,
                      (i & 2) != 0 ? bmax.y : bmin.y,
                      (i & 1) != 0 ? bmax.z : bmin.z);
        w[i] = (inst.transform * vec4(p, 1.0)).xyz;
    }
    for (int p = 0; p < 6; ++p) {
        bool all_out = true;
        for (int i = 0; i < 8; ++i)
            if (dot(planes[p].xyz, w[i]) + planes[p].w >= 0.0) { all_out = false; break; }
        if (all_out) { atomicAdd(stat_culled, 1u); return; }
    }

    // ---- LOD select (== raster_cull.h cluster_lod_select).
    // inst_scale: average basis-column length. Engine columns are the shader
    // matrix's ROWS after the upload transpose; row r = vec3(M[0][r],M[1][r],M[2][r]).
    float sx = length(vec3(inst.transform[0][0], inst.transform[1][0], inst.transform[2][0]));
    float sy = length(vec3(inst.transform[0][1], inst.transform[1][1], inst.transform[2][1]));
    float sz = length(vec3(inst.transform[0][2], inst.transform[1][2], inst.transform[2][2]));
    float scale = (sx + sy + sz) / 3.0;
    vec3 lc3 = (bmin + bmax) * 0.5;
    vec3 wc  = (inst.transform * vec4(lc3, 1.0)).xyz;
    float dist = max(distance(wc, cam_eye), 0.01);
    float psize = cm.aabb_min_radius.w * scale / dist * pixel_budget;
    int lv = int(cm.lod_count) - 1;
    for (uint i = 0u; i < cm.lod_count; ++i)
        if (psize >= cm.thresholds[i]) { lv = int(i); break; }

    // ---- Emit.
    uint bucket = ci * uint(MAX_LOD) + uint(lv);
    uint slot = atomicAdd(cmds[bucket].instance_count, 1u);
    out_xforms[cmds[bucket].base_instance + slot] = inst.transform;
    atomicAdd(stat_emitted, 1u);
}
```

**Note on inst_scale:** CPU `inst_scale` reads engine m[0],m[4],m[8] (column 0 of the engine matrix). After `transpose_to_gl`, engine element `t[r*4+c]` sits at GLSL `M[r][c]`... verify with care: GLSL `M[col][row]`; upload wrote `out[c*4+r] = t[r*4+c]`, and GLSL column `k` = memory floats `[k*4 .. k*4+3]`, so `M[k][r] = out[k*4+r] = t[r*4+k]`. Engine column 0 = `{t[0],t[4],t[8]}` = `{M[0][0], M[1][0], M[2][0]}` — the code above is correct. The parity test is the backstop.

- [ ] **Step 2: Write `gpu_culler.h/.cpp`.** Implementation notes (write real code, ~350 lines):
  - `init`: compile `shaders_gpu/cull.comp` with `glCreateShader(GL_COMPUTE_SHADER)` + link; create `ssbo_stats_` (8 B); zero-size placeholders for growable buffers. Cache uniform locations (`glGetUniformLocation`).
  - `ensure_part(hash, store)`: if `slot_of_.count(hash)` return it. `store.get_or_load(hash)`; build interleaved VBO (stride 36 B: pos 3f, normal 3f, color 4 x u8 normalized, texcoord 2f) concatenating every `lod_mesh_data[i]`, recording `MeshRange{first_vertex, vertex_count}` per mesh index. VAO attributes: location 0 = position (3f), 1 = texcoord (2f), 2 = normal (3f), 3 = color (4 x u8 normalized) — matching Task 8's vertex shader. Append packed ClusterMeta records (all `pack_cluster`, or single `pack_whole_part` when `clusters.empty()`) into a CPU staging vector + `glBufferSubData` grow-realloc of `ssbo_clusters_` (grow: create bigger buffer, `glCopyBufferSubData` old content, delete old). Extend `cmd_template_` (CPU `std::vector<DrawArraysCmd>`) with `cluster_count * kMaxLod` entries: `count = ranges[lod_mesh_idx[lv]].vertex_count`, `first = ranges[...].first_vertex`, `instance_count = 0`, `base_instance` = P1 region layout (below). Initial per-part `region_cap = 4096` instances.
  - **P1 regions:** `region_base` allocated from a running total: each part reserves `region_cap * cluster_count * kMaxLod` slots in `ssbo_xforms_`; bucket `(ci_local, lv)`'s `base_instance = region_base + (ci_local * kMaxLod + lv) * region_cap`. On frame overflow (any part's resolved instance count > `region_cap`): grow that part's cap ×1.5 rounded up, recompute ALL region bases + template `base_instance`s, re-upload template, realloc `ssbo_xforms_`. Overflow risk inside a frame: since `region_cap >= N_p` (resolved count known BEFORE dispatch), clamp is structural — check before dispatch, never trust the shader to bounds-check.
  - `cull(...)`: count instances per part first (one pass over `resolved`, following each instance's expansion table: each `ExpandedNode` becomes one `GpuInstanceRec` with `transform = resolved.transform × node.rel_transform` via `mul16`, then `transpose_to_gl`); `ensure_part` every referenced hash; grow caps as needed; fill CPU staging `std::vector<GpuInstanceRec>`; upload instances (`glBufferData` orphan + `glBufferSubData`); upload `cmd_template_` into `ssbo_cmds_`; zero `ssbo_stats_`; compute `max_clusters_per_instance` = max over registered parts (min 1); set uniforms (`glUniform4fv planes`, etc.); `glBindBufferBase` bindings 0-4; `glDispatchCompute((n_records*max_cpi + 63)/64, 1, 1)`; `glMemoryBarrier(GL_COMMAND_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT)`.
  - `readback_batches(store)`: `glGetBufferSubData` on cmds + xforms; for each bucket with `instance_count > 0`, map `bucket -> (part_hash, cluster_index, lv)` via `parts_` bookkeeping and build `RasterBatch{part_hash, cluster_index (or UINT32_MAX for synthetic), lv, transforms}` — converting each mat4 back to raylib `Matrix` by direct memcpy (GL column-major memory == raylib `MatrixToFloatV` order, i.e. `Matrix` fields m0..m15 in declaration order... **verify**: raylib `Matrix` declaration order m0,m4,m8,m12,m1,... is ROW-major memory; GL memory is column-major; so memcpy GL float16 into `Matrix` yields the TRANSPOSE — which is exactly `row_major_to_matrix(engine_t)`. Write a 5-line comment and one debug assert comparing against `row_major_to_matrix` on the identity+translate case in the Task 6 test).
  - Stats: read `ssbo_stats_` into `stat_culled_/stat_emitted_`.

- [ ] **Step 3: Build** — `make viewer` and `make windows` compile clean (no runtime verification yet; that's Task 6).

- [ ] **Step 4: Commit** — `git commit -m "feat: GpuCuller — compute frustum cull + LOD, indirect command buffers"`

---

### Task 6: GL-context parity tests (`gpu-tests` target)

**Files:**
- Create: `MatterEngine3/viewer/gpu_cull_tests.cpp`
- Modify: `MatterEngine3/viewer/Makefile` (new target `gpu-tests`: links same objects as `viewer` minus `main.o` plus `gpu_cull_tests.o`)

**Interfaces:**
- Consumes: `GpuCuller`, `raster_cull.h` CPU helpers, `gpu_cull_types.h`.

- [ ] **Step 1: Write the test binary.** Structure:

```cpp
// Hidden-window GL 4.6 test harness. Exits non-zero on failure.
// Run: cd MatterEngine3/viewer && make gpu-tests && ./gpu_tests
#include "raylib.h"
#include "gl46.h"
#include "gpu_culler.h"
#include "raster_cull.h"
#include <cstdio>
#include <random>

// Builds a synthetic PartStore-free fixture is impossible (GpuCuller takes
// PartStore&), so use the REAL meadow cache: PartStore over the standard
// cache_root, loading the grass + terrain hashes found in a world manifest is
// heavyweight. Instead: construct a PartStore on a temp cache dir and inject
// a synthetic LoadedPart via a test-only PartStore hook.
```

**Decision:** add a test-only injection hook to `PartStore` (public method, clearly named):

```cpp
// part_store.h — test-only: register a pre-built LoadedPart under a hash.
// Used by gpu_cull_tests to build synthetic fixtures without disk artifacts.
void inject_for_test(uint64_t part_hash, LoadedPart lp);
```

(`loaded_[part_hash] = std::move(lp);` — one line.) Fixture: one part, 2 clusters (AABBs offset ±4 on x), 3 LOD levels each with `lod_mesh_data` filled with a 1-triangle mesh per level (vertices non-empty so registration works), thresholds {0.5, 0.1, 0.02}. 200 instances on a 20×10 grid, spacing 3, camera at origin looking +X... use `camera_frustum_planes_raw` for planes.

Test cases (each a function; `main` runs all, prints PASS/FAIL, returns count of failures):

1. `test_parity_frustum_lod` — CPU reference loop over (instance, cluster): `aabb_culled(...)` then `cluster_lod_select(...)` (both from raster_cull.h), accumulate per-(cluster, lv) count + transform multiset (compare transforms by sorted translation tuples to avoid order dependence). Run `GpuCuller::cull` + `readback_batches`. Assert per-bucket counts identical and translation multisets identical (epsilon 1e-4).
2. `test_matrix_convention` — single instance with translate(5,6,7), no rotation: assert the readback `Matrix` equals `row_major_to_matrix(engine_t)` field-for-field.
3. `test_cap_growth` — 10k instances of the same part (cap starts 4096): assert no crash, emitted count == CPU reference count (growth path exercised).
4. `test_empty_resolve` — `cull({}, ...)` returns false, readback empty.

`main`: `SetConfigFlags(FLAG_WINDOW_HIDDEN); InitWindow(320, 200, "gpu_tests");` then gl46 gate (SKIP with exit 0 + message if unavailable — headless CI tolerance), run tests, `CloseWindow()`.

- [ ] **Step 2: Makefile target:**

```make
GPU_TEST_OBJ = $(filter-out $(L_DIR)/main.o,$(L_ALL_OBJ)) $(L_DIR)/gpu_cull_tests.o
gpu-tests: shaders $(GPU_TEST_OBJ)
	$(CC) $(GPU_TEST_OBJ) -o gpu_tests $(CFLAGS) $(LDFLAGS) $(LDLIBS)
```

(Add `gpu_cull_tests.cpp` compilation via the existing pattern rule — it lives in the viewer dir so the vpath covers it; add its name to a `TEST_ONLY_SRC` var excluded from `VIEWER_SRC`.)

- [ ] **Step 3: Run** — `cd MatterEngine3/viewer && make gpu-tests && ./gpu_tests`. Expected: all PASS. Iterate on GpuCuller/cull.comp until parity holds — matrix-convention and plane-sign bugs land here, not in the meadow.

- [ ] **Step 4: Commit** — `git commit -m "test: GPU cull parity harness (frustum+LOD vs CPU reference)"`

---

### Task 7: Stage-1 wiring — compute cull feeds the existing draw path

**Files:**
- Modify: `MatterEngine3/viewer/main.cpp` (raster branch, lines ~326-338)
- Modify: `MatterEngine3/viewer/ui.h` / `ui.cpp` (HUD: `gpu_cull_active` bool + `gpu culled/emitted` counters line)

**Interfaces:**
- Consumes: `GpuCuller` (init after the gl46 gate), `camera_frustum_planes_raw`.

- [ ] **Step 1: Wire.** In the raster branch, when `gpu_cull` is true:

```cpp
            auto t1 = std::chrono::steady_clock::now();
            if (gpu_cull) {
                float eye[3]    = {cp.x, cp.y, cp.z};
                Vector3 tgt = renderer.camera().target;
                float target3[3] = {tgt.x, tgt.y, tgt.z};
                float up3[3]     = {0, 1, 0};
                float aspect = (float)GetScreenWidth() / (float)GetScreenHeight();
                float planes[6][4];
                viewer::camera_frustum_planes_raw(eye, target3, up3,
                        renderer.camera().fovy, aspect, planes);
                gpu_culler.cull(resolved, *store, eye, planes, stats.pixel_budget);
                batches = gpu_culler.readback_batches(*store);   // Stage-1 bridge
            } else {
                batches = raster->build_batches(resolved, *store, renderer.camera(), state.version());
            }
```

`GpuCuller gpu_culler;` init next to the raster composer init (only when `gpu_cull`; FATAL on `init` failure). HUD gets one line: `GPU cull: emitted N culled M` when active.

- [ ] **Step 2: Verify live** — `MATTER_GPU_CULL=1 MATTER_WORLD=meadow ./viewer`, fly around: image should look identical to the CPU path (same batches, same draw). Screenshot both paths from one FIFO camera pose and diff (`cam ... / shot ...` via FIFO, `compare` or a pixel-diff script if available — a byte-identical PNG is expected since draws are identical modulo batch order; if z-fighting order changes pixels, eyeball instead and note it).
- [ ] **Step 3: Sweep** — `MatterEngine3/tools/meadow_sweep.sh gpucull-stage1` with `MATTER_GPU_CULL=1` exported (check the script: it launches the viewer; env passes through). Commit CSV rows. Expected: build_ms replaced by GPU cull+readback cost; frame totals near stage3 rows (readback is temporary and slow — regression here is acceptable and documented in the commit message).
- [ ] **Step 4: `make windows`** (FIFO is Linux-only, but the .exe must still build + run the flag-off path).
- [ ] **Step 5: Commit** — `git commit -m "feat: Stage-1 GPU cull wired behind MATTER_GPU_CULL (readback bridge)"`

---

### Task 8: Stage-2 — true indirect draw

**Files:**
- Create: `MatterEngine3/viewer/shaders_gpu/raster_gpu_driven.vs`
- Modify: `MatterEngine3/viewer/gpu_culler.h/.cpp` (add `draw_indirect`)
- Modify: `MatterEngine3/viewer/raster_composer.h/.cpp` (add `draw_gpu_driven(GpuCuller&, PartStore&, const Camera3D&)` — global-uniform setup shared with `draw` via a private `setup_frame_uniforms(Shader&)` refactor)
- Modify: `MatterEngine3/viewer/main.cpp` (call `draw_gpu_driven` when `gpu_cull`)
- Modify: `MatterEngine3/viewer/Makefile` (`win-shaders`: also `cp -r shaders_gpu` — verify how the exe resolves the relative path, mirror `shaders`)

**Interfaces:**
- Produces: `int RasterComposer::draw_gpu_driven(GpuCuller& culler, PartStore& store, const Camera3D& cam)` returns drawn tris (sum over buckets of `count/3 * instance_count`, from the culler's CPU cmd readback of JUST the 16-byte commands — small; or keep a GPU query. Start with the small readback: 360 cmds × 16 B).

- [ ] **Step 1: Vertex shader** `shaders_gpu/raster_gpu_driven.vs`:

```glsl
#version 460
// GPU-driven variant of raster.vs: per-instance transform comes from the
// DrawXforms SSBO (written by cull.comp) indexed by gl_BaseInstance +
// gl_InstanceID instead of a divisor-1 vertex attribute.
layout(location = 0) in vec3 vertexPosition;
layout(location = 1) in vec2 vertexTexCoord;
layout(location = 2) in vec3 vertexNormal;
layout(location = 3) in vec4 vertexColor;

layout(std430, binding = 3) readonly buffer DrawXforms { mat4 xforms[]; };

uniform mat4 mvp;

out vec3 fragNormal;
out vec4 fragTint;
out vec2 fragMatAO;
out vec3 fragWorldPos;

void main() {
    mat4 model = xforms[gl_BaseInstance + gl_InstanceID];
    vec4 world = model * vec4(vertexPosition, 1.0);
    fragNormal   = normalize(mat3(model) * vertexNormal);
    fragTint     = vertexColor;
    fragMatAO    = vertexTexCoord;
    fragWorldPos = world.xyz;
    gl_Position  = mvp * world;
}
```

- [ ] **Step 2: Shader load.** In `RasterComposer::init` (or a new `init_gpu_driven` called only when the flag is on): read `shaders_gpu/raster_gpu_driven.vs` and `shaders/raster.fs` with raylib `LoadFileText`, patch the fs: replace leading `#version 330` with `#version 460` (single `std::string::find`/`replace` on the first line), `LoadShaderFromMemory(vs_text, patched_fs)`. Resolve all the same uniform locations into a second `Shader shader_gpu_` + locs set (reuse `setup_frame_uniforms` for sun/probe/material upload against either shader).
- [ ] **Step 3: `GpuCuller::draw_indirect`.** After `cull()`: caller does `BeginMode3D` + `rlDrawRenderBatchActive()` + `glUseProgram(shader_gpu.id)` + set mvp (raylib `SetShaderValueMatrix` with the same `MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection())` raylib uses — simpler: call `rlGetMatrixTransform`? Follow how raylib's `DrawMeshInstanced` computes `mvp`: `MatrixMultiply(MatrixMultiply(transform-model=identity, rlGetMatrixModelview()), rlGetMatrixProjection())`; replicate with rlgl getters). Then per part in `culler.parts()` (skip parts with zero resolved instances this frame — track a per-frame `active_slots_` set in `cull()`):

```cpp
    glBindVertexArray(p.vao);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, ssbo_xforms_);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, ssbo_cmds_);
    // Part's first bucket = its first global cluster index * kMaxLod
    // (buckets are keyed bucket = global_cluster_index * kMaxLod + lod).
    glMultiDrawArraysIndirect(GL_TRIANGLES,
        (const void*)(uintptr_t)((size_t)p.cluster_start * kMaxLod * sizeof(DrawArraysCmd)),
        (GLsizei)(p.cluster_count * kMaxLod), 0);
```

Backface culling disabled around the loop (`rlDisableBackfaceCulling()` / enable after — same as current draw). Restore `glBindVertexArray(0)` and raylib's shader (`rlEnableShader(rlGetShaderIdDefault())` or just let raylib's next draw rebind) before `EndMode3D`.
- [ ] **Step 4: main.cpp** — when `gpu_cull`, skip `readback_batches`; call `stats.raster_tris = raster->draw_gpu_driven(gpu_culler, *store, renderer.camera());`. Delete the Stage-1 readback call (keep the method for tests).
- [ ] **Step 5: Visual gate** — 5-camera FIFO screenshot set, flag on vs flag off: expect visually identical (LOD picks identical per parity; pixel-exact may differ by instance draw ORDER within a bucket — depth test makes overlap deterministic except coplanar z-fights; eyeball + note). Live fly-through.
- [ ] **Step 6: Sweep** — `meadow_sweep.sh gpucull-stage2` with flag on; commit rows. Expected: build_ms ≈ 0, draw_ms small; total at or below stage3 rows.
- [ ] **Step 7: `make windows`; commit** — `git commit -m "feat: Stage-2 indirect draw path (glMultiDrawArraysIndirect + SSBO instancing)"`

---

### Task 9: HiZ pyramid build

**Files:**
- Create: `MatterEngine3/viewer/shaders_gpu/hiz_downsample.comp`
- Modify: `MatterEngine3/viewer/gpu_culler.h/.cpp` (`build_hiz()`, `hiz_tex_`, depth-blit FBO plumbing, `hiz_enabled_` flag default false)
- Modify: `MatterEngine3/viewer/gpu_cull_tests.cpp` (pyramid correctness test)

**Interfaces:**
- Produces: `void GpuCuller::build_hiz(int screen_w, int screen_h)` — call at END of frame (after 3D draw, before ImGui): blits default-framebuffer depth into a depth texture, converts to R32F mip 0, downsamples max-chain. `void set_hiz_enabled(bool)`, `bool hiz_enabled() const`.

- [ ] **Step 1: `hiz_downsample.comp`:**

```glsl
#version 460
// Max-reduce 2x2 -> 1. Bind src as sampler (previous mip), dst as r32f image.
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0)          uniform sampler2D src;   // mip set via textureLod
layout(r32f, binding = 1)    uniform writeonly image2D dst;
uniform int src_mip;
uniform ivec2 dst_size;

void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= dst_size.x || p.y >= dst_size.y) return;
    ivec2 s = p * 2;
    float d0 = texelFetch(src, s,               src_mip).r;
    float d1 = texelFetch(src, s + ivec2(1, 0), src_mip).r;
    float d2 = texelFetch(src, s + ivec2(0, 1), src_mip).r;
    float d3 = texelFetch(src, s + ivec2(1, 1), src_mip).r;
    imageStore(dst, p, vec4(max(max(d0, d1), max(d2, d3))));
}
```

(Odd-size mips: the 2× fetch may read out of bounds — texelFetch OOB is undefined; clamp `s` to `textureSize(src, src_mip)-1` per fetch. Include the clamp in the real shader.)

- [ ] **Step 2: Plumbing.** Create once (and on resize): depth texture `GL_DEPTH_COMPONENT32F` (`depth_copy_tex_`) + FBO wrapping it; R32F texture `hiz_tex_` with full mip chain (`glTexStorage2D`, levels = floor(log2(max(w,h)))+1). `build_hiz`: `glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, depth_fbo_); glBlitFramebuffer(0,0,w,h, 0,0,w,h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);` then a copy pass (fullscreen-free: a tiny compute `depth_to_r32f` — reuse hiz_downsample with src = depth texture sampler, src_mip 0, dst = hiz mip 0, but sampling depth texture needs `sampler2D` with DEPTH_TEXTURE_MODE default — depth textures sample as float, fine; write a second entry uniform `int copy_mode` that does 1:1 texelFetch instead of 2×2 when 1). Then loop mips 1..N binding hiz as both sampler (mip i-1 via `src_mip`) and image (level i) — **NB:** sampling and imaging the same texture at different mips requires `glTextureView` or careful `TEXTURE_BASE_LEVEL`; simplest correct approach: TWO R32F textures ping-pong is wasteful — instead set `GL_TEXTURE_BASE_LEVEL`/`MAX_LEVEL` to pin the sampler to mip i-1 while imaging mip i (legal: texture is mipmap-complete with texStorage; simultaneous read/write to DIFFERENT levels is defined when the levels differ and `glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT)` separates dispatches). Implement with base/max level pinning + barriers between mips.
  - **MSAA guard:** if the default framebuffer has samples > 0 (`glGetIntegerv(GL_SAMPLES)`), print once "HiZ disabled: MSAA framebuffer" and keep `hiz_enabled_` false.
- [ ] **Step 3: Test** (gpu_cull_tests): render nothing; clear depth to 1.0; draw one small quad at known NDC depth 0.25 covering pixels [0..32)² via a throwaway shader (or `glClearBufferfv` a scissored region of the depth copy directly — simpler: skip rendering, write mip 0 directly with `glTexSubImage2D` of a synthetic pattern, then only test the downsample chain). Assert mip1/mip2 texels are the max of their quads (readback via `glGetTexImage`). This isolates the reduce logic from blit behavior; the blit is eyeballed live in Task 10.
- [ ] **Step 4: Call site** — main.cpp end of frame (inside the raster branch, after `EndMode3D`-containing draw call, before `ui.begin_frame()`): `if (gpu_cull) gpu_culler.build_hiz(GetScreenWidth(), GetScreenHeight());`
- [ ] **Step 5: build, gpu-tests pass, `make windows`, commit** — `git commit -m "feat: HiZ max-pyramid build from previous-frame depth"`

---

### Task 10: HiZ occlusion in the cull shader + toggles

**Files:**
- Modify: `MatterEngine3/viewer/shaders_gpu/cull.comp` (HiZ test between frustum and LOD)
- Modify: `MatterEngine3/viewer/gpu_culler.h/.cpp` (bind hiz sampler + `view_proj` uniform, `stat_culled_hiz` in Stats SSBO)
- Modify: `MatterEngine3/viewer/main.cpp` (FIFO `hiz on|off`), `ui.h/ui.cpp` (HUD checkbox `HiZ occlusion` + culled-by-hiz counter)
- Modify: `MatterEngine3/viewer/gpu_cull_tests.cpp` (occlusion fixture)

- [ ] **Step 1: Shader addition** (after the frustum block, before LOD):

```glsl
uniform int  hiz_enabled;
uniform sampler2D hiz_tex;
uniform mat4 view_proj;          // shader-convention VP (CPU vp transposed on upload)
uniform vec2 hiz_size;           // mip-0 texel dims

    // ---- HiZ occlusion (previous-frame pyramid; conservative).
    if (hiz_enabled == 1) {
        vec2 uv_min = vec2( 1e9), uv_max = vec2(-1e9);
        float z_min = 1e9;
        bool crosses_near = false;
        for (int i = 0; i < 8; ++i) {
            vec4 c = view_proj * vec4(w[i], 1.0);
            if (c.w <= 0.001) { crosses_near = true; break; }
            vec3 ndc = c.xyz / c.w;
            uv_min = min(uv_min, ndc.xy); uv_max = max(uv_max, ndc.xy);
            z_min = min(z_min, ndc.z * 0.5 + 0.5);   // [0,1] window depth
        }
        if (!crosses_near) {
            vec2 a = clamp(uv_min * 0.5 + 0.5, 0.0, 1.0);
            vec2 b = clamp(uv_max * 0.5 + 0.5, 0.0, 1.0);
            vec2 px = (b - a) * hiz_size;
            float lod = ceil(log2(max(max(px.x, px.y), 1.0)));
            vec2 mid = (a + b) * 0.5;
            ivec2 msize = textureSize(hiz_tex, int(lod));
            ivec2 t0 = clamp(ivec2(a   * vec2(msize)), ivec2(0), msize - 1);
            ivec2 t1 = clamp(ivec2(vec2(b.x, a.y) * vec2(msize)), ivec2(0), msize - 1);
            ivec2 t2 = clamp(ivec2(vec2(a.x, b.y) * vec2(msize)), ivec2(0), msize - 1);
            ivec2 t3 = clamp(ivec2(b   * vec2(msize)), ivec2(0), msize - 1);
            float far0 = texelFetch(hiz_tex, t0, int(lod)).r;
            float far1 = texelFetch(hiz_tex, t1, int(lod)).r;
            float far2 = texelFetch(hiz_tex, t2, int(lod)).r;
            float far3 = texelFetch(hiz_tex, t3, int(lod)).r;
            float hiz_far = max(max(far0, far1), max(far2, far3));
            if (z_min > hiz_far) { atomicAdd(stat_culled_hiz, 1u); return; }
        }
    }
```

Stats SSBO grows to 3 uints (`stat_culled_frustum`, `stat_culled_hiz`, `stat_emitted`). CPU uploads `view_proj` = transpose of the CPU `vp` from `mul16(view, proj, vp)` (the transpose makes it shader-convention, mirroring what `row_major_to_matrix`+raylib upload does).

**Depth convention check (do FIRST):** main pass is standard GL depth (0=near, 1=far, LEQUAL — verify `glGetIntegerv(GL_DEPTH_FUNC)` at runtime once and read `make_perspective`: it's a standard -1..1 clip projection, so yes standard). Pyramid = MAX reduce ✓ (Task 9). Cull condition `z_min > hiz_far` ✓.

- [ ] **Step 2: Toggles.** FIFO: `else if (sscanf(line.c_str(), "hiz %15s", labelbuf) == 1) { stats.hiz_enabled = (strcmp(labelbuf,"on")==0); }`. HUD checkbox bound to `stats.hiz_enabled` (new `ViewerStats` field, default **true** once Task 10 lands — the parity/regression story is the off-toggle). `gpu_culler.set_hiz_enabled(stats.hiz_enabled)` each frame. First frame guard: `hiz_valid_` false until the first `build_hiz` — pass `hiz_enabled=0` to the shader that frame.
- [ ] **Step 3: Occlusion test** (gpu_cull_tests): fixture with a synthetic HiZ: write mip 0 = 0.1 everywhere (a "wall" at depth 0.1 filling the screen), instances behind it (projected z ≈ 0.5): expect all hiz-culled; instances in front (z ≈ 0.05): expect emitted. Drive the pyramid with `glTexSubImage2D` + downsample, camera fixed, compare `stat_culled_hiz` and emitted counts to expected.
- [ ] **Step 4: Live check** — meadow corner view: HUD `hiz culled` counter > 0 when behind terrain undulation/trees; toggle off → counter 0, image identical (occlusion must never change the image, only skip hidden work). Screenshot on/off diff at all 5 cameras: identical images expected (allow z-fight noise; investigate anything structural).
- [ ] **Step 5: Sweep** — `meadow_sweep.sh gpucull-stage3-hizoff` and `gpucull-stage3-hizon`; commit rows. `make windows`. Commit — `git commit -m "feat: Stage-3 HiZ occlusion culling + toggles"`

---

### Task 11: Stress fixture + scale characterization

**Files:**
- Create: `MatterEngine3/examples/world_demo/WorldData/StressForest50k/world.manifest` (+ `100k`, `200k`, `500k` variants)
- Create: `MatterEngine3/examples/world_demo/schemas/StressForest50k.js` (+ 3 more, each ~12 lines) and `MatterEngine3/examples/world_demo/schemas/stress_forest_lib.js` (shared builder)
- Create: `MatterEngine3/tools/stress_sweep.sh` (copy meadow_sweep.sh structure; 5 cameras scaled to the 2 km world; label column includes world name)
- Create: `MatterEngine3/docs/perf/stress_sweep.csv` (header row)
- Test: `MatterEngine3/tests/` — determinism: hash the flattened world manifest/placement stream for a fixed seed twice (follow how existing world/flatten tests fixture worlds — read `part_flatten_tests.cpp` for the pattern; if world-level fixtures don't exist, test the schema by evaluating placements via ScriptHost as `grass_lod_tests.cpp` does)

Schema sketch (`stress_forest_lib.js` exports one function; per-count files call it):

```js
// stress_forest_lib.js
import { rng } from 'shared-lib/rng';
import { heightAt } from 'shared-lib/terrain_noise';

export function buildForest(self, count, seed) {
  const r = rng(seed);
  const W = 2000.0;                     // 2 km x 2 km
  for (let i = 0; i < count; ++i) {
    const x = r.range(0, W), z = r.range(0, W);
    self.pushMatrix();
    self.translate(x, heightAt(x, z), z);
    self.rotateY(r.range(0, Math.PI * 2));
    const s = r.range(0.7, 1.3);
    self.scale(s, s, s);
    self.placeChild('Tree');
    self.popMatrix();
  }
}
```

```js
// StressForest500k.js
import { buildForest } from './stress_forest_lib';
class StressForest500k extends Part {
  static requires = [{ module: 'Tree' }];
  build(p) { buildForest(this, 500000, 20260703); }
}
export default StressForest500k;
```

**Check before writing:** exact Part API (`placeChild`, `pushMatrix`, class export form) from `Meadow.js`; relative-import support in the module resolver (Meadow imports `shared-lib/...`; test `./stress_forest_lib` resolves — if not, inline the builder into each schema file, 4× duplication accepted). Manifest: `StressForest500k expand` (same `expand` flag as Meadow). No terrain tiles initially (trees float at `heightAt`; if `heightAt` needs terrain schema params, place trees at y=0 — visual fidelity is not the point of this fixture). Add terrain tiles ONLY if the y=0 look breaks LOD/occlusion realism (occluders matter for HiZ measurement: without terrain the forest self-occludes, which is fine).

- [ ] **Step 1:** Write schemas + manifests. `MATTER_WORLD=stressforest50k ./viewer` (world matching is case-insensitive per main.cpp:99 comment — verify) renders, determinism test passes.
- [ ] **Step 2:** First bake will be slow (500k placements flatten/bake Tree once — content-addressed, cached). Confirm the 500k world loads; note load time.
- [ ] **Step 3:** `stress_sweep.sh`: for each world in 50k/100k/200k/500k: launch viewer w/ FIFO, run the 5-camera stats set, append rows `date,stage,world,camera,frame_ms,resolve_ms,build_ms,draw_ms,active,batches,tris,culled`. Run with `MATTER_GPU_CULL=1`, HiZ on.
- [ ] **Step 4:** Characterize: table in commit message — CPU split vs instance count (expect resolve_ms sub-linear, build≈0); frame_ms vs visible tris. **P2 decision:** compute per-part region memory at 500k (printed by GpuCuller at registration); if > ~200 MB total `ssbo_xforms_`, implement P2 (separate follow-up task — STOP and surface to Jack with the numbers first).
- [ ] **Step 5:** Also run the meadow sweep (`gpucull-stage4`) to confirm no regression. Commit CSVs + code — `git commit -m "feat: Stage-4 stress_forest fixture + scale sweep"`

---

### Task 12: Promotion & cleanup (Stage 5)

**Only after Jack has done an interactive fly-through pass on Windows** (fresh `make windows`, meadow + stress forest, flag on) and approved.

**Files:**
- Modify: `MatterEngine3/viewer/gl46.h` (`gpu_cull_requested`: default ON — `MATTER_GPU_CULL=0` opts out; if GL 4.6 unavailable and the var is unset, WARN + fall back to CPU path instead of FATAL)
- Modify: `MatterEngine3/viewer/main.cpp` (fallback logic; delete Stage-1 bridge call if still reachable)
- Modify: `MatterEngine3/viewer/raster_composer.h/.cpp` — delete `build_batches`, fingerprint members, `ensure_mesh` + `mesh_cache_` (whole-batch draw path), keep `draw_gpu_driven` + uniform setup. Keep `RasterBatch` struct only if `readback_batches` (tests) still needs it.
- Modify: `MatterEngine3/tests/viewer_logic_tests.cpp` — delete tests that exercised `build_batches`' fingerprint/cache (identify by grep `fingerprint\|build_batches`); keep raster_cull.h math tests (still exercised by GPU parity).
- Modify: `MatterEngine3/docs/rendering.md` + `MatterEngine3/docs/architecture.md` — replace the CPU batch/cull description with the GpuCuller pipeline (buffers, passes, HiZ, flags).

- [ ] **Step 1:** CPU-path fallback decision: since GL < 4.6 machines can't run the deleted CPU path anymore, the fallback when GL 4.6 is missing becomes: FATAL with clear message (raster path now requires 4.6; `MATTER_RT` ray path unaffected). Confirm with Jack before deleting (the alternative — keeping both paths forever — contradicts the approved spec).
- [ ] **Step 2:** Delete, build, run ALL suites: tests Makefile suites + `gpu-tests` + `./build-all.sh test` at repo root.
- [ ] **Step 3:** Final sweeps: `meadow_sweep.sh gpucull-promoted`, `stress_sweep.sh promoted`; commit rows.
- [ ] **Step 4:** `make clean && make viewer && make windows` (struct changes landed — clean rebuild per project rule).
- [ ] **Step 5:** Commit — `git commit -m "feat: promote GPU-driven cull/draw to default; delete CPU batch path"`

---

## Verification gates summary

| Gate | Where | Blocking |
|---|---|---|
| Frustum-math unit tests | Task 1 | yes |
| GL 4.6 available on dev box | Task 2 | yes — STOP and surface if WSLg caps below 4.6 |
| Pack/transpose unit tests | Task 3 | yes |
| Expansion unit test | Task 4 | yes |
| GPU↔CPU cull parity | Task 6 | yes — do not proceed to wiring until parity |
| Meadow visual + sweep, stage1/2 | Tasks 7-8 | yes |
| HiZ pyramid + occlusion tests | Tasks 9-10 | yes |
| Occlusion changes no pixels | Task 10 | yes |
| Stress scale characterization | Task 11 | data reviewed by Jack |
| Interactive Windows fly-through | before Task 12 | Jack, manual |
