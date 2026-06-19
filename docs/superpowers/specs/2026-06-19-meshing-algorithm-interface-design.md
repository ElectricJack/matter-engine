# Meshing Algorithm Interface — Design

**Date:** 2026-06-19
**Project:** MatterSurfaceLib
**Status:** Approved design, pending implementation plan

## Goal

Make the mesh-generation step pluggable so different merge groups can be meshed by
different algorithms, selected per material. The first new algorithm renders particles
as oriented cubes (for sand-like substances) instead of a smooth isosurface. Watertightness
across algorithm boundaries is explicitly a non-goal — this is a rendering technique for
producing interesting geometry, not a simulation-correctness concern.

Two pieces ship together in this spec:

1. A `MeshingAlgorithm` interface plus a marching-cubes implementation that wraps the
   existing code (behavior-preserving for all current materials).
2. The first alternative implementation: `OrientedCubeAlgorithm`.

Building both at once is deliberate — the second implementation is what proves the
interface is actually right rather than an abstraction designed in a vacuum.

## Key Decisions

- **Selection is material-driven.** Algorithm is chosen by the material, not a new
  particle-type concept. Materials are already authored per-substance and already carry
  the `mergeGroup` that controls meshing grouping, so "sand renders as cubes" is naturally
  a material property and needs no new particle-level data.
- **One algorithm per merge group (required).** Merge groups fuse multiple materials into
  one SDF/mesh; materials in a group are required to agree on algorithm. The group's
  representative material is authoritative. Non-blending primitives like cubes don't share
  a merge group with smooth fluids anyway, so this matches real usage and keeps dispatch a
  single lookup per group with no per-particle branching in the hot loop.
- **Same raytracing pipeline for all algorithms.** Every algorithm produces a fully-formed
  `GroupMeshResult` (mesh + `Tri` + `TriEx`, already material/tint-tagged) and is registered
  through the existing `register_triangles` BLAS path. There is no separate render path and
  no flat-shading flag. Cubes are ordinary triangle geometry; they read as cubes because
  each triangle carries its true face normal in the standard `TriEx` per-vertex normals —
  the same mechanism MC meshes use to carry normals.
- **Threading model untouched.** Dispatch lives in `Cell::build_group_mesh` (already on a
  worker thread). Each worker owns a `SurfaceScratch`; the context borrows it. Algorithms
  must remain GL-free (CPU only), the same contract `build_group_mesh` already honors.

## Architecture

### File layout (new files in MatterSurfaceLib)

- `include/meshing_algorithm.h` — `MeshContext`, `MeshingAlgorithm` abstract base,
  `MeshAlgorithm` enum, `GetMeshingAlgorithm(MeshAlgorithm)` registry accessor.
- `src/meshing_algorithm.cpp` — registry (static singletons) and shared helpers.
- `src/marching_cubes_algorithm.cpp` — `MarchingCubesAlgorithm`, wrapping the existing
  generation logic lifted from `build_group_mesh` (cell.cpp:409–462).
- `src/oriented_cube_algorithm.cpp` — `OrientedCubeAlgorithm`.

### Interface

```cpp
enum class MeshAlgorithm { MarchingCubes = 0, OrientedCubes = 1 };

class MeshingAlgorithm {
public:
  virtual ~MeshingAlgorithm() = default;
  virtual GroupMeshResult generate(const MeshContext& ctx) const = 0;
};

const MeshingAlgorithm& GetMeshingAlgorithm(MeshAlgorithm algo);
```

Each `generate` returns a fully-formed `GroupMeshResult` (mesh + triangles +
triangle_normals, already tagged). No shared post-processing assumes the marching-cubes
spatial hash; the cube algorithm tags triangles directly from their source particle.

### MeshContext (dispatch contract)

`build_group_mesh` keeps its setup (resolve the group's particle subset, bounds, blend
params, clip set) and packs the results into a `MeshContext`, then hands it to the algorithm.
It carries everything an algorithm might need; impls ignore what they don't use.

```cpp
struct MeshContext {
  // Resolved group particles (post cull/vis-clamp) + parallel tint array
  const std::vector<Particle>& particles;
  const std::vector<float4>&   particle_tints;
  float  max_radius;

  // Volume / placement
  Bounds     bounds;        // center, size, divisionPow
  CellBounds cell_bounds;   // min/max for boundary locking
  float      voxel;         // actual_size / (gridSize-1)

  // Isosurface params (MC uses; cubes ignore)
  float blend_width;
  const Particle* clip;   int clip_count;
  const Particle* carve;  int carve_count;
  float carve_blend;
  float simplification_ratio;

  // Per-worker scratch (MC uses spatial hash; cubes ignore)
  SurfaceScratch* scratch;

  uint32_t group_id;
};
```

- `MarchingCubesAlgorithm::generate` is a near-verbatim move of cell.cpp:409–462.
- `OrientedCubeAlgorithm::generate` reads only `particles`, `particle_tints`, `max_radius`,
  `group_id`; it ignores blend/clip/carve/scratch entirely.

## Algorithm Selection & Dispatch

- **Storage:** `MaterialDef` gains one field, `int meshingAlgorithm;` (a `MeshAlgorithm`
  value, `0 = MarchingCubes`). Defaulting to `0` means every existing material keeps current
  behavior with no migration.
- **Resolution:** in `build_group_mesh`, after the particle list is built, read the
  algorithm from the group's representative material (`particles[0].materialId` — the same
  one already used at cell.cpp:402 for the transparency check). Materials in a merge group
  are required to agree, so the representative is authoritative.
- **Accessor:** add `MaterialMeshingAlgorithm(id)` to `material_registry`, mirroring the
  existing `MaterialIsTransparent` / `MaterialMergeGroup` accessors.
- **Dispatch point:**

```cpp
// ... existing setup builds `particles`, `clip`, bounds, blend params ...
MeshAlgorithm algo = (MeshAlgorithm)MaterialMeshingAlgorithm(particles[0].materialId);
MeshContext ctx{ /* fill */ };
return GetMeshingAlgorithm(algo).generate(ctx);
```

Everything downstream (`commit_group_mesh`, BLAS registration, TLAS rebuild) is unchanged —
it only consumes `GroupMeshResult`.

## Oriented-Cube Algorithm

`OrientedCubeAlgorithm::generate` builds the mesh directly — no SDF, no grid, no scratch.

Per particle in the group:

- **Size:** `edge = 2 * radius * sizeScale` (`sizeScale` param, default 1.0).
- **Orientation:** deterministic pseudo-random rotation seeded from quantized position —
  `hash(round(pos / voxel))` → seed → a stable rotation. Stable across re-meshes because the
  seed depends only on the particle's position, not iteration order or frame, so cubes do not
  flicker or re-roll when a cell re-meshes.
- **Geometry:** 8 transformed corners → 12 triangles. Each triangle carries its true face
  normal in the standard `TriEx` per-vertex normals (`N0 = N1 = N2 = face normal`). This is
  what makes the cube read as a cube under raytracing; averaging corner normals would shade
  it as a rounded blob.
- **Color:** material albedo × tint (alpha handled as in current meshes).
- **Tagging:** each emitted triangle's `materialId` / `tint` comes straight from its source
  particle — no spatial-hash lookup needed.

**Output:** accumulate verts/indices into one raylib `Mesh` for the group, plus the parallel
`std::vector<Tri>` / `std::vector<TriEx>` built inline as cubes are emitted. Returns a
`GroupMeshResult` identical in shape to the MC path.

**Params:** `sizeScale` and `rotationJitter` (0–1, scales orientation randomness, default 1.0).
For the first cut these live as constants in the algorithm with `MSL_`-prefixed env-var
overrides, matching the existing pattern (`MSL_BLEND_VOXELS`, `MSL_CARVE_BLEND` at
cell.cpp:371,374) rather than expanding `MaterialDef` yet.

## Testing

- **Behavior preservation:** confirm a representative cell/cluster meshed with the
  refactored `MarchingCubesAlgorithm` produces output equivalent to the pre-refactor path
  (vertex/triangle counts and geometry for existing all-MC materials unchanged).
- **Cube algorithm unit-level checks:** for a small particle set, verify each particle yields
  12 triangles with axis-consistent per-face normals, correct material/tint tags, and that
  orientation is identical across two independent `generate` calls (determinism).
- **Integration:** a mixed cluster with one MC merge group and one oriented-cube merge group
  builds without error, both groups register BLAS, and the scene raytraces.
- **Existing suites:** the MatterSurfaceLib test suites under `MatterSurfaceLib/tests/` and
  `./build-all.sh test` continue to pass.

## Out of Scope

- Watertightness or continuity across algorithm boundaries.
- Per-particle (as opposed to per-merge-group) algorithm selection.
- Splitting a single merge group into multiple algorithm outputs.
- Exposing cube params through `MaterialDef` or UI (env-var overrides only for now).
- Additional isosurface extractors (dual contouring, etc.).
