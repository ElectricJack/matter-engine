# Cell Skip-Meshing Interior Culling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate marching-cubes/BLAS work for fully-interior lattice cells and drop deep-core particles, reproducing the outer surface essentially exactly with no inner surface.

**Architecture:** `cull_interior` is rewritten to classify each cell as Surface/Skin/Core, emit particles for every slot except core cells, and output the interior ("no-mesh") cell list. The `Cluster` gains a skip-gate in `rebuild_dirty_cells` that creates but does not mesh no-mesh cells. `main.cpp` lowers the cell size to ~2.4 and wires the no-mesh set in.

**Tech Stack:** C++17, g++, raylib `Vector3`/`Vector4` PODs, marching-cubes mesher, GLSL ray tracer. Branch `feat/lattice-tint`.

**Reference spec:** `docs/superpowers/specs/2026-06-14-cell-skip-meshing-interior-design.md`

---

## Background the implementer must know

- **The prior approach failed.** Dropping a fully-interior cell's *particles* produced *more* surface triangles than bypass (123,056 > 107,826) because (1) the cluster still creates the cell from neighbor sphere-spill-in and meshes it from a sparse set → inner shell, and (2) removing those particles strips the spill-in that kept adjacent surface cells solid → inner surface. **Do not reintroduce particle-dropping for non-core cells.**
- **The fix:** keep particles for surface + skin cells; only *skip the meshing* of interior cells; only *drop particles* of core cells (interior with all 26 neighbors interior — safe because no meshed cell is within sphere-reach).
- **Cell/slot mapping (the #1 risk).** The cull must bucket slots into the *same* grid the cluster keys cells on:
  - cluster: `cell_coord = floor(local_position / cell_size)`, where `local_position = slot_position + cell_origin_offset` (offset = -half-extent, applied by the scene before `add_particle`).
  - cull: `cell_coord = floor((slot_position + cell_origin_offset) / cell_size)`.
  - Both sides pack the integer cell coord with the **same** `pack_slot` (21 bits/axis, `SLOT_BIAS = 1<<20`) from `occupancy.h`. The cluster converts its float `Cell::coordinates` to int via `lroundf` before packing.
- **`slot_is_buried(occ, c, margin)`** (Chebyshev box, margin clamped to ≥1) and `make_particle` / `emit_all` / `lattice_vhash` / `lattice_vnoise` are **unchanged**.

## File Structure

- Modify `MatterSurfaceLib/include/particle_culling.h` — redefine `CullStats`; add `no_mesh_cells` out-param to `cull_interior`; update the contract comment.
- Modify `MatterSurfaceLib/src/particle_culling.cpp` — replace `cull_interior` body with the three-pass classify/core/emit algorithm; add `cell_coord_of` helper; `#include <unordered_set>`.
- Modify `MatterSurfaceLib/include/cluster.h` — add `set_no_mesh_cells` / `clear_no_mesh_cells`; `std::unordered_set<uint64_t> no_mesh_cells_` member; `#include <unordered_set>`.
- Modify `MatterSurfaceLib/src/cluster.cpp` — implement `set_no_mesh_cells`; add the skip-gate in `rebuild_dirty_cells`; `#include "occupancy.h"` and `<cmath>` (for `lroundf`).
- Modify `MatterSurfaceLib/main.cpp` `setup_lattice_scene` — lower cell size to 2.4 (env-sweepable); wire `no_mesh` into `set_no_mesh_cells`; extend `[cull]` log.
- Modify `MatterSurfaceLib/tests/particle_culling_tests.cpp` — replace `test_cull_counts`; replace `test_cell_atomic_no_partial` with `test_no_meshed_cell_borders_dropped`; add `test_skip_set_is_interior`; thread the new param/stats.
- No Makefile / build-all.sh changes.

---

## Task 1: Rewrite the cull (classify / core / emit) with tests

**Files:**
- Modify: `MatterSurfaceLib/include/particle_culling.h`
- Modify: `MatterSurfaceLib/src/particle_culling.cpp`
- Test: `MatterSurfaceLib/tests/particle_culling_tests.cpp`

- [ ] **Step 1: Write the failing tests**

In `MatterSurfaceLib/tests/particle_culling_tests.cpp`, add `#include <set>` and `#include <tuple>` next to the existing `#include <map>`. Add a key helper next to `test_cell_key` (which already exists):

```cpp
// Same key formula as test_cell_key, but from an integer cell coord directly
// (the cull reports no_mesh cells as integer coords with floor already applied).
static long long cell_coord_key(int cx, int cy, int cz) {
    return (cx + 1000) * 4000000LL + (cy + 1000) * 2000LL + (cz + 1000);
}
```

Replace the whole `test_cull_counts` function with:

```cpp
static void test_cull_counts() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(10);

    // spacing 0.8, cell_size 1.6 -> 2 slots/cell; cell k covers slots {2k,2k+1}.
    // margin 1 buries slots with every coord in [1,8]. A cell is interior iff all
    // its slots are buried -> interior cells = k in {1,2,3}^3 = 27. Only (2,2,2)
    // has all 26 neighbors interior -> 1 core cell, dropping its 8 slots.
    CullStats stats;
    std::vector<SlotCoord> no_mesh;
    auto m1 = cull_interior(lat, occ, default_params(1), &stats, &no_mesh);
    CHECK(m1.size() == 992, "margin 1 drops only the single core cell's 8 slots");
    CHECK(no_mesh.size() == 27, "27 interior cells reported as no-mesh");
    CHECK(stats.cells_total == 125, "125 occupied cells total");
    CHECK(stats.cells_skipped == 27, "27 cells skip-meshed (interior)");
    CHECK(stats.cells_core == 1, "1 core cell");
    CHECK(stats.cells_meshed == 98, "98 cells meshed (125 - 27 interior)");

    auto all = emit_all(lat, occ, default_params(1));
    CHECK(all.size() == 1000, "emit_all keeps all 1000 slots");

    // margin 0 clamps to 1 -> same result.
    CullStats s0;
    auto m0 = cull_interior(lat, occ, default_params(0), &s0, nullptr);
    CHECK(m0.size() == 992, "margin 0 clamped to 1");
}
```

Replace the whole `test_cell_atomic_no_partial` function with:

```cpp
static void test_no_meshed_cell_borders_dropped() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(10);
    CullParams p = default_params(1);
    p.jitter_amount = 0.0f;   // emitted position == slot_position, exact bucketing

    std::vector<SlotCoord> no_mesh;
    auto kept = cull_interior(lat, occ, p, nullptr, &no_mesh);

    auto cell_of = [&](const Vector3& pos) {
        return std::make_tuple((int)floorf(pos.x / p.cell_size),
                               (int)floorf(pos.y / p.cell_size),
                               (int)floorf(pos.z / p.cell_size));
    };

    std::set<std::tuple<int,int,int>> occupied;
    occ.for_each([&](SlotCoord c, const SlotData&) {
        occupied.insert(cell_of(lat.slot_position(c)));
    });
    std::set<std::tuple<int,int,int>> kept_cells;   // still bear particles
    for (const auto& ep : kept) kept_cells.insert(cell_of(ep.position));
    std::set<std::tuple<int,int,int>> interior;     // skip-meshed
    for (const SlotCoord& c : no_mesh) interior.insert(std::make_tuple(c.x, c.y, c.z));

    // dropped = occupied with no particles; meshed = occupied and not interior.
    // Invariant: no dropped cell is a 26-neighbor of any meshed cell.
    bool invariant = true;
    for (const auto& cell : occupied) {
        if (kept_cells.find(cell) != kept_cells.end()) continue;  // not dropped
        int cx, cy, cz; std::tie(cx, cy, cz) = cell;
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy && !dz) continue;
            auto nb = std::make_tuple(cx + dx, cy + dy, cz + dz);
            bool meshed = occupied.count(nb) && !interior.count(nb);
            if (meshed) invariant = false;
        }
    }
    CHECK(invariant, "no dropped cell borders a meshed cell (no inner surface)");
}

static void test_skip_set_is_interior() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(10);
    CullParams p = default_params(1);

    std::vector<SlotCoord> no_mesh;
    cull_interior(lat, occ, p, nullptr, &no_mesh);

    // Reference: a cell is interior iff every slot in it is buried.
    std::map<long long, bool> all_buried;
    occ.for_each([&](SlotCoord c, const SlotData&) {
        long long k = test_cell_key(lat.slot_position(c), p.cell_size);
        bool b = slot_is_buried(occ, c, 1);
        auto it = all_buried.find(k);
        if (it == all_buried.end()) all_buried.emplace(k, b);
        else it->second = it->second && b;
    });

    std::set<long long> nm;
    for (const SlotCoord& c : no_mesh) nm.insert(cell_coord_key(c.x, c.y, c.z));

    bool ok = true;
    for (long long k : nm) if (!all_buried[k]) ok = false;
    CHECK(ok, "every no-mesh cell is interior (all slots buried)");

    size_t interior_count = 0;
    for (const auto& kv : all_buried) if (kv.second) ++interior_count;
    CHECK(nm.size() == interior_count, "no-mesh set equals exactly the interior cells");
}
```

Update `main()` in the test file: replace the `test_cull_counts();` line group so the calls read:

```cpp
    test_cull_counts();
    test_no_meshed_cell_borders_dropped();
    test_skip_set_is_interior();
    test_thin_shape_keeps_all();
    test_determinism();
```

(Remove the old `test_cell_atomic_no_partial();` call.)

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make -C "MatterSurfaceLib/tests" particle_culling_tests`
Expected: COMPILE FAILURE — `cull_interior` takes 3-4 args, no `no_mesh_cells` parameter; `CullStats` has no `cells_meshed`/`cells_skipped`/`cells_core` members.

- [ ] **Step 3: Update the header**

In `MatterSurfaceLib/include/particle_culling.h`, replace the `CullStats` struct with:

```cpp
// Per-call statistics about the cell classification (optional output).
struct CullStats {
    size_t cells_total = 0;    // occupied cells
    size_t cells_meshed = 0;   // Surface cells (not interior) -> meshed
    size_t cells_skipped = 0;  // interior cells (Skin + Core) -> skip-meshed
    size_t cells_core = 0;     // core cells -> particles dropped
};
```

Replace the `cull_interior` declaration (and its doc comment) with:

```cpp
// Cell-granular interior skip-meshing. Slots are bucketed into cells via
// floor((slot_position + cell_origin_offset) / cell_size). A cell is INTERIOR
// iff every slot in it is buried (slot_is_buried, margin>=1); a cell is CORE iff
// it is interior AND all 26 neighbor cells are present and interior.
//
// Emits a particle for every occupied slot EXCEPT those in core cells (core
// particles are never within sphere-reach of a meshed cell, so dropping them
// cannot perturb the outer surface). When `no_mesh_cells` is non-null it is
// filled with every interior cell's integer coordinate (the cells the cluster
// should create-but-not-mesh). When `stats` is non-null it gets the per-call
// cell counts. Interior cells keep their particles unless they are core, so
// every meshed (non-interior) cell is backed by particle-bearing neighbors and
// no inner surface forms.
std::vector<EmittedParticle> cull_interior(const Lattice& lattice,
                                           const Occupancy& occ,
                                           const CullParams& p,
                                           CullStats* stats = nullptr,
                                           std::vector<SlotCoord>* no_mesh_cells = nullptr);
```

- [ ] **Step 4: Replace the implementation**

In `MatterSurfaceLib/src/particle_culling.cpp`, add `#include <unordered_set>` next to the existing `#include <unordered_map>`. Replace the `cell_key` helper and the whole `cull_interior` body with:

```cpp
// Integer cell coordinate of a slot, on the same grid the Cluster keys cells on.
static SlotCoord cell_coord_of(const Lattice& lat, SlotCoord c, const CullParams& p) {
    Vector3 base = lat.slot_position(c);
    int cx = (int)floorf((base.x + p.cell_origin_offset.x) / p.cell_size);
    int cy = (int)floorf((base.y + p.cell_origin_offset.y) / p.cell_size);
    int cz = (int)floorf((base.z + p.cell_origin_offset.z) / p.cell_size);
    return SlotCoord{cx, cy, cz};
}

std::vector<EmittedParticle> cull_interior(const Lattice& lattice,
                                           const Occupancy& occ,
                                           const CullParams& p,
                                           CullStats* stats,
                                           std::vector<SlotCoord>* no_mesh_cells) {
    assert(p.cell_size > 0.0f && "CullParams.cell_size must be positive");
    int margin = p.margin < 1 ? 1 : p.margin;

    // Pass 1: classify each cell. interior[k] == true iff no slot in cell k is
    // non-buried. coord_of[k] remembers the cell's integer coordinate.
    std::unordered_map<uint64_t, bool> interior;
    std::unordered_map<uint64_t, SlotCoord> coord_of;
    occ.for_each([&](SlotCoord c, const SlotData&) {
        SlotCoord cc = cell_coord_of(lattice, c, p);
        uint64_t k = pack_slot(cc);
        bool buried = slot_is_buried(occ, c, margin);
        auto it = interior.find(k);
        if (it == interior.end()) {
            interior.emplace(k, buried);
            coord_of.emplace(k, cc);
        } else if (!buried) {
            it->second = false;
        }
    });

    // Pass 2: core = interior cell whose 26 neighbors are all present + interior.
    static const int OFF[3] = {-1, 0, 1};
    std::unordered_set<uint64_t> core;
    for (const auto& kv : interior) {
        if (!kv.second) continue;
        SlotCoord cc = coord_of[kv.first];
        bool all_in = true;
        for (int dz : OFF) { for (int dy : OFF) { for (int dx : OFF) {
            if (dx == 0 && dy == 0 && dz == 0) continue;
            uint64_t nk = pack_slot(SlotCoord{cc.x + dx, cc.y + dy, cc.z + dz});
            auto it = interior.find(nk);
            if (it == interior.end() || !it->second) { all_in = false; }
        }}}
        if (all_in) core.insert(kv.first);
    }

    // Pass 3: emit every slot whose cell is not core.
    std::vector<EmittedParticle> out;
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        uint64_t k = pack_slot(cell_coord_of(lattice, c, p));
        if (core.find(k) == core.end())
            out.push_back(make_particle(lattice, c, d, p));
    });

    if (no_mesh_cells) {
        no_mesh_cells->clear();
        for (const auto& kv : interior)
            if (kv.second) no_mesh_cells->push_back(coord_of[kv.first]);
    }

    if (stats) {
        stats->cells_total = interior.size();
        size_t skipped = 0;
        for (const auto& kv : interior) if (kv.second) ++skipped;
        stats->cells_skipped = skipped;
        stats->cells_core = core.size();
        stats->cells_meshed = stats->cells_total - skipped;
    }
    return out;
}
```

Note: `pack_slot` is declared in `occupancy.h`, already included via `particle_culling.h`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make -C "MatterSurfaceLib/tests" particle_culling_tests && "MatterSurfaceLib/tests/particle_culling_tests"`
Expected: `All particle_culling tests passed`

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/particle_culling.h MatterSurfaceLib/src/particle_culling.cpp MatterSurfaceLib/tests/particle_culling_tests.cpp
git commit -m "$(cat <<'EOF'
feat: rewrite cull_interior as cell skip-meshing (keep particles, drop core)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Cluster skip-gate (create but don't mesh no-mesh cells)

**Files:**
- Modify: `MatterSurfaceLib/include/cluster.h`
- Modify: `MatterSurfaceLib/src/cluster.cpp`

No headless unit test (the skip-gate runs inside GL meshing); it is verified by build + the Task 4 A/B.

- [ ] **Step 1: Add the interface to `cluster.h`**

Add `#include <unordered_set>` near the other includes. In the `public:` section, after the LOD configuration block (around line 78), add:

```cpp
    // Skip-meshing: cells whose packed integer coordinate is in this set are
    // created/tracked but never meshed (they hold no mesh, register no BLAS).
    // Coordinates use the same floor(local/cell_size) basis as get_cell_coordinates.
    void set_no_mesh_cells(const std::vector<Vector3>& coords);
    void clear_no_mesh_cells() { no_mesh_cells_.clear(); }
```

In the `private:` section, after `std::vector<std::unique_ptr<Cell>> cells_;` (around line 112), add:

```cpp
    std::unordered_set<uint64_t> no_mesh_cells_;  // packed integer cell coords
```

- [ ] **Step 2: Implement `set_no_mesh_cells` and the skip-gate in `cluster.cpp`**

At the top of `MatterSurfaceLib/src/cluster.cpp`, add (if not already present):

```cpp
#include "occupancy.h"   // pack_slot, SlotCoord
#include <cmath>         // lroundf
```

Add the method implementation (place it just before `Cluster::rebuild_dirty_cells`):

```cpp
void Cluster::set_no_mesh_cells(const std::vector<Vector3>& coords) {
    no_mesh_cells_.clear();
    no_mesh_cells_.reserve(coords.size());
    for (const Vector3& c : coords) {
        no_mesh_cells_.insert(pack_slot(SlotCoord{
            (int)lroundf(c.x), (int)lroundf(c.y), (int)lroundf(c.z)}));
    }
}
```

In `rebuild_dirty_cells`, replace the dirty-cell loop body so that no-mesh cells are cleared and skipped:

```cpp
    for (auto& cell : cells_) {
        if (cell->is_dirty) {
            uint64_t key = pack_slot(SlotCoord{
                (int)lroundf(cell->coordinates.x),
                (int)lroundf(cell->coordinates.y),
                (int)lroundf(cell->coordinates.z)});
            if (no_mesh_cells_.find(key) != no_mesh_cells_.end()) {
                cell->clear_meshes(&blas_manager_);  // drop any stale geometry
                cell->is_dirty = false;
                continue;                            // never meshed, no BLAS
            }
            update_cell_meshes(cell.get());
            cell->is_dirty = false;
            rebuilt_count++;
        }
    }
```

(The surrounding `rebuilt_count`/TLAS-rebuild logic is unchanged; skipped cells do not count as rebuilt, which is correct — they register nothing.)

- [ ] **Step 3: Verify it builds**

Run: `cd "MatterSurfaceLib" && WSL_LINUX=1 make && cd ..`
Expected: clean build producing `build/linux/matter_surface_lib` (and the copied `./matter_surface_lib`). No new warnings about `pack_slot`/`lroundf`.

- [ ] **Step 4: Commit**

```bash
git add MatterSurfaceLib/include/cluster.h MatterSurfaceLib/src/cluster.cpp
git commit -m "$(cat <<'EOF'
feat: add no-mesh skip-gate to Cluster::rebuild_dirty_cells

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Wire the cull into `main.cpp` `setup_lattice_scene`

**Files:**
- Modify: `MatterSurfaceLib/main.cpp`

- [ ] **Step 1: Lower the cell size (env-sweepable) at the top of `setup_lattice_scene`**

In `setup_lattice_scene`, just after the `--- Tunables ---` block and before building the occupancy, add:

```cpp
        // Skip-meshing cell size (~3 slots/cell so the meshed shell hugs the
        // surface and the interior is skippable). Sweep via MSL_CELL_SIZE.
        float cell_size = 2.4f;
        const char* csEnv = getenv("MSL_CELL_SIZE");
        if (csEnv) { float v = (float)atof(csEnv); if (v > 0.0f) cell_size = v; }
        test_cluster_->set_smallest_cell_size(cell_size);
```

(`p.cell_size = test_cluster_->get_smallest_cell_size();` already reads this back at LOD 0.)

- [ ] **Step 2: Capture the no-mesh set from the cull**

Replace the `CullStats stats;` / `emitted` declaration block with:

```cpp
        CullStats stats;
        std::vector<SlotCoord> no_mesh;
        std::vector<EmittedParticle> emitted =
            bypass ? emit_all(lattice, occ, p)
                   : cull_interior(lattice, occ, p, &stats, &no_mesh);
```

- [ ] **Step 3: Wire the no-mesh set into the cluster and update the log**

Replace the `if (bypass) { ... } else { ... }` logging block with:

```cpp
        if (bypass) {
            test_cluster_->set_no_mesh_cells({});  // mesh everything
            printf("[cull] occupied=%zu emitted=%zu (margin=%d, BYPASS)\n",
                   occ.count(), emitted.size(), margin);
        } else {
            std::vector<Vector3> nm;
            nm.reserve(no_mesh.size());
            for (const SlotCoord& c : no_mesh)
                nm.push_back(Vector3{(float)c.x, (float)c.y, (float)c.z});
            test_cluster_->set_no_mesh_cells(nm);
            printf("[cull] occupied=%zu emitted=%zu cells_meshed=%zu "
                   "cells_skipped=%zu cells_core=%zu (margin=%d)\n",
                   occ.count(), emitted.size(), stats.cells_meshed,
                   stats.cells_skipped, stats.cells_core, margin);
        }
```

The existing `set_no_mesh_cells` call lands **before** the `rebuild_dirty_cells()` call at the bottom of `setup_lattice_scene`, so the skip-gate sees the set on the first rebuild.

- [ ] **Step 4: Verify it builds**

Run: `cd "MatterSurfaceLib" && WSL_LINUX=1 make && cd ..`
Expected: clean build. The `no_mesh` cell coords are the cull's integer cell coords; the `lroundf` packing in the cluster matches `(float)c.x` round-trips exactly.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "$(cat <<'EOF'
feat: wire skip-meshing cull into lattice scene (cell size 2.4, no-mesh set)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: A/B acceptance (culled ≤ bypass)

**Files:**
- Modify: `docs/superpowers/specs/2026-06-14-cell-skip-meshing-interior-design.md` (append the recorded result)

This task has no code; it runs the headless A/B and records the outcome. The pass criteria are from the spec's "Acceptance test" section.

- [ ] **Step 1: Run the bypass (baseline) capture**

Run:
```bash
cd "MatterSurfaceLib" && WSL_LINUX=1 make >/dev/null && cd .. && \
MSL_CULL_MARGIN=-1 MSL_RENDER_MODE=1 MSL_CAPTURE=ab_bypass.png MSL_FRAMES=2 \
  ./matter_surface_lib 2>&1 | grep -E '\[cull\]|REBUILD|TLAS|tris'
```
Expected: a `[cull] ... BYPASS` line; record `emitted`, total cells meshed (`REBUILD: Completed N`), and the BLAS/triangle totals printed for the run.

- [ ] **Step 2: Run the culled capture (default margin 2, cell size 2.4)**

Run:
```bash
MSL_RENDER_MODE=1 MSL_CAPTURE=ab_culled.png MSL_FRAMES=2 \
  ./matter_surface_lib 2>&1 | grep -E '\[cull\]|REBUILD|TLAS|tris'
```
Expected: a `[cull] ... cells_meshed=… cells_skipped=… cells_core=…` line; record the same metrics.

- [ ] **Step 3: Compare against the acceptance criteria**

Confirm ALL of:
- BLAS-registered triangles: **culled ≤ bypass** (the prior approach failed this: 123,056 > 107,826).
- Surface triangle count: culled ≈ bypass (small tolerance).
- Cells meshed: culled < bypass.
- Particles fed in (`emitted`): culled < bypass.
- Mesh-gen time: culled < bypass.
- `ab_culled.png` vs `ab_bypass.png`: visually equal within AA tolerance — no holes, no interior facets.

If culled BLAS triangles materially **exceed** bypass, an invariant is violated. Debug the cell-coordinate alignment between the cull's `no_mesh` coords and `Cluster::get_cell_coordinates` (the shared `pack_slot` + `lroundf` round-trip and the `cell_origin_offset`); do **not** mark this task complete. Escalate if the mapping is correct but BLAS still exceeds bypass.

- [ ] **Step 4: Optionally sweep cell size / margin**

Run a couple of points to confirm the trend (more skipped interior at smaller cells / lower margins) without regressing the BLAS≤bypass invariant:
```bash
MSL_CELL_SIZE=1.6 MSL_RENDER_MODE=1 MSL_FRAMES=2 ./matter_surface_lib 2>&1 | grep '\[cull\]'
MSL_CULL_MARGIN=1 MSL_RENDER_MODE=1 MSL_FRAMES=2 ./matter_surface_lib 2>&1 | grep '\[cull\]'
```

- [ ] **Step 5: Record the result in the spec and commit**

Append a short "## A/B Result (2026-06-14)" section to the design doc with the recorded bypass-vs-culled numbers and PASS/FAIL.

```bash
git add docs/superpowers/specs/2026-06-14-cell-skip-meshing-interior-design.md
git commit -m "$(cat <<'EOF'
docs: record A/B acceptance result for cell skip-meshing cull

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

(Capture PNGs `ab_bypass.png` / `ab_culled.png` are throwaway artifacts; do not commit them.)

---

## Self-Review (completed during planning)

- **Spec coverage:** classify/core/emit three-pass (spec §The Core Model, §Components Changed) → Task 1; `CullStats` redefinition + `no_mesh_cells` out-param → Task 1; cluster `set_no_mesh_cells`/skip-gate (spec §the one pipeline touch) → Task 2; cell size 2.4 + wiring + logging (spec §main.cpp) → Task 3; replaced/added tests (spec §Tests) → Task 1; A/B with restored BLAS≤bypass metric (spec §Acceptance) → Task 4. Error handling (cell_size>0 assert, margin clamp, empty no-mesh = bypass) covered in Task 1/3 code.
- **Placeholder scan:** none — every code step contains complete code; every run step has a concrete command and expected output.
- **Type consistency:** `cull_interior(lattice, occ, p, CullStats* = nullptr, std::vector<SlotCoord>* = nullptr)` is used identically in the header, the impl, all test calls, and `main.cpp`. `CullStats{cells_total, cells_meshed, cells_skipped, cells_core}` is consistent across impl, tests, and the `[cull]` log. `set_no_mesh_cells(const std::vector<Vector3>&)` + `clear_no_mesh_cells()` match between `cluster.h` and `cluster.cpp` and the `main.cpp` callers. `pack_slot(SlotCoord)` + `lroundf` packing is identical in `particle_culling.cpp` and `cluster.cpp`.
