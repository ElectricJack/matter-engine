# Tiered Surface Lattice + Detail-Driven Mesh Resolution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Emit finer particles near the surface via a subdividable (tiered) lattice and choose each cell's marching-cubes resolution from the finest detail present, after removing the hardcoded multi-LOD system.

**Architecture:** Detail flows one direction: depth-from-surface → tier (computed in the emit path, `particle_culling.cpp`) → `EmittedParticle.detail_size` → `StaticParticle.detail_size` → per-cell `divisionPow` (chosen at mesh time in `cell.cpp`). The base `Occupancy` is unchanged and represents tier 0. The mesher (`surface.c`) is untouched. The LOD-level concept is deleted; mesh simplification stays.

**Tech Stack:** C++17 (g++), C (gcc) for SurfaceLib, raylib `Vector3`/`Vector4` PODs, custom marching-cubes mesher, headless CHECK-macro test suites under `MatterSurfaceLib/tests/`.

**Spec:** `docs/superpowers/specs/2026-06-14-tiered-surface-lattice-design.md`

---

## File Structure

All paths relative to repo root `/mnt/d/Shared With Desktop/AI/matter-engine-cpp`.

| File | Responsibility | Change |
|------|----------------|--------|
| `MatterSurfaceLib/include/cluster.h` | Cluster API + `StaticParticle` | Remove LOD API; add `detail_size`, `base_detail_size_`, `max_division_pow_`, new `add_particle` overload, setters |
| `MatterSurfaceLib/src/cluster.cpp` | Cluster impl | Remove LOD state/methods; single-resolution cells; unconditional skip-mesh; thread base-detail/max-pow into mesh path |
| `MatterSurfaceLib/include/particle_culling.h` | Emit API + types | Add `EmittedParticle.detail_size`, `CullParams.max_tier`/`spacing`, `slot_depth`/`slot_tier` decls |
| `MatterSurfaceLib/src/particle_culling.cpp` | Emit impl | Add `slot_depth`/`slot_tier`; refactor `make_particle` → `make_sub_particle`; tiered emission |
| `MatterSurfaceLib/include/cell.h` | Cell API | Add `choose_division_pow` decl; thread `base_detail`/`max_pow` through `rebuild_meshes`/`generate_mesh_for_group` |
| `MatterSurfaceLib/src/cell.cpp` | Cell impl | Define `choose_division_pow`; use it at the old `divisionPow = 4` site |
| `MatterSurfaceLib/main.cpp` | Scene + UI | Remove LOD UI/knobs; add `MSL_MAX_TIER`/`MSL_MAX_POW`; wire `spacing`/`detail_size`/`base_detail` |
| `MatterSurfaceLib/tests/particle_culling_tests.cpp` | Emit unit tests | Add tier/depth/sub-particle/regression tests |
| `MatterSurfaceLib/tests/cell_bounds_tests.cpp` | Cell unit tests | Add `choose_division_pow` tests |

### Build & test commands (reference)

- Emit tests: `cd "MatterSurfaceLib/tests" && make run-cull`
- Cell tests: `cd "MatterSurfaceLib/tests" && make run-cell`
- Material tests (regression): `cd "MatterSurfaceLib/tests" && make run-reg`
- Continuity tests (regression): `cd "MatterSurfaceLib/tests" && make run-cont`
- Full app build: `cd "MatterSurfaceLib" && WSL_LINUX=1 make`

---

## Task 1: Remove the LOD system from Cluster

**Files:**
- Modify: `MatterSurfaceLib/include/cluster.h`
- Modify: `MatterSurfaceLib/src/cluster.cpp`

This is a deletion/refactor task (no new unit test — verified by build + existing suites). The cluster collapses to a single cell resolution (`smallest_cell_size_`). `simplification_ratio_` stays untouched.

- [ ] **Step 1: Remove LOD members and methods from `cluster.h`**

In `include/cluster.h`, delete these lines from the `// LOD level management` block (currently lines 75–79):

```cpp
    // LOD level management
    void set_lod_level(int lod_level, bool clear_blas = false);
    int get_lod_level() const { return current_lod_level_; }
    float get_current_cell_size() const { return smallest_cell_size_ * (1 << current_lod_level_); }
    void force_rebuild_all_cells();
```

Replace with (keep `force_rebuild_all_cells`, drop the LOD accessors):

```cpp
    // Single-resolution rebuild of every cell (used after a full scene change).
    void force_rebuild_all_cells();
```

Then delete the member declaration (currently line 116):

```cpp
    int current_lod_level_;           // Currently active LOD level (0 = finest detail)
```

Leave `smallest_cell_size_`, `simplification_ratio_`, `set_smallest_cell_size`, `get_smallest_cell_size`, `set_simplification_ratio`, `get_simplification_ratio`, `set_no_mesh_cells`, `clear_no_mesh_cells` intact.

- [ ] **Step 2: Fix the constructor initializer in `cluster.cpp`**

In `src/cluster.cpp` the constructor initializer list (around line 35) ends with `current_lod_level_(0) {`. Remove that initializer. Find the line:

```cpp
      current_lod_level_(0) {
```

and merge the prior initializer's trailing comma into a clean `{`. For example if the prior line is `simplification_ratio_(1.0f),` change the tail so the list ends without `current_lod_level_`. The resulting constructor body opens with `{` and no dangling comma.

- [ ] **Step 3: Replace `get_current_cell_size()` call sites with `smallest_cell_size_`**

In `src/cluster.cpp`, `mark_cells_dirty_around_particle` (line ~143):

```cpp
    float cell_size = get_current_cell_size();
```
→
```cpp
    float cell_size = smallest_cell_size_;
```

In `get_cell_coordinates` (line ~172):

```cpp
    float cell_size = get_current_cell_size();
```
→
```cpp
    float cell_size = smallest_cell_size_;
```

- [ ] **Step 4: Single-resolution cell creation**

In `find_or_create_cell` (line ~197):

```cpp
    auto new_cell = std::make_unique<Cell>(cell_coords, current_lod_level_, smallest_cell_size_);
```
→
```cpp
    auto new_cell = std::make_unique<Cell>(cell_coords, 0, smallest_cell_size_);
```

- [ ] **Step 5: Make the interior skip-mesh unconditional**

In `rebuild_dirty_cells` (lines ~231–240), replace:

```cpp
            uint64_t key = pack_slot(SlotCoord{
                (int)lroundf(cell->coordinates.x),
                (int)lroundf(cell->coordinates.y),
                (int)lroundf(cell->coordinates.z)});
            if (current_lod_level_ == 0 &&
                no_mesh_cells_.find(key) != no_mesh_cells_.end()) {
                cell->clear_meshes(&blas_manager_);  // drop any stale geometry
                cell->is_dirty = false;
                continue;                            // never meshed, no BLAS
            }
```

with:

```cpp
            uint64_t key = pack_slot(SlotCoord{
                (int)lroundf(cell->coordinates.x),
                (int)lroundf(cell->coordinates.y),
                (int)lroundf(cell->coordinates.z)});
            if (no_mesh_cells_.find(key) != no_mesh_cells_.end()) {
                cell->clear_meshes(&blas_manager_);  // interior cell: never meshed
                cell->is_dirty = false;
                continue;
            }
```

- [ ] **Step 6: Delete `set_lod_level`; strip LOD from `force_rebuild_all_cells`**

In `src/cluster.cpp` delete the entire `Cluster::set_lod_level` definition (lines ~398–446).

In `force_rebuild_all_cells` (line ~449) replace:

```cpp
    printf("Cluster %u: Force rebuilding all cells at LOD level %d\n", cluster_id_, current_lod_level_);
```
with:
```cpp
    printf("Cluster %u: Force rebuilding all cells\n", cluster_id_);
```

- [ ] **Step 7: Build the app to verify it still compiles**

Note: `main.cpp` still references removed LOD methods — it is fixed in Task 2. To verify Task 1 in isolation, compile only the cluster object:

Run: `cd "MatterSurfaceLib" && WSL_LINUX=1 make obj/cluster.o`
Expected: compiles with no errors referencing `current_lod_level_`, `set_lod_level`, `get_lod_level`, or `get_current_cell_size`.

(If the object path differs, instead defer the build check to Task 2 Step 4, which builds the whole app.)

- [ ] **Step 8: Commit**

```bash
git add "MatterSurfaceLib/include/cluster.h" "MatterSurfaceLib/src/cluster.cpp"
git commit -m "refactor: collapse Cluster to a single cell resolution, drop LOD levels"
```

---

## Task 2: Remove LOD UI and knobs from main.cpp

**Files:**
- Modify: `MatterSurfaceLib/main.cpp`

Removes the LOD slider, number-key handlers, `MSL_LOD`, and `set_lod_level`/`get_lod_level`/`get_current_cell_size` references. Keeps the simplification UI and `MSL_RATIO`.

- [ ] **Step 1: Remove the `MSL_LOD` capture knob**

Around lines 408–413, delete:

```cpp
        // MSL_LOD lets a capture reproduce a specific LOD level (default scene
        // boots at LOD 1). Switch before the rebuild so the whole scene meshes
        // at the requested level.
        if (const char* lodEnv = getenv("MSL_LOD")) {
            test_cluster_->set_lod_level(atoi(lodEnv), true);
        }
```

- [ ] **Step 2: Remove boot-time LOD calls**

Delete the line at ~512:
```cpp
        test_cluster_->set_lod_level(1);
```
Delete the line at ~634:
```cpp
        test_cluster_->set_lod_level(0);
```
At line ~603, update the stale comment only:
```cpp
        p.cell_size = test_cluster_->get_smallest_cell_size();   // LOD 0 cell size
```
→
```cpp
        p.cell_size = test_cluster_->get_smallest_cell_size();   // single cell size
```

- [ ] **Step 3: Remove the number-key LOD handlers and the ImGui LOD slider**

Delete the entire `PROFILE_SECTION("LOD Controls")` block (lines ~893–962, the `IsKeyPressed` handlers for keys that call `set_lod_level(0..4, true)`).

Delete the ImGui LOD slider block (lines ~1345–1348):
```cpp
        // LOD controls
        int current_lod = test_cluster_->get_lod_level();
        if (ImGui::SliderInt("LOD Level", &current_lod, 0, 4)) {
            test_cluster_->set_lod_level(current_lod, true);
        }
```

Delete the help bullet (line ~1422):
```cpp
        ImGui::BulletText("1-5: Change LOD level");
```

- [ ] **Step 4: Remove LOD from the status print**

Replace lines ~1715–1716:
```cpp
        printf("  - LOD level: %d (%.2f unit cells)\n", 
               test_cluster_->get_lod_level(), test_cluster_->get_current_cell_size());
```
with:
```cpp
        printf("  - cell size: %.2f units\n", test_cluster_->get_smallest_cell_size());
```

- [ ] **Step 5: Build the whole app**

Run: `cd "MatterSurfaceLib" && WSL_LINUX=1 make`
Expected: builds to `MatterSurfaceLib/matter_surface_lib` with no references to `set_lod_level`/`get_lod_level`/`get_current_cell_size`/`MSL_LOD`.

- [ ] **Step 6: Run existing headless suites to confirm no regression**

Run: `cd "MatterSurfaceLib/tests" && make run-cull && make run-cell && make run-reg && make run-cont`
Expected: each prints its "all tests passed" line and exits 0.

- [ ] **Step 7: Commit**

```bash
git add "MatterSurfaceLib/main.cpp"
git commit -m "refactor: remove LOD UI, keys, and MSL_LOD knob from scene"
```

---

## Task 3: `slot_depth` — Chebyshev depth below the surface

**Files:**
- Modify: `MatterSurfaceLib/include/particle_culling.h`
- Modify: `MatterSurfaceLib/src/particle_culling.cpp`
- Test: `MatterSurfaceLib/tests/particle_culling_tests.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/particle_culling_tests.cpp`, add this function (place it after `test_burial`, before `test_cull_counts`):

```cpp
static void test_slot_depth() {
    Occupancy occ = solid_block(5);   // coords 0..4 each axis
    // Center (2,2,2): radius-1 box fully occupied, radius-2 box hits the
    // boundary at coord 0/4 which is still occupied, radius-3 would leave block.
    CHECK(slot_depth(occ, SlotCoord{2,2,2}, 3) == 2, "center of 5^3 has depth 2 (capped scan 3)");
    CHECK(slot_depth(occ, SlotCoord{0,2,2}, 3) == 0, "face slot has depth 0");
    CHECK(slot_depth(occ, SlotCoord{1,2,2}, 3) == 1, "one-layer-in slot has depth 1");
    // Cap: never report more than max_depth even if deeper matter exists.
    CHECK(slot_depth(occ, SlotCoord{2,2,2}, 1) == 1, "depth is capped at max_depth");
    Occupancy single; single.set(SlotCoord{0,0,0}, SlotData{8});
    CHECK(slot_depth(single, SlotCoord{0,0,0}, 3) == 0, "isolated slot has depth 0");
}
```

Register it in `main()` (after `test_burial();`):

```cpp
    test_slot_depth();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make run-cull`
Expected: FAIL to compile — `slot_depth` not declared.

- [ ] **Step 3: Declare `slot_depth` in the header**

In `include/particle_culling.h`, after the `slot_is_buried` declaration (line ~48) add:

```cpp
// Chebyshev depth below the surface: the largest k in [0, max_depth] such that
// every slot within Chebyshev radius k of c is occupied. depth 0 means an
// immediate box-neighbor is empty (outermost shell). Capped at max_depth so the
// scan stays O(max_depth^3). (slot_is_buried(c, m) == slot_depth(c, m) >= m.)
int slot_depth(const Occupancy& occ, SlotCoord c, int max_depth);
```

- [ ] **Step 4: Implement `slot_depth`**

In `src/particle_culling.cpp`, after `slot_is_buried` (line ~46) add:

```cpp
int slot_depth(const Occupancy& occ, SlotCoord c, int max_depth) {
    if (max_depth < 0) max_depth = 0;
    int k = 0;
    for (; k < max_depth; ++k) {
        int r = k + 1;  // does the radius-(k+1) box stay fully occupied?
        bool full = true;
        for (int dz = -r; dz <= r && full; ++dz)
        for (int dy = -r; dy <= r && full; ++dy)
        for (int dx = -r; dx <= r && full; ++dx) {
            if (!occ.occupied(SlotCoord{c.x + dx, c.y + dy, c.z + dz})) full = false;
        }
        if (!full) break;
    }
    return k;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make run-cull`
Expected: PASS — "All particle_culling tests passed".

- [ ] **Step 6: Commit**

```bash
git add "MatterSurfaceLib/include/particle_culling.h" "MatterSurfaceLib/src/particle_culling.cpp" "MatterSurfaceLib/tests/particle_culling_tests.cpp"
git commit -m "feat: add slot_depth (Chebyshev depth below surface)"
```

---

## Task 4: `slot_tier` — map depth to refinement tier

**Files:**
- Modify: `MatterSurfaceLib/include/particle_culling.h`
- Modify: `MatterSurfaceLib/src/particle_culling.cpp`
- Test: `MatterSurfaceLib/tests/particle_culling_tests.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/particle_culling_tests.cpp`, add after `test_slot_depth`:

```cpp
static void test_slot_tier() {
    // tier = max_tier - min(depth, max_tier): outermost shell is finest.
    CHECK(slot_tier(0, 2) == 2, "depth 0 -> finest tier (max_tier)");
    CHECK(slot_tier(1, 2) == 1, "depth 1 -> one coarser");
    CHECK(slot_tier(2, 2) == 0, "depth == max_tier -> tier 0");
    CHECK(slot_tier(5, 2) == 0, "deep interior clamps to tier 0");
    CHECK(slot_tier(0, 0) == 0, "max_tier 0 disables refinement");
}
```

Register in `main()` after `test_slot_depth();`:

```cpp
    test_slot_tier();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make run-cull`
Expected: FAIL to compile — `slot_tier` not declared.

- [ ] **Step 3: Declare `slot_tier` in the header**

In `include/particle_culling.h`, immediately after the `slot_depth` declaration add:

```cpp
// Map a depth to a refinement tier: max_tier - min(depth, max_tier). The
// outermost shell (depth 0) gets the finest tier; depth >= max_tier gets tier 0.
int slot_tier(int depth, int max_tier);
```

- [ ] **Step 4: Implement `slot_tier`**

In `src/particle_culling.cpp`, immediately after `slot_depth` add:

```cpp
int slot_tier(int depth, int max_tier) {
    if (max_tier < 0) max_tier = 0;
    int d = depth < 0 ? 0 : depth;
    if (d > max_tier) d = max_tier;
    return max_tier - d;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make run-cull`
Expected: PASS — "All particle_culling tests passed".

- [ ] **Step 6: Commit**

```bash
git add "MatterSurfaceLib/include/particle_culling.h" "MatterSurfaceLib/src/particle_culling.cpp" "MatterSurfaceLib/tests/particle_culling_tests.cpp"
git commit -m "feat: add slot_tier (depth -> refinement tier mapping)"
```

---

## Task 5: `detail_size` field + tier-aware `make_sub_particle`

**Files:**
- Modify: `MatterSurfaceLib/include/particle_culling.h`
- Modify: `MatterSurfaceLib/src/particle_culling.cpp`
- Test: `MatterSurfaceLib/tests/particle_culling_tests.cpp`

Refactor `make_particle` into `make_sub_particle(slot, tier, ox, oy, oz, ...)`. Tier 0 with offset (0,0,0) must reproduce today's single particle exactly (regression). Sub-particles use **centered** offsets so the sub-block is centered on the slot: along each axis the world offset from the slot center is `((o + 0.5)/2ᵗ - 0.5) * spacing`, and the continuous-noise lattice coordinate is `slot + ((o + 0.5)/2ᵗ - 0.5)` (which is exactly `slot` at tier 0). Per-particle hashing uses the fine integer coord `slot·2ᵗ + o`.

- [ ] **Step 1: Add the new fields (header)**

In `include/particle_culling.h`, extend `EmittedParticle` (currently lines 9–14):

```cpp
struct EmittedParticle {
    Vector3 position;   // local-space
    float radius;
    uint32_t materialId;
    Vector4 tint;       // RGBA; w = blend strength
    float detail_size;  // nominal lattice spacing at this particle's tier (S / 2^tier)
};
```

Extend `CullParams` — add these two members (place after `vein_warp`, before `seed`):

```cpp
    int   max_tier = 0;   // 0 = one particle per slot (pre-feature behavior)
    float spacing  = 0.0f;// lattice tier-0 spacing S (GridLattice::spacing())
```

- [ ] **Step 2: Write the failing regression + detail_size tests**

In `tests/particle_culling_tests.cpp`, first make the helper set the new params. Replace `default_params` (lines 64–70) with:

```cpp
static CullParams default_params(int margin) {
    CullParams p;
    p.margin = margin; p.base_radius = 0.4f;
    p.jitter_amount = 0.1f; p.tint_alpha = 0.2f; p.seed = 1337;
    p.cell_size = 1.6f; p.cell_origin_offset = Vector3{0,0,0};
    p.spacing = 0.8f;   // matches the GridLattice(0.8f) used throughout
    p.max_tier = 0;     // refinement off unless a test opts in
    return p;
}
```

Add a new test after `test_determinism`:

```cpp
static void test_tier0_regression_and_detail() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(5);

    // max_tier 0: every emitted particle carries detail_size == spacing (tier 0).
    auto base = cull_interior(lat, occ, default_params(1));
    bool all_tier0_detail = true;
    for (const auto& ep : base)
        if (fabsf(ep.detail_size - 0.8f) > 1e-6f) all_tier0_detail = false;
    CHECK(all_tier0_detail, "tier-0 emit sets detail_size == spacing");
    CHECK(!base.empty(), "tier-0 emit is non-empty");
}
```

Register in `main()` after `test_determinism();`:

```cpp
    test_tier0_regression_and_detail();
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make run-cull`
Expected: FAIL — `EmittedParticle` has no member `detail_size` (compile error), confirming the field/emit wiring isn't done.

- [ ] **Step 4: Refactor `make_particle` into `make_sub_particle`**

In `src/particle_culling.cpp`, replace the whole `make_particle` function (lines ~50–90) with:

```cpp
// Build one emitted sub-particle for tier `tier` at sub-offset (ox,oy,oz) within
// slot `c`. Tier 0 with offset (0,0,0) reproduces the legacy one-particle-per-
// slot output exactly. Continuous fields (radius clusters, marble veins) sample
// the centered fractional lattice coord; per-particle uniqueness hashes the fine
// integer coord. All deterministic in (SlotCoord, sub-offset, seed).
static EmittedParticle make_sub_particle(const Lattice& lat, SlotCoord c, int tier,
                                         int ox, int oy, int oz,
                                         const SlotData& d, const CullParams& p) {
    int   scale = 1 << tier;                 // 2^tier
    float inv   = 1.0f / (float)scale;
    int s = (int)p.seed;

    // Fine integer coord (per-particle hashing) and centered fractional coord
    // (continuous noise). Centered: fr == 0 at tier 0, so legacy output is exact.
    int   fx = c.x * scale + ox, fy = c.y * scale + oy, fz = c.z * scale + oz;
    float frx = (ox + 0.5f) * inv - 0.5f;
    float fry = (oy + 0.5f) * inv - 0.5f;
    float frz = (oz + 0.5f) * inv - 0.5f;
    float cfx = c.x + frx, cfy = c.y + fry, cfz = c.z + frz;

    Vector3 base = lat.slot_position(c);
    float spacing = p.spacing;
    float jamt = p.jitter_amount * inv;       // jitter proportional to sub-spacing
    float jx = (lattice_vhash(fx * 2 + 1 + s, fy, fz) - 0.5f) * jamt;
    float jy = (lattice_vhash(fx, fy * 2 + 1 + s, fz) - 0.5f) * jamt;
    float jz = (lattice_vhash(fx, fy, fz * 2 + 1 + s) - 0.5f) * jamt;

    EmittedParticle ep;
    ep.position = Vector3{ base.x + frx * spacing + jx,
                          base.y + fry * spacing + jy,
                          base.z + frz * spacing + jz };

    float f = p.radius_cluster_freq;
    float cluster = (lattice_vnoise(cfx * f, cfy * f, cfz * f) - 0.5f) * 2.0f; // [-1,1]
    float fine    = (lattice_vhash(fx + 211 + s, fy + 211, fz + 211) - 0.5f) * 2.0f;
    float rv = cluster * 0.75f + fine * 0.25f;
    ep.radius     = (p.base_radius * inv) * (1.0f + rv * p.radius_variation);
    ep.materialId = d.materialId;
    ep.detail_size = spacing * inv;           // S / 2^tier

    if (p.vein_freq > 0.0f) {
        float turb  = fbm3(cfx * 0.08f + s, cfy * 0.08f, cfz * 0.08f);
        float band  = sinf((cfx + cfy * 0.6f + cfz) * p.vein_freq
                           + p.vein_warp * turb * 6.2831853f);
        float vein  = powf(0.5f + 0.5f * band, 6.0f);
        float mottle = (fbm3(cfx * 0.2f + 50.0f, cfy * 0.2f, cfz * 0.2f) - 0.5f) * 0.10f;
        float L = (0.92f - 0.55f * vein) + mottle;
        if (L < 0.05f) L = 0.05f;
        if (L > 1.0f)  L = 1.0f;
        ep.tint = Vector4{ L, L * 0.97f, L * 0.92f, p.tint_alpha };
    } else {
        float tr = lattice_vhash(fx + 101 + s, fy, fz);
        float tg = lattice_vhash(fx, fy + 101 + s, fz);
        float tb = lattice_vhash(fx, fy, fz + 101 + s);
        ep.tint = Vector4{ tr, tg, tb, p.tint_alpha };
    }
    return ep;
}
```

NOTE: this changes the call sites of `make_particle` (now `make_sub_particle`); those are updated in Task 6. Do not run the suite yet — it will not link until Task 6.

- [ ] **Step 5: Update the two existing callers to compile (temporary tier-0 calls)**

So the code compiles after this task, update both call sites now to a tier-0 single call (Task 6 replaces the `cull_interior` one with the real loop):

In `cull_interior` Pass 3 (line ~147):
```cpp
            out.push_back(make_particle(lattice, c, d, p));
```
→
```cpp
            out.push_back(make_sub_particle(lattice, c, 0, 0, 0, 0, d, p));
```

In `emit_all` (line ~172):
```cpp
        out.push_back(make_particle(lattice, c, d, p));
```
→
```cpp
        out.push_back(make_sub_particle(lattice, c, 0, 0, 0, 0, d, p));
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make run-cull`
Expected: PASS — "All particle_culling tests passed" (tier-0 `detail_size == spacing`, and the existing determinism/count tests still pass, proving tier-0 output is unchanged).

- [ ] **Step 7: Commit**

```bash
git add "MatterSurfaceLib/include/particle_culling.h" "MatterSurfaceLib/src/particle_culling.cpp" "MatterSurfaceLib/tests/particle_culling_tests.cpp"
git commit -m "feat: tier-aware make_sub_particle + EmittedParticle.detail_size (tier-0 identical)"
```

---

## Task 6: Tiered emission in `cull_interior` and `emit_all`

**Files:**
- Modify: `MatterSurfaceLib/src/particle_culling.cpp`
- Test: `MatterSurfaceLib/tests/particle_culling_tests.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/particle_culling_tests.cpp`, add after `test_tier0_regression_and_detail`:

```cpp
static void test_tiered_emission() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(6);   // coords 0..5

    CullParams p = default_params(1);
    p.max_tier = 1;

    // With max_tier 1, every outermost-shell slot (depth 0) emits 2^3 = 8
    // sub-particles; deeper slots stay at tier 0 (1 particle). So the total
    // exceeds emit_all's one-per-slot count, and is strictly larger than tier 0.
    auto tier0 = cull_interior(lat, occ, default_params(1));
    auto tier1 = cull_interior(lat, occ, p);
    CHECK(tier1.size() > tier0.size(), "max_tier 1 emits more particles than tier 0");

    // detail_size appears at two scales only: S (tier 0) and S/2 (tier 1).
    int n_full = 0, n_half = 0, n_other = 0;
    for (const auto& ep : tier1) {
        if (fabsf(ep.detail_size - 0.8f) < 1e-5f)      ++n_full;
        else if (fabsf(ep.detail_size - 0.4f) < 1e-5f) ++n_half;
        else ++n_other;
    }
    CHECK(n_half > 0, "tier-1 sub-particles carry detail_size == S/2");
    CHECK(n_full > 0, "interior keeps tier-0 detail_size == S");
    CHECK(n_other == 0, "no unexpected detail_size values");

    // Sub-particle count for a single refined slot is exactly 8.
    Occupancy one; one.set(SlotCoord{0,0,0}, SlotData{8});
    CullParams q = default_params(1); q.max_tier = 2;   // isolated slot -> depth 0 -> tier 2
    auto refined = emit_all(lat, one, q);
    CHECK(refined.size() == 64, "isolated slot at tier 2 emits 4^3 = 64 sub-particles");
}

static void test_core_dropped_with_tiers() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(10);
    CullParams p = default_params(1);
    p.max_tier = 1;

    CullStats stats;
    std::vector<SlotCoord> no_mesh;
    auto kept = cull_interior(lat, occ, p, &stats, &no_mesh);

    // The single core cell's 8 slots are still dropped (they are fully buried,
    // depth >= max_tier so tier 0, but core => not emitted at all).
    CHECK(stats.cells_core == 1, "core cell count unchanged with tiers on");
    // No emitted particle may sit inside the core cell (cell index (2,2,2)).
    bool any_in_core = false;
    for (const auto& ep : kept) {
        int cx = (int)floorf(ep.position.x / p.cell_size);
        int cy = (int)floorf(ep.position.y / p.cell_size);
        int cz = (int)floorf(ep.position.z / p.cell_size);
        if (cx == 2 && cy == 2 && cz == 2) any_in_core = true;
    }
    CHECK(!any_in_core, "core cell emits zero particles even with tiers on");
}
```

Register both in `main()` after `test_tier0_regression_and_detail();`:

```cpp
    test_tiered_emission();
    test_core_dropped_with_tiers();
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd "MatterSurfaceLib/tests" && make run-cull`
Expected: FAIL — `tier1.size() > tier0.size()` is false (still one particle per slot) and the 64-count check fails, because emission isn't looping over tiers yet.

- [ ] **Step 3: Implement tiered emission in `cull_interior` Pass 3**

In `src/particle_culling.cpp`, replace the Pass-3 lambda body (lines ~144–148):

```cpp
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        uint64_t k = pack_slot(cell_coord_of(lattice, c, p));
        if (core.find(k) == core.end())
            out.push_back(make_sub_particle(lattice, c, 0, 0, 0, 0, d, p));
    });
```

with:

```cpp
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        uint64_t k = pack_slot(cell_coord_of(lattice, c, p));
        if (core.find(k) != core.end()) return;     // core slots are dropped
        int tier  = slot_tier(slot_depth(occ, c, p.max_tier), p.max_tier);
        int scale = 1 << tier;
        for (int oz = 0; oz < scale; ++oz)
        for (int oy = 0; oy < scale; ++oy)
        for (int ox = 0; ox < scale; ++ox)
            out.push_back(make_sub_particle(lattice, c, tier, ox, oy, oz, d, p));
    });
```

- [ ] **Step 4: Implement tiered emission in `emit_all`**

Replace `emit_all`'s lambda (lines ~171–173):

```cpp
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        out.push_back(make_sub_particle(lattice, c, 0, 0, 0, 0, d, p));
    });
```

with:

```cpp
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        int tier  = slot_tier(slot_depth(occ, c, p.max_tier), p.max_tier);
        int scale = 1 << tier;
        for (int oz = 0; oz < scale; ++oz)
        for (int oy = 0; oy < scale; ++oy)
        for (int ox = 0; ox < scale; ++ox)
            out.push_back(make_sub_particle(lattice, c, tier, ox, oy, oz, d, p));
    });
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cd "MatterSurfaceLib/tests" && make run-cull`
Expected: PASS — "All particle_culling tests passed" (tiered counts, detail scales, isolated-slot 64-count, core still dropped, and all pre-existing tier-0 tests still green).

- [ ] **Step 6: Commit**

```bash
git add "MatterSurfaceLib/src/particle_culling.cpp" "MatterSurfaceLib/tests/particle_culling_tests.cpp"
git commit -m "feat: tiered sub-particle emission keyed on depth-below-surface"
```

---

## Task 7: `StaticParticle.detail_size` + `add_particle` overload

**Files:**
- Modify: `MatterSurfaceLib/include/cluster.h`
- Modify: `MatterSurfaceLib/src/cluster.cpp`

No new unit test here (it is exercised end-to-end by Task 9's resolution test and the app build). Keep the change minimal and compiling.

- [ ] **Step 1: Add `detail_size` to `StaticParticle`**

In `include/cluster.h`, extend `StaticParticle` (lines 19–28):

```cpp
struct StaticParticle {
    Vector3 position;       // Position in local cluster space
    float radius;          // Particle radius
    uint32_t materialId;   // Material identifier
    Vector4 tint;          // RGBA tint; a = blend strength. (1,1,1,0) = no tint.
    float detail_size;     // tier-0 spacing / 2^tier; 0 => fall back to tier 0

    StaticParticle(const Vector3& pos = {0,0,0}, float r = 1.0f, uint32_t mat = 0,
                   const Vector4& t = {1.0f, 1.0f, 1.0f, 0.0f}, float ds = 0.0f)
        : position(pos), radius(r), materialId(mat), tint(t), detail_size(ds) {}
};
```

- [ ] **Step 2: Declare the new `add_particle` overload**

In `include/cluster.h`, after the existing tinted overload (line 50) add:

```cpp
    uint32_t add_particle(const Vector3& local_position, float radius, uint32_t material_id,
                          const Vector4& tint, float detail_size);
```

- [ ] **Step 3: Implement the overload**

In `src/cluster.cpp`, after the existing tinted `add_particle` (lines 84–89) add:

```cpp
uint32_t Cluster::add_particle(const Vector3& local_position, float radius, uint32_t material_id,
                               const Vector4& tint, float detail_size) {
    uint32_t particle_id = next_particle_id_++;
    particles_.emplace_back(local_position, radius, material_id, tint, detail_size);
    mark_cells_dirty_around_particle(local_position, radius);
    return particle_id;
}
```

- [ ] **Step 4: Build to verify it compiles**

Run: `cd "MatterSurfaceLib" && WSL_LINUX=1 make`
Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
git add "MatterSurfaceLib/include/cluster.h" "MatterSurfaceLib/src/cluster.cpp"
git commit -m "feat: StaticParticle.detail_size + add_particle overload carrying it"
```

---

## Task 8: `choose_division_pow` pure helper

**Files:**
- Modify: `MatterSurfaceLib/include/cell.h`
- Modify: `MatterSurfaceLib/src/cell.cpp`
- Test: `MatterSurfaceLib/tests/cell_bounds_tests.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/cell_bounds_tests.cpp`, add this function before `main()` (after the last existing test function):

```cpp
static void test_choose_division_pow() {
    printf("--- choose_division_pow derives resolution from detail ---\n");
    const float S = 0.8f;          // base (tier-0) spacing
    const int base_pow = 4, max_pow = 6;
    check(choose_division_pow(S,        S, base_pow, max_pow) == 4, "tier 0 (detail==S) -> pow 4");
    check(choose_division_pow(S * 0.5f, S, base_pow, max_pow) == 5, "tier 1 (detail==S/2) -> pow 5");
    check(choose_division_pow(S * 0.25f,S, base_pow, max_pow) == 6, "tier 2 (detail==S/4) -> pow 6");
    check(choose_division_pow(S * 0.125f,S, base_pow, max_pow) == 6, "tier 3 clamps to max_pow 6");
    check(choose_division_pow(0.0f,     S, base_pow, max_pow) == 4, "absent detail (0) -> base pow");
    check(choose_division_pow(S * 2.0f, S, base_pow, max_pow) == 4, "coarser-than-base detail -> base pow");
}
```

Register it in `cell_bounds_tests.cpp`'s `main()` (call it alongside the other `test_*()` calls):

```cpp
    test_choose_division_pow();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd "MatterSurfaceLib/tests" && make run-cell`
Expected: FAIL to compile — `choose_division_pow` not declared.

- [ ] **Step 3: Declare `choose_division_pow` in `cell.h`**

In `include/cell.h`, after the `build_clip_particles` declaration (line ~35) add:

```cpp
// Pick a marching-cubes divisionPow from the finest detail present in a cell.
// detail_size_min: smallest StaticParticle.detail_size among the cell's particles
// (<= 0 or >= base_detail => tier 0). base_detail: lattice tier-0 spacing S.
// tier = round(log2(base_detail / detail_size_min)); returns
// clamp(base_pow + max(0,tier), base_pow, max_pow). GL-free / pure.
int choose_division_pow(float detail_size_min, float base_detail, int base_pow, int max_pow);
```

- [ ] **Step 4: Implement `choose_division_pow`**

In `src/cell.cpp`, near the top after the includes (before `build_clip_particles`), add:

```cpp
int choose_division_pow(float detail_size_min, float base_detail, int base_pow, int max_pow) {
    int tier = 0;
    if (detail_size_min > 0.0f && base_detail > 0.0f && detail_size_min < base_detail) {
        tier = (int)lroundf(log2f(base_detail / detail_size_min));
        if (tier < 0) tier = 0;
    }
    int pow = base_pow + tier;
    if (pow < base_pow) pow = base_pow;
    if (pow > max_pow)  pow = max_pow;
    return pow;
}
```

Ensure `<cmath>` is included in `src/cell.cpp` (for `lroundf`/`log2f`); add `#include <cmath>` if not already present.

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd "MatterSurfaceLib/tests" && make run-cell`
Expected: PASS — the new checks print `ok:` and the suite exits 0.

- [ ] **Step 6: Commit**

```bash
git add "MatterSurfaceLib/include/cell.h" "MatterSurfaceLib/src/cell.cpp" "MatterSurfaceLib/tests/cell_bounds_tests.cpp"
git commit -m "feat: choose_division_pow derives cell mesh resolution from detail size"
```

---

## Task 9: Thread base-detail + max-pow into the mesh path

**Files:**
- Modify: `MatterSurfaceLib/include/cluster.h`
- Modify: `MatterSurfaceLib/src/cluster.cpp`
- Modify: `MatterSurfaceLib/include/cell.h`
- Modify: `MatterSurfaceLib/src/cell.cpp`

Wire the cluster's lattice spacing and resolution ceiling down to where the cell picks `divisionPow`. No new unit test (covered by Task 8's pure-function test and Task 11's app acceptance).

- [ ] **Step 1: Add cluster state + setters**

In `include/cluster.h`, add to the public API (near `set_simplification_ratio`):

```cpp
    // Lattice tier-0 spacing S; cells use it to recover the finest tier present
    // from each particle's detail_size when choosing mesh resolution.
    void set_base_detail_size(float s) { base_detail_size_ = s; }
    float get_base_detail_size() const { return base_detail_size_; }
    // Upper bound on per-cell divisionPow (2^pow grid).
    void set_max_division_pow(int p) { max_division_pow_ = p; }
    int get_max_division_pow() const { return max_division_pow_; }
```

Add to the private members (near `simplification_ratio_`):

```cpp
    float base_detail_size_ = 0.0f;   // lattice tier-0 spacing S (0 => disabled)
    int   max_division_pow_ = 6;      // resolution ceiling (64^3)
```

- [ ] **Step 2: Extend `Cell::rebuild_meshes` / `generate_mesh_for_group` signatures**

In `include/cell.h`, change `rebuild_meshes` (line 63):

```cpp
    void rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio = 1.0f);
```
→
```cpp
    void rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager,
                        float simplification_ratio = 1.0f, float base_detail = 0.0f, int max_pow = 6);
```

And `generate_mesh_for_group` (line 88):

```cpp
    void generate_mesh_for_group(uint32_t group_id, const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager, float simplification_ratio);
```
→
```cpp
    void generate_mesh_for_group(uint32_t group_id, const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager,
                                 float simplification_ratio, float base_detail, int max_pow);
```

- [ ] **Step 3: Pass the new args through `rebuild_meshes`**

In `src/cell.cpp`, update the `rebuild_meshes` definition (line ~122) signature to match:

```cpp
void Cell::rebuild_meshes(const std::vector<StaticParticle>& cluster_particles, BLASManager& blas_manager,
                          float simplification_ratio, float base_detail, int max_pow) {
```

and update its call to `generate_mesh_for_group` (line ~134):

```cpp
        generate_mesh_for_group(group_id, cluster_particles, blas_manager, simplification_ratio);
```
→
```cpp
        generate_mesh_for_group(group_id, cluster_particles, blas_manager, simplification_ratio, base_detail, max_pow);
```

- [ ] **Step 4: Use detail-driven resolution in `generate_mesh_for_group`**

In `src/cell.cpp`, update the definition signature (line ~293) to match Step 2.

Then, find where the group's particles are gathered (the loop populating `particles` from `particle_indices`, lines ~322–335). After that loop, compute the finest detail. Insert immediately before `bounds.divisionPow = 4;` (line ~304 is BEFORE the loop — so instead set the pow AFTER the loop, then rebuild `voxel`/`blend_width`/radii using it).

Concretely, restructure lines ~301–335 so resolution is chosen from the gathered particles. Replace:

```cpp
    Bounds bounds;
    bounds.center = center;
    bounds.size = Vector3{actual_size, actual_size, actual_size};
    bounds.divisionPow = 4; // Always 16x16x16 resolution
    int gridSize = 1 << bounds.divisionPow;
    float voxel = actual_size / (float)(gridSize - 1);
    float blend_width = kBlendVoxels * voxel;
    float cull_radius = kFeatureCullVoxels * voxel;
    float vis_radius  = kFeatureVisVoxels  * voxel;
```

with:

```cpp
    Bounds bounds;
    bounds.center = center;
    bounds.size = Vector3{actual_size, actual_size, actual_size};

    // Mesh resolution follows the finest detail present in this cell.
    float detail_min = base_detail;   // default tier-0 (base) detail
    for (uint32_t idx : particle_indices) {
        if (idx >= cluster_particles.size()) continue;
        float ds = cluster_particles[idx].detail_size;
        if (ds > 0.0f && ds < detail_min) detail_min = ds;
    }
    bounds.divisionPow = choose_division_pow(detail_min, base_detail, 4, max_pow);
    int gridSize = 1 << bounds.divisionPow;
    float voxel = actual_size / (float)(gridSize - 1);
    float blend_width = kBlendVoxels * voxel;
    float cull_radius = kFeatureCullVoxels * voxel;
    float vis_radius  = kFeatureVisVoxels  * voxel;
```

(`particle_indices` is `group_it->second`, already in scope from line ~299. If `base_detail` is 0, `choose_division_pow` returns the base pow 4, preserving today's behavior.)

- [ ] **Step 5: Pass cluster state into `rebuild_meshes`**

In `src/cluster.cpp`, `update_cell_meshes` (line ~287):

```cpp
        cell->rebuild_meshes(particles_, blas_manager_, simplification_ratio_);
```
→
```cpp
        cell->rebuild_meshes(particles_, blas_manager_, simplification_ratio_,
                             base_detail_size_, max_division_pow_);
```

- [ ] **Step 6: Build the app to verify it compiles**

Run: `cd "MatterSurfaceLib" && WSL_LINUX=1 make`
Expected: builds clean. (Default `base_detail_size_ == 0` until Task 10 sets it, so resolution stays at base pow 4 — unchanged behavior.)

- [ ] **Step 7: Run the cell suite to confirm no regression**

Run: `cd "MatterSurfaceLib/tests" && make run-cell && make run-cont`
Expected: both pass. (`cell_bounds_tests` does not call `rebuild_meshes`, so signature changes don't break it; `mesh_continuity_tests` replicates meshing independently and is unaffected.)

- [ ] **Step 8: Commit**

```bash
git add "MatterSurfaceLib/include/cluster.h" "MatterSurfaceLib/src/cluster.cpp" "MatterSurfaceLib/include/cell.h" "MatterSurfaceLib/src/cell.cpp"
git commit -m "feat: cells pick divisionPow from finest detail via base_detail + max_pow"
```

---

## Task 10: Scene wiring — knobs + detail_size + base_detail

**Files:**
- Modify: `MatterSurfaceLib/main.cpp`

Activate the feature in `setup_lattice_scene`: read `MSL_MAX_TIER`/`MSL_MAX_POW`, set `CullParams.max_tier`/`spacing`, set the cluster's base detail + max pow, and pass `detail_size` into `add_particle`.

- [ ] **Step 1: Read the new env knobs and set cluster resolution state**

In `main.cpp`, just after the cell-size setup (`set_smallest_cell_size(cell_size);`, line ~562), add:

```cpp
        int max_tier = 1;
        if (const char* mtEnv = getenv("MSL_MAX_TIER")) max_tier = atoi(mtEnv);
        if (max_tier < 0) max_tier = 0;
        int max_pow = 6;
        if (const char* mpEnv = getenv("MSL_MAX_POW")) max_pow = atoi(mpEnv);
        if (max_pow < 4) max_pow = 4;
        test_cluster_->set_base_detail_size(SPACING);
        test_cluster_->set_max_division_pow(max_pow);
```

- [ ] **Step 2: Set `max_tier` and `spacing` on `CullParams`**

In the `CullParams p;` block (lines ~597–604), after `p.cell_origin_offset = ...;` add:

```cpp
        p.max_tier = max_tier;
        p.spacing  = SPACING;
```

- [ ] **Step 3: Pass `detail_size` into `add_particle`**

In the emit→add loop (lines ~612–615), change:

```cpp
        for (auto& ep : emitted) {
            Vector3 pos = { ep.position.x - halfx, ep.position.y - halfy, ep.position.z - halfz };
            test_cluster_->add_particle(pos, ep.radius, ep.materialId, ep.tint);
        }
```
to:
```cpp
        for (auto& ep : emitted) {
            Vector3 pos = { ep.position.x - halfx, ep.position.y - halfy, ep.position.z - halfz };
            test_cluster_->add_particle(pos, ep.radius, ep.materialId, ep.tint, ep.detail_size);
        }
```

- [ ] **Step 4: Update the cull log line to report tiering**

Replace the non-bypass log (lines ~627–630):

```cpp
            printf("[cull] occupied=%zu emitted=%zu cells_meshed=%zu "
                   "cells_skipped=%zu cells_core=%zu (margin=%d)\n",
                   occ.count(), emitted.size(), stats.cells_meshed,
                   stats.cells_skipped, stats.cells_core, margin);
```
with:
```cpp
            printf("[cull] occupied=%zu emitted=%zu cells_meshed=%zu "
                   "cells_skipped=%zu cells_core=%zu (margin=%d max_tier=%d max_pow=%d)\n",
                   occ.count(), emitted.size(), stats.cells_meshed,
                   stats.cells_skipped, stats.cells_core, margin, max_tier, max_pow);
```

- [ ] **Step 5: Build the app**

Run: `cd "MatterSurfaceLib" && WSL_LINUX=1 make`
Expected: builds to `MatterSurfaceLib/matter_surface_lib`.

- [ ] **Step 6: Smoke-test emit counts at tier 0 vs tier 1 (headless capture)**

Run:
```bash
cd "MatterSurfaceLib" && MSL_MAX_TIER=0 MSL_FRAMES=3 MSL_RENDER_MODE=1 MSL_CAPTURE=tier0.png timeout 120 ./matter_surface_lib 2>&1 | grep "\[cull\]"
MSL_MAX_TIER=1 MSL_FRAMES=3 MSL_RENDER_MODE=1 MSL_CAPTURE=tier1.png timeout 120 ./matter_surface_lib 2>&1 | grep "\[cull\]"
```
Expected: the `emitted=` count for `max_tier=1` is substantially larger than for `max_tier=0`, and both `[cull]` lines print. (Render mode 1 = solid, fast.)

- [ ] **Step 7: Commit**

```bash
git add "MatterSurfaceLib/main.cpp"
git commit -m "feat: wire tiered lattice + detail-driven resolution into the brick scene"
```

---

## Task 11: Full verification + visual acceptance

**Files:** none (verification only).

- [ ] **Step 1: Run all headless suites**

Run: `cd "MatterSurfaceLib/tests" && make run-cull && make run-cell && make run-reg && make run-cont && make run-simp && make run-blas && make run-tint`
Expected: every suite prints its pass line and exits 0.

- [ ] **Step 2: Capture a raytraced comparison (tier 0 vs tier 1)**

Raytrace mode is slow; run in the background and wait for completion notification (do NOT poll).

Run (background):
```bash
cd "MatterSurfaceLib" && MSL_MAX_TIER=0 MSL_FRAMES=3 MSL_RENDER_MODE=0 MSL_CAPTURE=rt_tier0.png ./matter_surface_lib
```
Then:
```bash
cd "MatterSurfaceLib" && MSL_MAX_TIER=1 MSL_FRAMES=3 MSL_RENDER_MODE=0 MSL_CAPTURE=rt_tier1.png ./matter_surface_lib
```
Expected: `MatterSurfaceLib/rt_tier0.png` and `rt_tier1.png` are written. The user inspects them: the tier-1 surface should read as finer and less periodic than tier-0, with no holes/inner surfaces and comparable (not multi-minute-regressed) render time.

- [ ] **Step 3: Report results to the user for visual sign-off**

Summarize emit counts (from the `[cull]` lines), the resolution range used (look for the `[cull]` `max_pow`), and present the two captures for the user to compare. Do not mark the feature done until the user confirms the surface looks right.

---

## Self-Review

**Spec coverage:**
- Phase 0 (strip LOD, keep simplification, unconditional skip-mesh) → Tasks 1–2. ✓
- Phase 1 (depth→tier, `(2ᵗ)³` sub-particles, determinism, `detail_size`) → Tasks 3–6. ✓
- Phase 2 (`StaticParticle.detail_size`, `choose_division_pow`, base_detail/max_pow threading) → Tasks 7–9. ✓
- Phase 3 (`MSL_MAX_TIER`, `MSL_MAX_POW`, scene wiring, drop `MSL_LOD`/`MSL_SURFACE_POW`) → Tasks 2 + 10. ✓
- Testing list (slot_depth, slot_tier, sub-count, detail_size, determinism, core-dropped, max_tier=0 regression, divisionPow derivation) → Tasks 3,4,5,6,8. ✓
- Backlog doc → already written in the brainstorming phase (`docs/superpowers/backlog.md`). ✓

**Refinement vs. spec:** the spec wrote the fractional coord as `cf = c + (o+0.5)/2ᵗ`; this plan uses the *centered* form `cf = c + ((o+0.5)/2ᵗ − 0.5)` so tier 0 reproduces legacy output exactly (regression guard in Task 5/6). This is a strict improvement and is documented in Task 5.

**Type/name consistency:** `detail_size` (EmittedParticle, StaticParticle), `max_tier`/`spacing` (CullParams), `slot_depth(occ,c,max_depth)`, `slot_tier(depth,max_tier)`, `make_sub_particle(lat,c,tier,ox,oy,oz,d,p)`, `choose_division_pow(detail_size_min,base_detail,base_pow,max_pow)`, `set_base_detail_size`/`set_max_division_pow`, `rebuild_meshes(...,base_detail,max_pow)`, `generate_mesh_for_group(...,base_detail,max_pow)` — used identically across all tasks. ✓

**Placeholder scan:** no TBD/TODO; every code step shows complete code; every run step states the expected result. ✓
