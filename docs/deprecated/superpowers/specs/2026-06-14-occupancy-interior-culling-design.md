# Occupancy-Based Interior Particle Culling — Design

**Date:** 2026-06-14
**Status:** Approved (design); ready for implementation plan

## Goal

Speed up mesh generation for solid, lattice-arranged parts by **eliminating interior particles before they reach the meshing pipeline**, while producing **visibly identical** geometry. This is the first sub-project of a larger effort to build reusable high-detail "bricks" and assemble them into larger structures (e.g. a castle). LOD generation and instancing/streaming are explicitly out of scope here and handled in later passes.

## Non-Goals

- No changes to the meshing pipeline (`Cluster`, `Cell::generate_mesh_for_group`, `GenerateMesh`, the SDF/marching-cubes core, the simplifier, BLAS registration). That code is working and trusted; we only change what is fed into it.
- No LOD generation (separate later pass).
- No instancing, frustum culling, occlusion culling, or streaming (later sub-projects).
- No new lattice types beyond a regular grid (the interface is designed to allow hex/diamond later, but only the grid is implemented now).

## Key Insight

A solid part is mostly interior. Marching cubes only emits a surface where the SDF field crosses the isolevel — i.e. near the *outer shell*. Particles buried deep inside the part contribute to the SDF field there but produce no visible surface, yet the existing pipeline still evaluates the SDF against every one of them. If we never emit those buried particles, the pipeline does proportionally less work and the visible surface is unchanged.

The lattice gives us a cheap way to decide "buried vs. surface": a slot is on the shell if any neighbor (out to a small margin) is empty.

## Architecture

The change is a **pre-pass that sits between authoring and the cluster**. Today:

```
author places particles  →  cluster.add_particle(...)  →  cells  →  generate_mesh_for_group
```

After this change:

```
author defines lattice + occupancy
        ↓
  [interior-culling pass]   ← NEW: keeps surface slots, drops buried slots
        ↓
  emit particles for kept slots  →  cluster.add_particle(...)  →  (unchanged pipeline)
```

Nothing downstream of `cluster.add_particle` is modified.

## Components

All new code lives in MatterSurfaceLib (new files under `include/` and `src/`). Three small, independently-testable units:

### 1. Lattice interface (`lattice.h` / `lattice.cpp`)

A minimal interface that maps integer slot coordinates to a world/local position and enumerates neighbor offsets. Only a regular-grid implementation ships now.

```cpp
struct SlotCoord { int x, y, z; };

class Lattice {
public:
    virtual ~Lattice() = default;
    // Base (un-jittered) local-space position of a slot's center.
    virtual Vector3 slot_position(SlotCoord c) const = 0;
    // Neighbor offsets that define adjacency for shell detection.
    // For the grid: the 6 face neighbors (±x, ±y, ±z).
    virtual const std::vector<SlotCoord>& neighbor_offsets() const = 0;
};

class GridLattice : public Lattice {
public:
    GridLattice(float spacing);          // spacing = distance between adjacent slot centers
    Vector3 slot_position(SlotCoord c) const override;       // c.* * spacing
    const std::vector<SlotCoord>& neighbor_offsets() const override; // 6-connectivity
private:
    float spacing_;
    std::vector<SlotCoord> neighbors_;   // {±1,0,0},{0,±1,0},{0,0,±1}
};
```

Rationale for neighbor offsets living in the lattice: shell detection means "has an empty neighbor," and what counts as a neighbor differs per lattice type. Keeping it here means the culling algorithm is lattice-agnostic.

### 2. Occupancy grid (`occupancy.h` / `occupancy.cpp`)

A dense or hashed set of occupied slots plus per-slot authoring parameters. Backed by a hash set/map keyed by packed `SlotCoord` so parts need not be axis-aligned dense blocks.

```cpp
struct SlotData {
    uint32_t materialId;
    // Per-slot deterministic variation seed inputs are derived from SlotCoord,
    // not stored, so the same design always bakes identically (see §Determinism).
};

class Occupancy {
public:
    void set(SlotCoord c, const SlotData& d);   // mark slot occupied
    bool occupied(SlotCoord c) const;
    size_t count() const;
    // Iteration over occupied slots (order-independent).
    void for_each(const std::function<void(SlotCoord, const SlotData&)>&) const;
private:
    std::unordered_map<uint64_t, SlotData> slots_;  // key = pack(SlotCoord)
};
```

`pack(SlotCoord)` packs three ints into one `uint64_t` with a bias so negatives are representable (e.g. 21 bits each + offset). This bound (±~1M per axis) is far beyond any realistic part size.

### 3. Interior-culling pass (`particle_culling.h` / `particle_culling.cpp`)

The core algorithm. Pure function: given a lattice, occupancy, and a margin, produce the list of particles to emit. No engine state, no I/O — trivially unit-testable.

```cpp
struct EmittedParticle {
    Vector3 position;   // local-space, jitter already applied
    float radius;
    uint32_t materialId;
    Vector4 tint;
};

struct CullParams {
    int   margin;        // sub-shell layers to keep (see §Margin). >=1.
    float base_radius;   // nominal particle radius
    float jitter_amount; // position jitter magnitude (0 = none)
    float tint_alpha;    // tint blend strength
    uint32_t seed;       // global determinism seed
};

// A slot is KEPT if it is occupied AND (it has an empty neighbor within `margin`
// Chebyshev/however-the-lattice-defines distance). Buried slots (fully surrounded
// out to `margin`) are dropped.
std::vector<EmittedParticle> cull_interior(
    const Lattice& lattice, const Occupancy& occ, const CullParams& p);
```

Shell test for the grid with margin `m`: a slot is kept if **any** slot within the `m`-radius neighborhood (using the lattice's neighbor connectivity expanded `m` steps, or simply the Chebyshev box of half-width `m` for the grid) is unoccupied. Equivalent and cheaper to compute: a slot is *buried* iff every slot in that neighborhood is occupied. We iterate only occupied slots (from `Occupancy::for_each`), so cost is O(occupied × neighborhood_size), independent of bounding-box volume.

### Integration glue (in the scene-setup caller, e.g. `main.cpp`)

`setup_lattice_scene` (or a new `setup_brick_scene`) builds a `GridLattice` + `Occupancy`, calls `cull_interior`, then loops the result into `test_cluster_->add_particle(pos, radius, mat, tint)` exactly as it does today. The existing `lattice_vhash`/`lattice_vnoise` deterministic-noise helpers (main.cpp:294-314) move into the culling module so jitter/tint variation is generated inside `cull_interior` rather than at the call site.

## Data Flow

1. Caller defines a `GridLattice(spacing)` and fills an `Occupancy` (which slots, what material).
2. Caller calls `cull_interior(lattice, occ, params)`.
3. For each occupied slot, the pass checks the margin neighborhood; buried slots are skipped.
4. For each kept slot, the pass computes jittered position (via the moved `lattice_vnoise`) and deterministic tint, emitting an `EmittedParticle`.
5. Caller adds each emitted particle to the cluster.
6. `rebuild_dirty_cells()` runs the existing pipeline unchanged.

## Determinism

A brick's geometry must be a pure function of `(lattice, occupancy, params)` so the same design always bakes byte-identically — this keeps BLAS dedup working and makes the A/B test below reproducible. Jitter and tint are derived from `SlotCoord` + `params.seed` via the existing `lattice_vhash` (no global RNG, no `SetRandomSeed` dependence). Iteration order in `cull_interior` must not affect output (it doesn't: each emitted particle depends only on its own slot).

## The Margin Safeguard (correctness crux)

If we kept *only* the outermost shell (margin 0 / one layer), the SDF field would go hollow just under the surface and marching cubes would emit an unwanted **inner** surface (and possibly thin/holey outer geometry). To prevent this we keep the shell **plus `margin` sub-shell layers**, so the field stays above the isolevel for a band thick enough that no interior crossing occurs.

The minimum safe margin depends on particle reach relative to slot spacing: the kept band must be at least as thick as the SDF's influence radius so the field never dips below isolevel between the deepest kept layer and the surface. Concretely, `margin` must cover `ceil(influence_radius / spacing)` layers. The plan will:

- Start with a conservatively safe margin (e.g. 2 layers) and the existing radius/spacing relationship from the current lattice scene.
- **Validate empirically** (see Testing) that the culled part is pixel-identical (within tolerance) to the fully-populated part, and reduce the margin to the smallest value that still passes.

## Error Handling

- `margin < 1` is clamped to 1 (a 0 margin risks hollow artifacts).
- Empty occupancy → empty result (caller adds nothing; no crash).
- Slot-coord packing asserts coordinates are within the representable range in debug builds.
- No dynamic failure modes in the hot path; the pass allocates one output vector.

## Testing

### Unit tests (headless, gated in `build-all.sh test`)

New `MatterSurfaceLib/tests/particle_culling_tests.cpp`:

1. **Solid block culls interior.** Fill an N×N×N grid; with margin `m`, assert kept count equals the count of slots within `m` of the surface (interior dropped). Check exact counts for small N.
2. **Margin keeps sub-shell layers.** margin=1 vs margin=2 keep progressively more layers; assert the difference equals the expected layer counts.
3. **Hollow / single-voxel-thick shapes keep everything.** A one-slot-thick wall has no buried slots → all kept regardless of margin.
4. **Determinism.** Two runs with identical inputs produce identical `EmittedParticle` lists (positions, tints) bit-for-bit; different `seed` changes jitter but not which slots are kept.
5. **Lattice abstraction.** `GridLattice` neighbor offsets are the 6 face neighbors; `slot_position` scales by spacing.

### Acceptance test (visual A/B)

The headline correctness claim is "identical visible geometry." Verify with the existing headless capture harness:

- Build the same part **twice** — once fully populated (emit every occupied slot, culling bypassed), once culled (chosen margin) — render both with identical camera (`MSL_CAM`, `MSL_RENDER_MODE`, `MSL_CAPTURE`). The "emit every occupied slot" baseline is a simple no-cull code path, not a margin value.
- Compare the two PNGs; they must match within a small tolerance (anti-aliasing only). Also compare total triangle counts of the *surface* (should be equal) and particle counts fed in (culled ≪ full).
- Record the achieved particle/time reduction at the part scale used by the lattice stress scene.

This A/B is also how we tune `margin` down to its smallest safe value.

## File Structure

- Create: `MatterSurfaceLib/include/lattice.h`, `MatterSurfaceLib/src/lattice.cpp`
- Create: `MatterSurfaceLib/include/occupancy.h`, `MatterSurfaceLib/src/occupancy.cpp`
- Create: `MatterSurfaceLib/include/particle_culling.h`, `MatterSurfaceLib/src/particle_culling.cpp`
- Create: `MatterSurfaceLib/tests/particle_culling_tests.cpp`
- Modify: `MatterSurfaceLib/main.cpp` — move `lattice_vhash`/`lattice_vnoise` into the culling module; rewrite scene setup to build lattice+occupancy → `cull_interior` → `add_particle`.
- Modify: `MatterSurfaceLib/Makefile` — compile the three new sources.
- Modify: `MatterSurfaceLib/tests/Makefile` — add `particle_culling_tests` target.
- Modify: `build-all.sh` — add `particle_culling_tests` to the headless suite loop.

## Risks / Open Questions for the Plan

- **Margin tuning vs. existing taper/cull.** The mesher already culls sub-voxel particles per LOD (`kFeatureCullVoxels`). At LOD 0 (this sub-project's target) that taper is mild; the A/B test must run at the LOD(s) the bricks will actually be baked at.
- **Non-grid future lattices** change the neighborhood expansion math; the margin logic must ask the lattice for connectivity rather than assuming 6-connectivity. Designed for, not implemented now.
