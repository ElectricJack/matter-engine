# Cell-Granular Interior Culling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the per-slot interior culling with cell-granular (all-or-nothing per meshing cell) culling so dropped particles only ever come from fully-interior cells that emit zero triangles, eliminating the hidden inner-surface cavity while still cutting particle count and mesh-gen compute.

**Architecture:** The keep/drop decision moves from the individual slot to the meshing cell. A cell is kept if *any* of its slots is non-buried (existing `slot_is_buried` Chebyshev test, margin ≥ 1); a cell is dropped only when *every* slot is buried. Because a kept cell keeps *all* its slots, no holes form inside a meshed cell (no cavity); because dropped cells are buried with a margin, dropped particles never reach a surface cell. Slots bucket into the *same* cell grid the `Cluster` uses (`floor((slot_local_pos + recenter_offset) / cell_size)`).

**Tech Stack:** C++17, g++, raylib PODs (`Vector3`/`Vector4`), headless CHECK-macro unit tests, WSL Linux build (`WSL_LINUX=1 make`).

**Spec:** `docs/superpowers/specs/2026-06-14-cell-granular-interior-culling-design.md`

---

## Background the implementer needs

- **The meshing pipeline is a fixed, trusted black box.** Do NOT modify `Cluster`, `Cell`, `GenerateMesh`, the simplifier, or BLAS code. We only change what is fed into `Cluster::add_particle`.
- **Why the change:** the old per-slot `cull_interior` dropped individual buried slots, leaving holes inside otherwise-solid meshing cells. Marching cubes wrapped each hole in a hidden inner surface (culled produced *more* triangles than the no-cull baseline). Dropping whole cells atomically fixes this.
- **Each meshing cell builds its SDF from only its own particles** (`Cell::generate_mesh_for_group`, `MatterSurfaceLib/src/cell.cpp:293`). A particle is assigned to a cell when its sphere overlaps the cell (`Cluster::update_cell_meshes`, `MatterSurfaceLib/src/cluster.cpp:256`).
- **The Cluster cell grid** is `cell_coord = floor(local_position / cell_size)` (`MatterSurfaceLib/src/cluster.cpp:170`) with `cell_size = smallest_cell_size * (1 << lod_level)`. In the scene the cluster is built with `smallest_cell_size = 5.0` (`MatterSurfaceLib/main.cpp:302`) and runs at LOD 0, so `cell_size = 5.0`.
- **Existing files are already created and wired into the Makefile / build-all.sh** by the prior sub-project. This plan only edits them; no new files, no Makefile or build-all.sh changes.

## File Structure

- Modify: `MatterSurfaceLib/include/particle_culling.h` — add `cell_size` and `cell_origin_offset` to `CullParams`; add `CullStats`; change `cull_interior` signature to accept an optional `CullStats*`; update the contract comment.
- Modify: `MatterSurfaceLib/src/particle_culling.cpp` — replace `cull_interior`'s body with the cell-atomic two-pass algorithm + a `cell_key` helper; fill stats. `slot_is_buried`, `emit_all`, `make_particle`, `lattice_vhash`, `lattice_vnoise` are unchanged.
- Modify: `MatterSurfaceLib/tests/particle_culling_tests.cpp` — thread the two new `CullParams` fields through `default_params`; replace `test_cull_counts` expectations with cell-atomic numbers; add an anti-cavity invariant test.
- Modify: `MatterSurfaceLib/main.cpp` — in `setup_lattice_scene`, set `cell_size` and `cell_origin_offset`, pass a `CullStats`, and extend the `[cull]` log line.

## Build & test commands (reference)

- Build the headless culling test: `make -C MatterSurfaceLib/tests particle_culling_tests`
- Run it: `MatterSurfaceLib/tests/particle_culling_tests`
- Build the Linux app binary: `cd MatterSurfaceLib && WSL_LINUX=1 make`
- Headless A/B capture uses env vars: `MSL_CAPTURE` (relative png path — raylib prepends CWD, so absolute paths fail), `MSL_RENDER_MODE` (1 = solid), `MSL_FRAMES`, `MSL_CAM`, `MSL_CULL_MARGIN` (-1 bypasses culling via `emit_all`).

---

## Task 1: Cell-atomic `cull_interior`

**Files:**
- Modify: `MatterSurfaceLib/include/particle_culling.h`
- Modify: `MatterSurfaceLib/src/particle_culling.cpp`
- Test: `MatterSurfaceLib/tests/particle_culling_tests.cpp`

### Why these exact test numbers

Unit tests use `GridLattice(0.8f)`, a solid 5×5×5 block (coords 0..4), `cell_origin_offset = {0,0,0}`, and `cell_size = 1.6` → 2 slots per cell along an axis (`floor(coord*0.8/1.6)`: coords 0,1→cell 0; 2,3→cell 1; 4→cell 2).

- **Buried set (margin 1):** a slot is buried iff its coord ∈ {1,2,3} on *every* axis → `{1,2,3}³` = 27 slots.
- **Dropped cells (margin 1):** a cell is dropped iff *all* its slots are buried. Only cell `(1,1,1)` (slots `{2,3}³`, all ⊂ `{1,2,3}³`) qualifies → 8 slots dropped. Every other cell contains a coord 0 or 4 slot (non-buried) → kept. So kept = 125 − 8 = **117**.
- **Margin 2:** buried set is just `{(2,2,2)}` (1 slot; coord 1 and 3 reach an empty slot at margin 2). No cell is fully buried (cell `(1,1,1)` has non-buried slots) → kept = **125**.
- **emit_all** → 125. **margin 0** clamps to 1 → 117.

- [ ] **Step 1: Update the header — add fields, stats, and signature**

In `MatterSurfaceLib/include/particle_culling.h`, change the `CullParams` struct and the `cull_interior` declaration. Replace the existing `CullParams` block and the `cull_interior` declaration block with:

```cpp
struct CullParams {
    int margin;          // sub-shell layers to keep; clamped to >= 1
    float base_radius;   // nominal particle radius
    float jitter_amount; // per-axis position jitter magnitude (0 = none)
    float tint_alpha;    // tint blend strength written to EmittedParticle.tint.w
    uint32_t seed;       // determinism seed for jitter/tint
    float cell_size;     // meshing cell size used to bucket slots into cells
    Vector3 cell_origin_offset; // added to slot_position before bucketing so the
                                // cull grid matches the Cluster's recentered grid
};

// Per-call statistics about the cell-granular decision (optional output).
struct CullStats {
    size_t cells_total = 0;
    size_t cells_kept = 0;
    size_t cells_dropped = 0;
};
```

Then replace the `cull_interior` declaration and its comment with:

```cpp
// Cell-granular interior culling. Slots are bucketed into meshing cells via
// floor((slot_position + cell_origin_offset) / cell_size). A cell is KEPT if any
// of its slots is non-buried (slot_is_buried with margin>=1); a cell is DROPPED
// only when EVERY slot in it is buried. Kept cells emit ALL their slots, so no
// holes (and thus no inner SDF cavity) ever form inside a meshed cell. When
// `stats` is non-null it is filled with the per-call cell counts.
std::vector<EmittedParticle> cull_interior(const Lattice& lattice,
                                           const Occupancy& occ,
                                           const CullParams& p,
                                           CullStats* stats = nullptr);
```

(`Vector3` and `size_t` are already available via the existing `#include "lattice.h"` / `#include <cstddef>` chain in `occupancy.h`.)

- [ ] **Step 2: Write the failing tests**

In `MatterSurfaceLib/tests/particle_culling_tests.cpp`:

(a) Update `default_params` to set the two new fields:

```cpp
static CullParams default_params(int margin) {
    CullParams p;
    p.margin = margin; p.base_radius = 0.4f;
    p.jitter_amount = 0.1f; p.tint_alpha = 0.2f; p.seed = 1337;
    p.cell_size = 1.6f; p.cell_origin_offset = Vector3{0,0,0};
    return p;
}
```

(b) Replace the body of `test_cull_counts` with the cell-atomic expectations:

```cpp
static void test_cull_counts() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(5);

    // cell_size 1.6 -> 2 slots/cell. margin 1 buries {1,2,3}^3; the only fully
    // buried cell is (1,1,1) = slots {2,3}^3 -> drops 8, keeps 117.
    auto m1 = cull_interior(lat, occ, default_params(1));
    CHECK(m1.size() == 117, "margin 1 drops the one fully-buried cell (8 slots)");

    // margin 2 buries only the center slot -> no cell fully buried -> keep all.
    auto m2 = cull_interior(lat, occ, default_params(2));
    CHECK(m2.size() == 125, "margin 2 leaves no fully-buried cell");

    auto all = emit_all(lat, occ, default_params(1));
    CHECK(all.size() == 125, "emit_all keeps all 125");

    auto m0 = cull_interior(lat, occ, default_params(0));
    CHECK(m0.size() == 117, "margin 0 clamped to 1");
}
```

(c) Add an anti-cavity invariant test (with jitter disabled so emitted positions equal slot positions and bucket exactly). Add the function and call it from `main`:

```cpp
// A cell-key helper mirroring cull_interior's bucketing (offset 0 in tests).
static long long test_cell_key(const Vector3& pos, float cs) {
    long long cx = (long long)floorf(pos.x / cs);
    long long cy = (long long)floorf(pos.y / cs);
    long long cz = (long long)floorf(pos.z / cs);
    return (cx + 1000) * 4000000LL + (cy + 1000) * 2000LL + (cz + 1000);
}

static void test_cell_atomic_no_partial() {
    GridLattice lat(0.8f);
    Occupancy occ = solid_block(5);
    CullParams p = default_params(1);
    p.jitter_amount = 0.0f;   // emitted position == slot_position, exact bucketing

    // Expected occupied-slot count per cell, straight from the occupancy.
    std::map<long long, int> expected;
    occ.for_each([&](SlotCoord c, const SlotData&) {
        Vector3 sp = lat.slot_position(c);
        expected[test_cell_key(sp, p.cell_size)]++;
    });

    // Actual emitted-particle count per cell after culling.
    auto kept = cull_interior(lat, occ, p);
    std::map<long long, int> got;
    for (const auto& ep : kept) got[test_cell_key(ep.position, p.cell_size)]++;

    // Every cell that contributes at least one particle must contribute ALL of
    // its occupied slots (no partially-emitted cell -> no cavity).
    bool all_full = true;
    for (const auto& kv : got)
        if (kv.second != expected[kv.first]) all_full = false;
    CHECK(all_full, "every kept cell emits all of its slots (no partial cell)");

    // Exactly one cell (the fully-buried center) is dropped.
    CHECK(got.size() == expected.size() - 1, "exactly one cell dropped at margin 1");
    CHECK(kept.size() == 117, "117 particles survive cell-atomic margin-1 cull");
}
```

Add `#include <map>` near the top of the test file (after the existing includes) and add `test_cell_atomic_no_partial();` to `main()` after `test_cull_counts();`.

- [ ] **Step 3: Run the tests to verify they FAIL**

Run: `make -C MatterSurfaceLib/tests particle_culling_tests && MatterSurfaceLib/tests/particle_culling_tests`

Expected: FAIL. With the old per-slot `cull_interior`, `test_cull_counts` reports 98 (not 117) for margin 1 and 124 (not 125) for margin 2, and `test_cell_atomic_no_partial` fails the "all_full"/"one cell dropped" checks. (If it instead fails to *compile* because `cull_interior` doesn't yet take the new fields, that still counts as red — proceed to Step 4.)

- [ ] **Step 4: Replace `cull_interior` with the cell-atomic algorithm**

In `MatterSurfaceLib/src/particle_culling.cpp`:

Add includes at the top (after the existing `#include <cmath>`):

```cpp
#include <unordered_map>
#include <cassert>
```

Add a `cell_key` helper just above `cull_interior` (after `make_particle`):

```cpp
// Pack a slot's owning meshing-cell coordinate into one key. Mirrors
// Cluster::get_cell_coordinates: floor((slot_pos + offset) / cell_size).
static uint64_t cell_key(const Lattice& lat, SlotCoord c, const CullParams& p) {
    Vector3 base = lat.slot_position(c);
    int cx = (int)floorf((base.x + p.cell_origin_offset.x) / p.cell_size);
    int cy = (int)floorf((base.y + p.cell_origin_offset.y) / p.cell_size);
    int cz = (int)floorf((base.z + p.cell_origin_offset.z) / p.cell_size);
    return pack_slot(SlotCoord{cx, cy, cz});
}
```

Replace the entire existing `cull_interior` function body with:

```cpp
std::vector<EmittedParticle> cull_interior(const Lattice& lattice,
                                           const Occupancy& occ,
                                           const CullParams& p,
                                           CullStats* stats) {
    assert(p.cell_size > 0.0f && "CullParams.cell_size must be positive");
    int margin = p.margin < 1 ? 1 : p.margin;

    // Pass 1: decide keep/drop per cell. A cell is kept iff any of its slots is
    // non-buried; this is independent of iteration order.
    std::unordered_map<uint64_t, bool> keep;
    occ.for_each([&](SlotCoord c, const SlotData&) {
        uint64_t k = cell_key(lattice, c, p);
        bool not_buried = !slot_is_buried(occ, c, margin);
        auto it = keep.find(k);
        if (it == keep.end()) keep.emplace(k, not_buried);
        else if (not_buried) it->second = true;
    });

    // Pass 2: emit every slot of every kept cell.
    std::vector<EmittedParticle> out;
    occ.for_each([&](SlotCoord c, const SlotData& d) {
        auto it = keep.find(cell_key(lattice, c, p));
        if (it != keep.end() && it->second)
            out.push_back(make_particle(lattice, c, d, p));
    });

    if (stats) {
        stats->cells_total = keep.size();
        stats->cells_kept = 0;
        for (const auto& kv : keep) if (kv.second) ++stats->cells_kept;
        stats->cells_dropped = stats->cells_total - stats->cells_kept;
    }
    return out;
}
```

(`pack_slot` is declared in `occupancy.h`, already included via `particle_culling.h`.)

- [ ] **Step 5: Run the tests to verify they PASS**

Run: `make -C MatterSurfaceLib/tests particle_culling_tests && MatterSurfaceLib/tests/particle_culling_tests`

Expected: PASS — prints `All particle_culling tests passed`.

- [ ] **Step 6: Commit**

```bash
git add MatterSurfaceLib/include/particle_culling.h MatterSurfaceLib/src/particle_culling.cpp MatterSurfaceLib/tests/particle_culling_tests.cpp
git commit -m "$(cat <<'EOF'
feat: cell-granular interior culling (replaces per-slot)

Drop a meshing cell only when every slot in it is buried; kept cells emit
all their slots. Atomic per-cell dropping eliminates the hidden inner SDF
cavity the per-slot pass created. Adds cell_size/cell_origin_offset to
CullParams and an optional CullStats.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Wire cell-granular culling into the scene

**Files:**
- Modify: `MatterSurfaceLib/main.cpp` (`setup_lattice_scene`, currently lines ~519-575)

The cull grid must match the Cluster grid. The scene re-centers particles by half the block extent before `add_particle`, and the cluster grids on those recentered positions with `cell_size = smallest_cell_size = 5.0`. So the cull's `cell_origin_offset` must be the same `-half` translation, and `cell_size` must be the cluster's cell size.

- [ ] **Step 1: Edit `setup_lattice_scene` to pass cell_size + offset and report cell stats**

In `MatterSurfaceLib/main.cpp`, the current block computes `half*` *after* building `CullParams` and calling the cull. Reorder so the half-extents are known first, then set the new fields. Replace this span (from the `CullParams p;` line through the `printf("[cull] ...")` line) with:

```cpp
        // Re-center offset: GridLattice puts slot 0 at the origin; shift by half
        // the block extent so the brick is centered. The cull must bucket slots
        // on the SAME grid the Cluster uses, so it gets this offset and the
        // cluster's cell size (LOD 0 -> smallest_cell_size).
        float halfx = (DIM_X - 1) * SPACING * 0.5f;
        float halfy = (DIM_Y - 1) * SPACING * 0.5f;
        float halfz = (DIM_Z - 1) * SPACING * 0.5f;

        CullParams p;
        p.margin = margin; p.base_radius = BASE_RADIUS;
        p.jitter_amount = POS_JITTER; p.tint_alpha = TINT_ALPHA; p.seed = 1337;
        p.cell_size = test_cluster_->get_smallest_cell_size();   // LOD 0 cell size
        p.cell_origin_offset = Vector3{ -halfx, -halfy, -halfz };

        CullStats stats;
        std::vector<EmittedParticle> emitted =
            bypass ? emit_all(lattice, occ, p) : cull_interior(lattice, occ, p, &stats);

        for (auto& ep : emitted) {
            Vector3 pos = { ep.position.x - halfx, ep.position.y - halfy, ep.position.z - halfz };
            test_cluster_->add_particle(pos, ep.radius, ep.materialId, ep.tint);
        }

        if (bypass) {
            printf("[cull] occupied=%zu emitted=%zu (margin=%d, BYPASS)\n",
                   occ.count(), emitted.size(), margin);
        } else {
            printf("[cull] occupied=%zu emitted=%zu cells_kept=%zu cells_dropped=%zu (margin=%d)\n",
                   occ.count(), emitted.size(), stats.cells_kept, stats.cells_dropped, margin);
        }
```

Leave everything else in the function (the `set_position`, `set_lod_level(0)`, `rebuild_dirty_cells`, and the `Brick has ... cells` print) unchanged.

- [ ] **Step 2: Build the Linux binary**

Run: `cd MatterSurfaceLib && WSL_LINUX=1 make`
Expected: clean build, no errors.

- [ ] **Step 3: Smoke-run headless and confirm the log**

Run (from the `MatterSurfaceLib` directory):

```bash
MSL_CAPTURE=brick_smoke.png MSL_RENDER_MODE=1 MSL_FRAMES=2 ./<linux-binary-name>
```

(Use the Linux binary produced by Step 2; the Makefile names it the project's executable.)

Expected: it prints a `[cull] occupied=8000 emitted=<N> cells_kept=<K> cells_dropped=<D> (margin=2)` line with `D > 0` and `N < 8000`, then `Brick has <cells> cells` and exits 0 after writing `brick_smoke.png`. No crash, no GL errors that abort.

- [ ] **Step 4: Commit**

```bash
git add MatterSurfaceLib/main.cpp
git commit -m "$(cat <<'EOF'
feat: feed cell-granular cull grid from the scene

Pass the cluster's LOD-0 cell size and the brick's re-center offset into
CullParams so the cull buckets on the same grid the Cluster meshes on.
Log cells kept/dropped.

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: A/B acceptance — culled vs. bypass

**Files:** none modified (verification task). Produces a recorded result.

The headline claim is "same visible surface, fewer particles, less work, no inner cavity." Verify against the headless capture harness. The earlier per-slot run gave culled = 68,800 surface triangles vs. bypass = 53,913 (inner cavity inflation). Cell-granular should bring culled surface triangles back to ≈ bypass.

- [ ] **Step 1: Capture the culled build and save its log**

From `MatterSurfaceLib`:

```bash
MSL_CAPTURE=brick_culled.png MSL_RENDER_MODE=1 MSL_FRAMES=8 \
  MSL_CAM="18,14,18,0,2,0" ./<linux-binary-name> 2>&1 | tee /tmp/ab_culled.log
```

Expected: writes `brick_culled.png`; log contains the `[cull]` line (margin 2, cells_dropped > 0) and the per-cell mesh-generation lines.

- [ ] **Step 2: Capture the bypass build and save its log**

```bash
MSL_CULL_MARGIN=-1 MSL_CAPTURE=brick_full.png MSL_RENDER_MODE=1 MSL_FRAMES=8 \
  MSL_CAM="18,14,18,0,2,0" ./<linux-binary-name> 2>&1 | tee /tmp/ab_full.log
```

Expected: writes `brick_full.png`; `[cull]` line shows `BYPASS`, `emitted=8000`.

- [ ] **Step 3: Compare triangle counts, particle counts, and timing**

From the two logs, extract and compare:
- Total triangles registered with the BLAS (sum the per-cell "Registering N triangles" / "N triangles, BLAS handle" lines).
- `emitted=` particle counts (culled ≪ 8000; bypass = 8000).
- Mesh-generation time, if the profiler line is present.

Commands:

```bash
echo "CULLED:"; grep -oE '[0-9]+ triangles' /tmp/ab_culled.log | awk '{s+=$1} END{print s" tris"}'
grep -E '^\[cull\]' /tmp/ab_culled.log
echo "BYPASS:"; grep -oE '[0-9]+ triangles' /tmp/ab_full.log | awk '{s+=$1} END{print s" tris"}'
grep -E '^\[cull\]' /tmp/ab_full.log
```

**Acceptance:**
- Culled BLAS triangles ≤ bypass BLAS triangles (NOT greater — greater means the cavity is back; treat as failure and debug the cell/offset mapping in Task 2 Step 1).
- Culled `emitted` ≪ 8000.
- Mesh-gen time culled < bypass (or at worst equal).

- [ ] **Step 4: Visual confirmation**

Open `brick_culled.png` and `brick_full.png` (have the user view them, or read them with the Read tool). They should look the same to the eye (a solid checkerboard-stone block) — no holes, no flickery interior facets. Note any visible difference; an identical-looking pair is the pass condition. (Solid render mode is semi-transparent, so a faint interior may show in BOTH equally — that is acceptable; what matters is that culled is not visibly worse than bypass.)

- [ ] **Step 5: Record the result**

Append a short "Acceptance result (cell-granular)" note to `docs/superpowers/specs/2026-06-14-cell-granular-interior-culling-design.md` with the measured culled-vs-bypass triangle counts, particle counts, and mesh-gen times, then commit:

```bash
git add docs/superpowers/specs/2026-06-14-cell-granular-interior-culling-design.md
git commit -m "$(cat <<'EOF'
docs: record cell-granular culling A/B acceptance result

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Self-Review (completed by plan author)

**Spec coverage:**
- Core rule (keep-if-any, drop-if-all-buried, atomic) → Task 1 Step 4. ✓
- `cell_size` + bucketing on Cluster grid → Task 1 (field), Task 2 (offset/cell_size wired). ✓
- Replace `cull_interior` outright (no per-slot mode) → Task 1 Step 4 replaces the body; no alternate path added. ✓
- Anti-cavity invariant test → Task 1 Step 2 (`test_cell_atomic_no_partial`). ✓
- Replace `test_cull_counts` expectations → Task 1 Step 2(b). ✓
- Extend cull logging (cells kept/dropped) → Task 2 Step 1 via `CullStats`. ✓
- A/B acceptance with restored triangle-count metric → Task 3. ✓
- Error handling: `cell_size <= 0` assert, `margin < 1` clamp → Task 1 Step 4. ✓
- Unchanged: `slot_is_buried`, `emit_all`, `make_particle`, jitter helpers, pipeline → not touched. ✓
- No Makefile/build-all.sh changes → confirmed (files already wired). ✓

**Placeholder scan:** No TBD/TODO/"handle edge cases"; all code steps show full code. ✓

**Type consistency:** `CullParams.cell_size` (float), `CullParams.cell_origin_offset` (Vector3), `CullStats{cells_total,cells_kept,cells_dropped}`, `cull_interior(..., CullStats* stats=nullptr)` are used identically in the header (Task 1 Step 1), the impl (Task 1 Step 4), the tests (Task 1 Step 2), and the scene (Task 2 Step 1). `default_params` sets both new fields so all existing test call-sites compile. ✓
