# Organic Surface Carving Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliberately recreate an organic cinderblock surface (divots, crevices, concavity, coarse lumpiness) on the watertight lattice mesh, via smooth-CSG subtractive particles plus low-frequency additive-radius modulation.

**Architecture:** Subtractive "carve" particles ride a separate array (mirroring the existing `clipParticles` plumbing), smooth-subtracted from the additive smooth-min union in both the scalar field and the normal gradient. Carve particles are generated from surface particles by noise, stored on the `Cluster`, and distributed per-cell using the same `intersects_sphere` halo that keeps the additive field continuous. Lumpiness is a separate, pure input-side radius modulation in `make_sub_particle`.

**Tech Stack:** C (marching-cubes mesher `surface.c`), C++17 (`cell.cpp`, `cluster.cpp`, `particle_culling.cpp`, `main.cpp`), raylib PODs, g++/gcc. Build: `cd MatterSurfaceLib && WSL_LINUX=1 make`. Headless tests: `cd MatterSurfaceLib/tests && make run-cont run-cell run-cull`.

---

## Background facts (verified against the code)

- Additive field: `CalculateScalarAndMaterial` (`src/surface.c:875`) — smooth-min via log-sum-exp; hard-union early-out at `src/surface.c:909`.
- Existing subtraction primitive: `ApplyClipField` (`src/surface.c:856`) = hard `max(scalar, -fO)` against a separate `clipParticles` array. Normals mirror at `src/surface.c:303-337`.
- Sign convention: `scalarValue` signed distance, NEGATIVE = inside; clip/carve only ever RAISE it (push surface inward).
- Cross-cell continuity: `Cluster::update_cell_meshes` (`src/cluster.cpp:295-301`) assigns each particle to every cell its sphere overlaps via `intersects_sphere`. Carve must use the same halo.
- Public API touched: `GenerateMesh`, `GenerateMeshWithConfig`, `ComputeSurfaceNormals` (`include/surface.h:49,52,66`). Test mirrors at `tests/mesh_continuity_tests.cpp:55-56`.
- Noise toolkit in `src/particle_culling.cpp`: `lattice_vhash`, `lattice_vnoise` (declared in header), `fbm3` (file-static).
- Carve generation seeds from the already-emitted surface particles (post interior-culling). Lumpiness lives in `make_sub_particle` (`src/particle_culling.cpp:76`).

**New env knobs (all default off / neutral; tuned visually in Task 8):** `MSL_CARVE_AMT`, `MSL_CARVE_FREQ`, `MSL_CARVE_RADIUS`, `MSL_CARVE_RIDGE`, `MSL_CARVE_BLEND`, `MSL_LUMP_AMT`, `MSL_LUMP_FREQ`.

---

## File Structure

| File | Responsibility / change |
|---|---|
| `include/surface.h` | Append `Particle* carveParticles, int carveCount, float carveBlend` to `GenerateMesh`, `GenerateMeshWithConfig`, `ComputeSurfaceNormals`. |
| `src/surface.c` | New `ApplySubtractField` helper (smooth-max carve); carve in scalar path, hard-union path, and `ComputeSurfaceNormals` gradient; thread params through `GenerateMeshInternal`, `CalculateScalarAndMaterial`. |
| `include/particle_culling.h` | `#include "particle.h"`; new `CarveParams` struct; `generate_carve_particles` decl; new `lump_amt`/`lump_freq` fields on `CullParams`. |
| `src/particle_culling.cpp` | Implement `generate_carve_particles`; lumpiness radius mod in `make_sub_particle`. |
| `include/cluster.h` | `#include "particle.h"`; `carve_particles_` member; `set_carve_particles`. |
| `src/cluster.cpp` | Per-cell carve halo gather in `update_cell_meshes`; pass to `rebuild_meshes`. |
| `include/cell.h` / `src/cell.cpp` | Carve params on `rebuild_meshes` + `generate_mesh_for_group`; compute `carve_blend`; pass to the two surface calls. |
| `main.cpp` | Read carve/lump env knobs; build seeds; `generate_carve_particles`; `set_carve_particles`. |
| `tests/mesh_continuity_tests.cpp` | Update prototype mirrors + all call sites; new carve scalar/hard/normal tests. |
| `tests/cell_bounds_tests.cpp` | Update `GenerateMesh` call sites (add `NULL,0,0.0f`). |
| `tests/particle_culling_tests.cpp` | New `generate_carve_particles` + lumpiness unit tests. |

---

### Task 1: Scalar smooth-subtraction field + signature threading

Threads the three carve params everywhere and implements the smooth-max carve in the scalar field. No-op (byte-identical) when `carveCount == 0`.

**Files:**
- Modify: `include/surface.h:49,52,66`
- Modify: `src/surface.c` (signatures at 199,205,214,364,875; helper before 875; calls at 444,815,913,928)
- Modify: `src/cell.cpp:135-136,148,312-313,391-392,409-410`
- Modify: `include/cell.h:75-77,102-103`
- Modify: `src/cluster.cpp:305-306`
- Modify: `tests/mesh_continuity_tests.cpp:55-56` and call sites `572,583,655,674,675,710`
- Modify: `tests/cell_bounds_tests.cpp:183,184`
- Test: `tests/mesh_continuity_tests.cpp` (new carve scalar test)

- [ ] **Step 1: Write the failing test** — append to `tests/mesh_continuity_tests.cpp` before its `main()` (model it on the existing clip carve test near line 674). It calls `GenerateMesh` with the new carve args, so it will not compile until signatures change.

```cpp
// A subtractive carve particle straddling a sphere's +x surface must pull the
// meshed surface inward there (smooth-CSG subtraction), while carveCount==0
// leaves the mesh unchanged.
static int test_carve_scalar_subtracts() {
    printf("--- carve particle subtracts from the scalar field ---\n");
    Bounds b; b.center = (Vector3){0,0,0}; b.size = (Vector3){4,4,4}; b.divisionPow = 5;
    Particle g[1]; g[0].position = (Vector3){0,0,0}; g[0].radius = 1.0f; g[0].materialId = 1;
    // Carve sphere centered on the +x surface of g[0].
    Particle carve[1]; carve[0].position = (Vector3){1.0f,0,0}; carve[0].radius = 0.5f; carve[0].materialId = 0;
    float blend = 0.05f;

    Mesh open   = GenerateMesh(g, 1.0f, 1, b, blend, NULL, 0, NULL, 0, 0.0f);
    Mesh carved = GenerateMesh(g, 1.0f, 1, b, blend, NULL, 0, carve, 1, blend);

    float openMaxX = -1e9f, carvedMaxX = -1e9f;
    for (int i = 0; i < open.vertexCount; i++)   if (open.vertices[i*3] > openMaxX)   openMaxX = open.vertices[i*3];
    for (int i = 0; i < carved.vertexCount; i++) if (carved.vertices[i*3] > carvedMaxX) carvedMaxX = carved.vertices[i*3];
    printf("  open maxX=%.3f  carved maxX=%.3f\n", openMaxX, carvedMaxX);

    int ok = (carved.vertexCount > 0) && (carvedMaxX < openMaxX - 0.1f);
    if (!ok) printf("  FAIL: carve did not pull the +x surface inward\n");

    // carveCount==0 must reproduce the uncarved mesh exactly.
    Mesh noop = GenerateMesh(g, 1.0f, 1, b, blend, NULL, 0, NULL, 0, blend);
    int identical = (noop.vertexCount == open.vertexCount);
    if (!identical) printf("  FAIL: carveCount==0 changed the mesh (%d vs %d)\n", noop.vertexCount, open.vertexCount);

    UnloadMesh(open); UnloadMesh(carved); UnloadMesh(noop);
    return ok && identical;
}
```

Register it in `main()` of that file alongside the other test calls (find the existing `test_*` invocation list and add `all_ok &= test_carve_scalar_subtracts();` following the file's existing aggregation pattern).

- [ ] **Step 2: Run test to verify it fails (compile error)**

Run: `cd MatterSurfaceLib/tests && make run-cont`
Expected: FAIL — compile error, `GenerateMesh` called with 10 args but declared with 7.

- [ ] **Step 3: Update public signatures in `include/surface.h`**

Append `, Particle* carveParticles, int carveCount, float carveBlend` to the three declarations (lines 49, 52, 66). Add to the doc comment above `GenerateMesh` (after the clip paragraph):

```c
// carveParticles/carveCount are SUBTRACTIVE particles smooth-CSG subtracted from
// the union (smooth-max against -(|p-c|-r)); carveBlend is the carve fillet width
// k_c (carveBlend<=0 => hard subtraction). Pass NULL,0,0 for no carving
// (byte-identical to the uncarved path).
```

- [ ] **Step 4: Add the `ApplySubtractField` helper in `src/surface.c`** immediately before `CalculateScalarAndMaterial` (line 875):

```c
// Smooth-CSG subtraction: carve inward where a subtractive particle is near.
// f_carved = smooth_max(f_add, -(|p-c_j|-r_j)) via log-sum-exp with a max-shift
// for numerical stability. k_c<=0 collapses to the hard max. materialId is left
// untouched (a divot exposes the surrounding material). No-op when carveCount==0,
// so the uncarved path is byte-identical.
static inline void ApplySubtractField(ScalarMaterialPair* result, Vector3 position,
                                      Particle* carveParticles, int carveCount, float k_c) {
    if (!carveParticles || carveCount <= 0) return;
    float f_add = result->scalarValue;
    if (k_c <= 1e-5f) {
        float best = f_add;
        for (int j = 0; j < carveCount; ++j) {
            float dx = position.x - carveParticles[j].position.x;
            float dy = position.y - carveParticles[j].position.y;
            float dz = position.z - carveParticles[j].position.z;
            float v = -(sqrtf(dx*dx + dy*dy + dz*dz) - carveParticles[j].radius);
            if (v > best) best = v;
        }
        result->scalarValue = best;
        return;
    }
    float m = f_add;
    for (int j = 0; j < carveCount; ++j) {
        float dx = position.x - carveParticles[j].position.x;
        float dy = position.y - carveParticles[j].position.y;
        float dz = position.z - carveParticles[j].position.z;
        float v = -(sqrtf(dx*dx + dy*dy + dz*dz) - carveParticles[j].radius);
        if (v > m) m = v;
    }
    float sum = expf((f_add - m) / k_c);
    for (int j = 0; j < carveCount; ++j) {
        float dx = position.x - carveParticles[j].position.x;
        float dy = position.y - carveParticles[j].position.y;
        float dz = position.z - carveParticles[j].position.z;
        float v = -(sqrtf(dx*dx + dy*dy + dz*dz) - carveParticles[j].radius);
        sum += expf((v - m) / k_c);
    }
    result->scalarValue = m + k_c * logf(sum);
}
```

- [ ] **Step 5: Thread carve params through `surface.c` signatures and forwards.**

Add `, Particle* carveParticles, int carveCount, float carveBlend` to: `GenerateMesh` (199), `GenerateMeshWithConfig` (205), `GenerateMeshInternal` decl (182) + def (364), `ComputeSurfaceNormals` (214), and the forward-decl of `CalculateScalarAndMaterial` (179) + its def (875). Update the internal calls:
- `GenerateMesh` (201) and `GenerateMeshWithConfig` (206) forward `..., clipParticles, clipCount, carveParticles, carveCount, carveBlend` to `GenerateMeshInternal`.
- `GenerateMeshInternal` calls `CalculateScalarAndMaterial` (444): append `, carveParticles, carveCount, carveBlend`.
- `GenerateMeshInternal` calls `ComputeSurfaceNormals` (815): append `, carveParticles, carveCount, carveBlend`.

- [ ] **Step 6: Apply carve in the scalar paths of `CalculateScalarAndMaterial`.**

In the hard-union early-out (before line 913 `ApplyClipField`) and in the smooth path (before line 928 `ApplyClipField`), insert:

```c
    ApplySubtractField(&result, position, carveParticles, carveCount, carveBlend);
```

(Carve before clip in both branches.)

- [ ] **Step 7: Make `ComputeSurfaceNormals` accept (but not yet use) carve params.** It already has the params from Step 5; the gradient handling lands in Task 3. No body change here.

- [ ] **Step 8: Thread carve through `cell.cpp` / `cell.h`.**

`include/cell.h`: add `, const Particle* carveParticles = nullptr, int carveCount = 0` to `rebuild_meshes` (75-77) and add `const Particle* carveParticles, int carveCount` params to the private `generate_mesh_for_group` (102-103).

`src/cell.cpp`:
- `rebuild_meshes` (135-136): add the two params; forward to `generate_mesh_for_group` (148): `generate_mesh_for_group(group_id, cluster_particles, blas_manager, simplification_ratio, base_detail, max_pow, uniform_detail, carveParticles, carveCount);`
- `generate_mesh_for_group` (312-313): add the two params. After `blend_width` is computed (345), add:

```cpp
    float carve_blend = blend_width;
    if (const char* e = getenv("MSL_CARVE_BLEND")) { float v = (float)atof(e); if (v > 0.0f) carve_blend = v; }
```
- `GenerateMesh` call (391-392): append `, carveParticles, carveCount, carve_blend`.
- `ComputeSurfaceNormals` call (409-410): append `, carveParticles, carveCount, carve_blend`.

- [ ] **Step 9: Update the `cluster.cpp` call site (pass none yet).**

`src/cluster.cpp:305-306`: leave the `rebuild_meshes` call as-is — the new params default to `nullptr,0`. (Real wiring in Task 6.) Verify it still compiles.

- [ ] **Step 10: Update test prototype mirrors and call sites.**

`tests/mesh_continuity_tests.cpp:55-56`: append `, Particle* carveParticles, int carveCount, float carveBlend` to both prototypes. Update existing calls:
- `572`: `GenerateMesh(sp.data(), pr, (int)sp.size(), bounds, 0.0f, NULL, 0, NULL, 0, 0.0f);`
- `583`: `ComputeSurfaceNormals(&simp, sp.data(), pr, (int)sp.size(), 0.0f, NULL, 0, NULL, 0, 0.0f);`
- `655`: `GenerateMesh(ps, 0.8f, 2, b, 0.0f, NULL, 0, NULL, 0, 0.0f);`
- `674`: `GenerateMesh(g, 1.0f, 1, b, 0.0f, NULL, 0, NULL, 0, 0.0f);`
- `675`: `GenerateMesh(g, 1.0f, 1, b, 0.0f, clip, 1, NULL, 0, 0.0f);`
- `710`: `GenerateMesh(g, 1.0f, 2, b, blendWidth, clip, 1, NULL, 0, 0.0f);`

`tests/cell_bounds_tests.cpp`:
- `183`: `GenerateMesh(g, 1.0f, 1, b, blendWidth, NULL, 0, NULL, 0, 0.0f);`
- `184`: append `, NULL, 0, 0.0f` to the clipped `GenerateMesh(... )` call.

- [ ] **Step 11: Run the new test (expect pass) and the regression suites.**

Run: `cd MatterSurfaceLib/tests && make run-cont && make run-cell`
Expected: `test_carve_scalar_subtracts` PASS (carved maxX < open maxX, carveCount==0 identical); all existing continuity and cell-bounds tests still PASS.

- [ ] **Step 12: Build the app to confirm the full chain compiles.**

Run: `cd MatterSurfaceLib && WSL_LINUX=1 make`
Expected: builds clean.

- [ ] **Step 13: Commit**

```bash
git add MatterSurfaceLib/include/surface.h MatterSurfaceLib/src/surface.c \
        MatterSurfaceLib/include/cell.h MatterSurfaceLib/src/cell.cpp \
        MatterSurfaceLib/src/cluster.cpp \
        MatterSurfaceLib/tests/mesh_continuity_tests.cpp MatterSurfaceLib/tests/cell_bounds_tests.cpp
git commit -m "feat: smooth-CSG subtractive carve particles in the scalar field"
```

---

### Task 2: Carve in the hard-union early-out path

`ApplySubtractField` already handles `k_c<=0` (hard max). Task 1 calls it in the hard path too, so this task only adds an explicit regression test for `blendWidth==0`.

**Files:**
- Test: `tests/mesh_continuity_tests.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// With blendWidth==0 (hard union) and carveBlend==0 (hard subtraction), a carve
// particle must still pull the +x surface inward (plain max(f, -s)).
static int test_carve_hard_path() {
    printf("--- carve in the hard-union path (blend=0) ---\n");
    Bounds b; b.center = (Vector3){0,0,0}; b.size = (Vector3){4,4,4}; b.divisionPow = 5;
    Particle g[1]; g[0].position = (Vector3){0,0,0}; g[0].radius = 1.0f; g[0].materialId = 1;
    Particle carve[1]; carve[0].position = (Vector3){1.0f,0,0}; carve[0].radius = 0.5f; carve[0].materialId = 0;

    Mesh open   = GenerateMesh(g, 1.0f, 1, b, 0.0f, NULL, 0, NULL, 0, 0.0f);
    Mesh carved = GenerateMesh(g, 1.0f, 1, b, 0.0f, NULL, 0, carve, 1, 0.0f);
    float omx=-1e9f, cmx=-1e9f;
    for (int i=0;i<open.vertexCount;i++)   if (open.vertices[i*3]>omx)   omx=open.vertices[i*3];
    for (int i=0;i<carved.vertexCount;i++) if (carved.vertices[i*3]>cmx) cmx=carved.vertices[i*3];
    int ok = (carved.vertexCount>0) && (cmx < omx - 0.1f);
    if (!ok) printf("  FAIL: hard-path carve did not pull surface inward (%.3f vs %.3f)\n", cmx, omx);
    UnloadMesh(open); UnloadMesh(carved);
    return ok;
}
```
Register it in `main()`.

- [ ] **Step 2: Run to verify** — Run: `cd MatterSurfaceLib/tests && make run-cont`. Expected: PASS (logic already present from Task 1).

- [ ] **Step 3: Commit**

```bash
git add MatterSurfaceLib/tests/mesh_continuity_tests.cpp
git commit -m "test: carve in the hard-union early-out path"
```

---

### Task 3: Carve gradient in `ComputeSurfaceNormals`

Make recomputed normals match the carved surface so divot walls shade inward.

**Files:**
- Modify: `src/surface.c:253-337` (the per-vertex gradient block in `ComputeSurfaceNormals`)
- Test: `tests/mesh_continuity_tests.cpp`

- [ ] **Step 1: Write the failing test**

```cpp
// On a carved divot wall, the recomputed normal must tilt toward the carve
// center's INWARD direction (-x at a +x divot), not stay the sphere's outward +x.
static int test_carve_normals_inward() {
    printf("--- carve normals tilt inward on the divot wall ---\n");
    Bounds b; b.center=(Vector3){0,0,0}; b.size=(Vector3){4,4,4}; b.divisionPow=5;
    Particle g[1]; g[0].position=(Vector3){0,0,0}; g[0].radius=1.0f; g[0].materialId=1;
    Particle carve[1]; carve[0].position=(Vector3){1.0f,0,0}; carve[0].radius=0.5f; carve[0].materialId=0;
    float blend=0.05f;
    Mesh m = GenerateMesh(g, 1.0f, 1, b, blend, NULL, 0, carve, 1, blend);
    // GenerateMesh runs ComputeSurfaceNormals internally with the same carve args.
    // Find the vertex nearest the carve center and check its normal.x is reduced
    // (tilted inward) vs a pure sphere normal (which would be ~+1 there).
    int best=-1; float bestd=1e9f;
    for (int i=0;i<m.vertexCount;i++){
        float dx=m.vertices[i*3]-1.0f, dy=m.vertices[i*3+1], dz=m.vertices[i*3+2];
        float d=dx*dx+dy*dy+dz*dz; if(d<bestd){bestd=d;best=i;}
    }
    int ok = (best>=0) && (m.normals[best*3+0] < 0.5f);
    if(!ok) printf("  FAIL: divot-wall normal.x=%.3f not tilted inward\n", best>=0?m.normals[best*3]:9.9f);
    UnloadMesh(m);
    return ok;
}
```
Register it in `main()`.

- [ ] **Step 2: Run to verify it fails** — Run: `cd MatterSurfaceLib/tests && make run-cont`. Expected: FAIL — normals ignore carve, `normal.x` ~ +1.

- [ ] **Step 3: Implement the carve gradient.** In `ComputeSurfaceNormals`, after the additive gradient is computed and normalized (after line 287, before the clip block at 289), insert a carve block. It needs the additive blended field value `fieldValue`; compute it here if carve is active, then smooth-max-blend the gradient:

```c
        // Carve gradient: blend the additive gradient with the inward directions
        // of nearby subtractive particles, weighted by exp((v_j - m)/k_c), to
        // match ApplySubtractField. Updates the running field value so the clip
        // override below compares against the carved field. No-op when carveCount==0.
        if (carveParticles && carveCount > 0 && haveGrad) {
            float f_add = fmin;
            if (blendWidth > 1e-5f && found > 1) {
                float k = blendWidth, sumf = 0.0f;
                for (int j = 0; j < found; j++) sumf += expf(-(fj[j] - fmin) / k);
                f_add = fmin - k * logf(sumf);
            }
            float k_c = (carveBlend > 1e-5f) ? carveBlend : 1e-5f;
            // max term for stability
            float m = f_add;
            for (int c = 0; c < carveCount; ++c) {
                float dx = vx - carveParticles[c].position.x;
                float dy = vy - carveParticles[c].position.y;
                float dz = vz - carveParticles[c].position.z;
                float v = -(sqrtf(dx*dx+dy*dy+dz*dz) - carveParticles[c].radius);
                if (v > m) m = v;
            }
            float wadd = expf((f_add - m) / k_c);
            float ax = gx*wadd, ay = gy*wadd, az = gz*wadd, wsum = wadd;
            for (int c = 0; c < carveCount; ++c) {
                float dx = vx - carveParticles[c].position.x;
                float dy = vy - carveParticles[c].position.y;
                float dz = vz - carveParticles[c].position.z;
                float dist = sqrtf(dx*dx+dy*dy+dz*dz);
                if (dist < 1e-6f) continue;
                float v = -(dist - carveParticles[c].radius);
                float w = expf((v - m) / k_c);
                float inv = 1.0f / dist;          // grad(-s) = -unit(v - c)
                ax += w * (-dx*inv); ay += w * (-dy*inv); az += w * (-dz*inv);
                wsum += w;
            }
            if (wsum > 1e-12f) {
                gx = ax/wsum; gy = ay/wsum; gz = az/wsum;
                float gl = sqrtf(gx*gx+gy*gy+gz*gz);
                if (gl > 1e-12f) { float n=1.0f/gl; gx*=n; gy*=n; gz*=n; }
            }
        }
```

(The subsequent clip block at 289-337 is unchanged; it still fires on `-fO > fieldValue` using the additive `fieldValue` it computes locally. Carve and clip both only push outward, so applying carve to the gradient first is consistent.)

- [ ] **Step 4: Run to verify it passes** — Run: `cd MatterSurfaceLib/tests && make run-cont`. Expected: `test_carve_normals_inward` PASS; all prior tests still PASS.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/surface.c MatterSurfaceLib/tests/mesh_continuity_tests.cpp
git commit -m "feat: carve-aware gradient in ComputeSurfaceNormals"
```

---

### Task 4: `generate_carve_particles` + `CarveParams`

Noise-driven carve seeding from surface particles.

**Files:**
- Modify: `include/particle_culling.h`
- Modify: `src/particle_culling.cpp`
- Test: `tests/particle_culling_tests.cpp`

- [ ] **Step 1: Write the failing test** — append to `tests/particle_culling_tests.cpp` (and register in its `main()`):

```cpp
static int test_generate_carve_particles() {
    printf("--- generate_carve_particles: determinism + threshold ---\n");
    std::vector<Particle> seeds;
    for (int i = 0; i < 1000; ++i) {
        Particle p; p.position = (Vector3){ i*0.31f, (i%7)*0.4f, (i%13)*0.5f };
        p.radius = 0.31f; p.materialId = 8; seeds.push_back(p);
    }
    CarveParams off{}; off.amt = 0.0f;
    if (!generate_carve_particles(seeds, off).empty()) { printf("  FAIL: amt=0 must emit nothing\n"); return 0; }

    CarveParams cp{}; cp.amt = 0.4f; cp.freq = 0.5f; cp.base_radius = 0.2f;
    cp.ridge = 0.0f; cp.r_max = 0.25f; cp.seed = 1337;
    auto a = generate_carve_particles(seeds, cp);
    auto b = generate_carve_particles(seeds, cp);
    int ok = (!a.empty()) && (a.size() == b.size());
    for (size_t i = 0; ok && i < a.size(); ++i)
        if (a[i].position.x != b[i].position.x || a[i].radius != b[i].radius) ok = 0;
    for (const auto& c : a) if (c.radius > cp.r_max + 1e-6f) { ok = 0; printf("  FAIL: r > r_max\n"); break; }
    CarveParams more = cp; more.amt = 0.8f;
    if (generate_carve_particles(seeds, more).size() < a.size()) { ok = 0; printf("  FAIL: higher amt should not reduce count\n"); }
    if (!ok) printf("  FAIL: determinism/threshold check\n"); else printf("  OK (%zu carves)\n", a.size());
    return ok;
}
```

- [ ] **Step 2: Run to verify it fails** — Run: `cd MatterSurfaceLib/tests && make run-cull`. Expected: FAIL — `CarveParams`/`generate_carve_particles` undeclared.

- [ ] **Step 3: Declare API in `include/particle_culling.h`.** Add near the top `#include "particle.h"`, and after the `EmittedParticle`/`CullParams` structs:

```cpp
// Subtractive carve-particle generation. Seeded from surface particles; where a
// blended blob/ridge noise field exceeds (1 - amt), emit a negative particle
// whose radius scales with the overshoot (capped at r_max for watertightness).
struct CarveParams {
    float amt = 0.0f;          // 0 = off; threshold = 1 - amt
    float freq = 0.0f;         // carve noise frequency (feature spacing)
    float base_radius = 0.0f;  // base divot radius
    float ridge = 0.0f;        // 0 = round divots, 1 = linear crevices
    float r_max = 0.0f;        // watertight cap on carve radius (<=0 = uncapped)
    uint32_t seed = 0;
};

std::vector<Particle> generate_carve_particles(const std::vector<Particle>& seeds,
                                               const CarveParams& cp);
```

Also add to `CullParams` (after `radius_cluster_freq`):
```cpp
    float lump_amt = 0.0f;  // low-freq additive radius modulation (0 = off)
    float lump_freq = 0.0f; // lumpiness noise frequency (low = coarse bulges)
```

- [ ] **Step 4: Implement `generate_carve_particles` in `src/particle_culling.cpp`** (after `emit_all`):

```cpp
std::vector<Particle> generate_carve_particles(const std::vector<Particle>& seeds,
                                               const CarveParams& cp) {
    std::vector<Particle> out;
    if (cp.amt <= 0.0f) return out;
    float threshold = 1.0f - cp.amt;
    float s = (float)cp.seed;
    for (const Particle& seed : seeds) {
        float x = seed.position.x, y = seed.position.y, z = seed.position.z;
        float blob  = fbm3(x*cp.freq + s, y*cp.freq, z*cp.freq);
        float ridge = 1.0f - fabsf(2.0f*fbm3(x*cp.freq + 97.0f + s, y*cp.freq, z*cp.freq) - 1.0f);
        float n = blob + (ridge - blob) * cp.ridge;
        if (n <= threshold) continue;
        float over = (threshold < 1.0f) ? (n - threshold) / (1.0f - threshold) : 1.0f;
        float r = cp.base_radius * (0.5f + over);
        if (cp.r_max > 0.0f && r > cp.r_max) r = cp.r_max;
        Particle c; c.position = seed.position; c.radius = r; c.materialId = 0;
        out.push_back(c);
    }
    return out;
}
```

- [ ] **Step 5: Run to verify it passes** — Run: `cd MatterSurfaceLib/tests && make run-cull`. Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/particle_culling.h MatterSurfaceLib/src/particle_culling.cpp MatterSurfaceLib/tests/particle_culling_tests.cpp
git commit -m "feat: generate_carve_particles + CarveParams"
```

---

### Task 5: Lumpiness radius modulation in `make_sub_particle`

**Files:**
- Modify: `src/particle_culling.cpp:107` (after `ep.radius` assignment)
- Test: `tests/particle_culling_tests.cpp`

- [ ] **Step 1: Write the failing test** (register in `main()`):

```cpp
static int test_lumpiness_modulates_radius() {
    printf("--- lumpiness modulates additive radius ---\n");
    GridLattice lat(0.8f); Occupancy occ;
    for (int x=0;x<6;x++) for (int y=0;y<6;y++) for (int z=0;z<6;z++)
        occ.set(SlotCoord{x,y,z}, SlotData{8});
    CullParams p{}; p.margin=1; p.base_radius=0.62f; p.jitter_amount=0.0f;
    p.tint_alpha=0.0f; p.seed=1337; p.cell_size=2.4f; p.max_tier=0; p.spacing=0.8f;
    p.cell_origin_offset=(Vector3){0,0,0};
    auto base = emit_all(lat, occ, p);
    p.lump_amt = 0.5f; p.lump_freq = 0.3f;
    auto lumped = emit_all(lat, occ, p);
    int differ = 0; float maxrel = 0.0f;
    for (size_t i=0;i<base.size() && i<lumped.size();++i) {
        float rel = fabsf(lumped[i].radius - base[i].radius) / base[i].radius;
        if (rel > 1e-4f) differ++;
        if (rel > maxrel) maxrel = rel;
    }
    int ok = (differ > 0) && (maxrel <= 0.5f + 1e-3f);
    if (!ok) printf("  FAIL: differ=%d maxrel=%.3f\n", differ, maxrel);
    return ok;
}
```

- [ ] **Step 2: Run to verify it fails** — Run: `cd MatterSurfaceLib/tests && make run-cull`. Expected: FAIL — radii unchanged (`differ==0`).

- [ ] **Step 3: Implement** — in `make_sub_particle`, immediately after line 107 (`ep.radius = (p.base_radius * inv) * (1.0f + rv * p.radius_variation);`):

```cpp
    if (p.lump_amt > 0.0f) {
        float ln = lattice_vnoise(cfx * p.lump_freq, cfy * p.lump_freq, cfz * p.lump_freq);
        ep.radius *= 1.0f + p.lump_amt * (2.0f * ln - 1.0f);
    }
```

- [ ] **Step 4: Run to verify it passes** — Run: `cd MatterSurfaceLib/tests && make run-cull`. Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/src/particle_culling.cpp MatterSurfaceLib/tests/particle_culling_tests.cpp
git commit -m "feat: low-frequency lumpiness radius modulation"
```

---

### Task 6: Cluster carve storage + per-cell halo distribution

**Files:**
- Modify: `include/cluster.h`
- Modify: `src/cluster.cpp:288-312` (`update_cell_meshes`)

- [ ] **Step 1: Add storage + setter to `include/cluster.h`.** Add near the top `#include "particle.h"`. In the public section (near `set_no_mesh_cells`):

```cpp
    // Subtractive carve particles (smooth-CSG). Distributed per-cell by the same
    // intersects_sphere halo as additive particles so the carved field stays
    // continuous across cell boundaries.
    void set_carve_particles(const std::vector<Particle>& carve) { carve_particles_ = carve; }
    void clear_carve_particles() { carve_particles_.clear(); }
```

In the private members (near `no_mesh_cells_`):
```cpp
    std::vector<Particle> carve_particles_;
```

- [ ] **Step 2: Distribute carve per cell in `src/cluster.cpp` `update_cell_meshes`.** Replace the `rebuild_meshes` call (305-306) with:

```cpp
    if (!cell->material_particle_indices.empty()) {
        // Gather carve particles whose influence overlaps this cell, mirroring the
        // additive intersects_sphere halo (slack covers the carve fillet reach) so
        // shared-face field values match and no seam cracks.
        std::vector<Particle> cell_carve;
        for (const Particle& cpart : carve_particles_) {
            if (cell->intersects_sphere(cpart.position, cpart.radius * 1.5f))
                cell_carve.push_back(cpart);
        }
        const Particle* carvePtr = cell_carve.empty() ? nullptr : cell_carve.data();
        int carveCount = static_cast<int>(cell_carve.size());
        cell->rebuild_meshes(particles_, blas_manager_, simplification_ratio_,
                             base_detail_size_, max_division_pow_, uniform_detail,
                             carvePtr, carveCount);
    } else {
        cell->clear_meshes(&blas_manager_);
    }
```

- [ ] **Step 3: Build to verify it compiles** — Run: `cd MatterSurfaceLib && WSL_LINUX=1 make`. Expected: builds clean.

- [ ] **Step 4: Run headless suites to confirm no regression** — Run: `cd MatterSurfaceLib/tests && make run-cont && make run-cell && make run-cull`. Expected: all PASS (carve list empty until Task 7, so meshes unchanged).

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/cluster.h MatterSurfaceLib/src/cluster.cpp
git commit -m "feat: cluster carve-particle storage + per-cell halo distribution"
```

---

### Task 7: Scene wiring in `main.cpp`

Read knobs, generate carve particles from the emitted surface particles, hand them to the cluster, and wire lumpiness into `CullParams`.

**Files:**
- Modify: `main.cpp:setup_lattice_scene` (566-696)

- [ ] **Step 1: Add env knob reads** near the other knob reads (after line 595):

```cpp
        float CARVE_AMT = 0.0f, CARVE_FREQ = 0.6f, CARVE_RADIUS = 0.16f, CARVE_RIDGE = 0.4f;
        float LUMP_AMT = 0.0f, LUMP_FREQ = 0.35f;
        if (const char* e = getenv("MSL_CARVE_AMT"))    { float v=(float)atof(e); if (v>=0.0f) CARVE_AMT=v; }
        if (const char* e = getenv("MSL_CARVE_FREQ"))   { float v=(float)atof(e); if (v>0.0f)  CARVE_FREQ=v; }
        if (const char* e = getenv("MSL_CARVE_RADIUS")) { float v=(float)atof(e); if (v>0.0f)  CARVE_RADIUS=v; }
        if (const char* e = getenv("MSL_CARVE_RIDGE"))  { float v=(float)atof(e); if (v>=0.0f) CARVE_RIDGE=v; }
        if (const char* e = getenv("MSL_LUMP_AMT"))     { float v=(float)atof(e); if (v>=0.0f) LUMP_AMT=v; }
        if (const char* e = getenv("MSL_LUMP_FREQ"))    { float v=(float)atof(e); if (v>0.0f)  LUMP_FREQ=v; }
```

- [ ] **Step 2: Wire lumpiness into `CullParams`** — after line 662 (`p.spacing = SPACING;`):

```cpp
        p.lump_amt = LUMP_AMT;
        p.lump_freq = LUMP_FREQ;
```

- [ ] **Step 3: Generate + register carve particles.** The add-particle loop (670-673) already produces recentered local positions. Accumulate seeds there and generate carve after the loop:

Change the loop body to also collect a seed, then after the loop add the carve generation. Replace lines 670-673 with:

```cpp
        std::vector<Particle> carve_seeds;
        carve_seeds.reserve(emitted.size());
        for (auto& ep : emitted) {
            Vector3 pos = { ep.position.x - halfx, ep.position.y - halfy, ep.position.z - halfz };
            test_cluster_->add_particle(pos, ep.radius, ep.materialId, ep.tint, ep.detail_size);
            Particle sp; sp.position = pos; sp.radius = ep.radius; sp.materialId = (int)ep.materialId;
            carve_seeds.push_back(sp);
        }

        CarveParams cv;
        cv.amt = CARVE_AMT; cv.freq = CARVE_FREQ; cv.base_radius = CARVE_RADIUS;
        cv.ridge = CARVE_RIDGE; cv.r_max = CARVE_RADIUS * 1.5f; cv.seed = 4242;
        std::vector<Particle> carve = generate_carve_particles(carve_seeds, cv);
        test_cluster_->set_carve_particles(carve);
        printf("[carve] amt=%.2f freq=%.2f radius=%.2f ridge=%.2f -> %zu carve particles\n",
               CARVE_AMT, CARVE_FREQ, CARVE_RADIUS, CARVE_RIDGE, carve.size());
```

(Confirm `main.cpp` includes `particle_culling.h`; it uses `CullParams`/`emit_all` already, so it does.)

- [ ] **Step 4: Build** — Run: `cd MatterSurfaceLib && WSL_LINUX=1 make`. Expected: builds clean.

- [ ] **Step 5: Smoke-run with carve on (headless capture).** Confirm the `[carve]` line reports a nonzero count and the app runs without crashing:

Run: `cd MatterSurfaceLib && MSL_CARVE_AMT=0.4 MSL_LUMP_AMT=0.25 MSL_FRAMES=2 MSL_RENDER_MODE=4 ./build/linux/matter_surface_lib`
Expected: prints `[carve] ... -> N carve particles` with N > 0; exits cleanly.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "feat: wire carve + lumpiness knobs into the lattice scene"
```

---

### Task 8: Full verification + visual acceptance + default tuning

**Files:**
- Modify: `main.cpp` (finalize default knob values once tuned)

- [ ] **Step 1: Run the entire headless suite**

Run: `cd MatterSurfaceLib/tests && make run-cont && make run-cell && make run-cull && make run-simp`
Expected: all suites PASS.

- [ ] **Step 2: Build-all from repo root**

Run: `cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && ./build-all.sh test`
Expected: every project builds; headless suites pass.

- [ ] **Step 3: Visual acceptance (user-driven).** Render the lattice scene and confirm the cinderblock character. Because GUI launch is reaped by the harness, ask the user to run it themselves (per memory: launch GUI via their own terminal / `!`):

```
cd MatterSurfaceLib && MSL_CARVE_AMT=0.4 MSL_CARVE_RIDGE=0.2 MSL_LUMP_AMT=0.25 ./build/linux/matter_surface_lib   # round divots + lumps
cd MatterSurfaceLib && MSL_CARVE_AMT=0.5 MSL_CARVE_RIDGE=0.9 ./build/linux/matter_surface_lib                      # crevices
```
Confirm with the user: divots, crevices (via `MSL_CARVE_RIDGE`), coarse lumps (via `MSL_LUMP_AMT`), concave surfaces, and that the surface stays watertight (no through-holes).

- [ ] **Step 4: Lock in tuned defaults.** Once the user signs off on values, set `CARVE_AMT`/`LUMP_AMT` (and friends) defaults in `main.cpp` to the agreed nonzero values so the organic look ships out of the box.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "chore: tune default carve/lumpiness values for the lattice scene"
```

---

## Self-Review

- **Spec coverage:** divots/crevices via `generate_carve_particles` + `CARVE_RIDGE` (Task 4/7); concavity via smooth subtraction (Task 1-3); lumpiness via radius modulation (Task 5/7); watertight via `r_max` cap (Task 4) + halo distribution (Task 6); knobs (Task 7); tests (Tasks 1-5) + watertight/continuity covered by existing `run-cont` regressions plus the carve scalar/normal tests; visual acceptance (Task 8). All spec sections map to a task.
- **Type consistency:** carve params are uniformly `Particle* carveParticles, int carveCount, float carveBlend` across `surface.h`/`surface.c`/`cell.*`; `const Particle*` in `cell.*`/`cluster.cpp` (C++ side), matching the `extern "C"` non-const C prototypes via implicit const-strip at the call (pass `.data()` of non-const vectors — carve buffers are built locally and non-const, so no const-cast needed). `CarveParams`/`CullParams` field names match between header and uses. `generate_carve_particles(const std::vector<Particle>&, const CarveParams&)` consistent in decl, impl, and test.
- **Placeholder scan:** no TBDs; every code step has complete code. Default knob *values* are intentionally finalized in Task 8 after visual tuning (the mechanism is complete from Task 7).
- **Note for implementer:** `cell.h` gives `rebuild_meshes` carve params defaults (`= nullptr, = 0`) so the Task 1 `cluster.cpp` call compiles before Task 6 wires real values. `generate_mesh_for_group` is private and always called internally, so its carve params need no defaults.
