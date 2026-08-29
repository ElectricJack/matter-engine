# GPU visual-meshing foundation — design

**Date:** 2026-08-22
**Status:** Phase 1 implemented and accepted on the target NVIDIA GPU
**Order:** specification 2 of 3; implementation begins only after
`2026-08-22-windows-msvc-build-migration-design.md` reaches its migration
acceptance gate, and this specification is then the prerequisite for
`2026-08-22-physx-fluid-bake-integration-design.md`
**Goal:** add a Vulkan-compute visual-meshing service whose first production
client turns baked fluid particles into a high-resolution water mesh, while
retaining the CPU meshers for collision, queries, headless builds, validation,
and fallback. The same GPU foundation may later accelerate streamed terrain,
but terrain is a measured Phase 2 and is not allowed to destabilize the fluid
milestone.

## 1. Decisions

1. The GPU mesher is **additional**, not a replacement. Existing CPU output and
   behavior remain the authority for collision and non-Vulkan operation.
2. The first client is a deliberately narrow water field: additive spherical
   particles, one material, one isolevel, no clip particles, no subtractive CSG,
   and no fat primitives.
3. The implementation is native Vulkan compute and belongs to MatterEngine. It
   does not call the PhysX isosurface extractor and does not depend on CUDA.
4. Visual water is generated during the hydrology bake and serialized. It is
   not regenerated every time the level loads.
5. The first PhysX integration reads the final particle snapshot to host memory
   once, then uploads it to this service. CUDA/Vulkan external-memory interop is
   explicitly deferred until profiling proves that one final readback/upload is
   material.
6. Terrain uses its existing surface-nets topology and seam contract. It may
   share field-evaluation, count/scan/compaction, buffer, and scheduling
   infrastructure with water, but it does **not** silently switch to marching
   cubes.
7. A terrain GPU path becomes available only after an A/B measurement proves a
   frame-time or streaming-throughput benefit. Terrain meshing currently runs on
   bake workers, so moving it to the GPU is not assumed to fix render-thread
   hitches.
8. Windows implementation, shader tests, and packaging use the accepted MSVC
   CMake/Ninja target graph. Linux and headless CPU fallback coverage continue
   through the existing GCC build.

## 2. Existing contracts that remain authoritative

### 2.1 Particle surfaces

MatterSurfaceLib already defines the CPU particle field and surface contract:

- `Particle` carries position, per-particle radius, and material id
  (`libs/MatterSurfaceLib/include/particle.h`).
- `GenerateMeshWithScratch` evaluates the smooth particle field and emits a
  marching-cubes mesh (`libs/MatterSurfaceLib/include/surface.h`).
- `ComputeSurfaceNormalsWithScratch` evaluates analytic field-gradient normals.
- `MarchingCubesAlgorithm` adds the existing simplification and tagging path
  (`libs/MatterSurfaceLib/src/marching_cubes_algorithm.cpp`).

The CPU implementation remains unchanged and is the field-semantics oracle.
The GPU water backend must match its zero crossing and normals within the
validation tolerances in section 10; it need not emit the same vertex order or
triangle indices.

### 2.2 Terrain surfaces and seams

Terrain is not a MatterSurfaceLib particle mesh. `terrain_mesher` evaluates a
`terrain_field::FieldRuntime`, emits surface-nets vertices, and exports
`seam::SectorBoundary`, including sparse face vertices and negative-face overlap
bands (`MatterEngine3/src/terrain_mesher.h`,
`MatterEngine3/src/seam_boundary.h`). Equal-rung neighbors depend on the shared
lattice and `[1..n]` ownership rule; cross-rung neighbors depend on the runtime
welder consuming those records.

Any GPU terrain path must preserve:

- the rung ladder and tiled-volume ownership rules;
- sector-local x/z and world-absolute y output convention;
- material buckets;
- all six boundary records for tiled sectors;
- negative-face overlap bands;
- all-air/all-solid success with empty geometry; and
- current CPU fallback and rollback controls.

### 2.3 Vulkan execution

Vulkan is owned by the app thread. `GpuJobQueue` is the current worker-to-app
thread seam, and the tileset bake is the existing pattern for an engine bake
that has a worker half and a Vulkan half. The current context has no general
asynchronous terrain-compute service and `submit_immediate` waits synchronously.

Consequently:

- Phase 1 water baking may run as a blocking GPU bake job because it is an
  explicit authoring operation.
- Phase 2 terrain streaming may not run heavy work through
  `submit_immediate` on the graphics queue. It requires an independently owned
  compute-capable queue plus timeline synchronization, or it stays disabled.
- Headless builds always use the CPU path.

## 3. Component boundaries

### 3.1 `GpuVisualMesher`

A renderer-owned service, implemented beside the Vulkan bake code rather than
inside MatterSurfaceLib's CPU implementation. Its engine-facing API contains no
raw Vulkan handles:

```cpp
namespace gpu_meshing {

enum class FieldKind : uint8_t { ParticleWater, TerrainDensity };

struct Limits {
    uint32_t max_particles;
    uint32_t max_grid_vertices;
    uint32_t max_mesh_vertices;
    uint32_t max_mesh_indices;
};

struct ParticleSample {
    matter::Float3 position_m;
    float radius_m;
};

struct ParticleJob {
    const ParticleSample* particles;
    uint32_t particle_count;
    matter::Aabb bounds_m;
    float voxel_m;
    float blend_width_m;
    float iso_value;
    uint32_t material;
    Limits limits;
};

struct MeshResult {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> indices;
    uint32_t material;
    uint64_t content_digest;
};

bool build_particle_visual(const ParticleJob&, MeshResult&, Stats&,
                           std::string& error);
}
```

The exact public types may be adjusted during planning to match the repository's
math and mesh containers, but these ownership rules are fixed:

- input arrays are borrowed for the duration of the blocking bake call;
- returned CPU vectors own the serialized visual artifact payload;
- renderer-private buffers and descriptors never escape the service;
- every capacity is explicit before dispatch; and
- `false` always includes a stable error category and diagnostic text.

### 3.2 Shared GPU primitives

The following primitives are reusable by water and terrain:

1. bounded storage-buffer allocation;
2. hierarchical exclusive prefix scan;
3. active-cell compaction;
4. deterministic output-offset assignment;
5. counter/status readback;
6. capacity overflow detection; and
7. vertex/index/normal buffer readback or renderer adoption.

The prefix scan is atomic-free for output placement. Atomics may count a final
total or populate unordered work queues, but they may not decide vertex or
triangle order. This makes repeated same-device bakes stable and makes overflow
behavior reproducible.

## 4. Phase 1 particle-water pipeline

### 4.1 Particle binning

The service derives a grid from `bounds_m` and `voxel_m`. Particle influence is
bounded by radius plus blend width. It computes per-cell particle counts,
prefix-scans them, and writes a compact cell-to-particle index list. A particle
touching several cells appears in each relevant list; a hard per-cell neighbor
cap is forbidden because it would silently change dense water surfaces.

The first implementation uses a dense scalar grid inside the bounded water
domain. Sparse bricks are a follow-up optimization gated by measured memory or
dispatch cost. The API nevertheless records grid and output limits so the
backend can change without changing callers.

### 4.2 Field evaluation

One invocation evaluates each scalar-grid vertex. It visits only particles in
overlapping bins and applies the same sphere SDF, smooth-union equation, blend
width, and sign convention as `surface.c`. The equation lives in one small GLSL
include with a CPU conformance test that compares randomly generated particle
sets and sample points against `ProbeFieldScalar`.

PhysX-only smoothing or anisotropy is not part of the initial field. Those are
optional visual preprocessors after the basic Matter field passes parity. If
added, they become explicit job settings and enter the artifact key; they never
alter the CPU collision/query output.

### 4.3 Marching cubes

Meshing uses the existing marching-cubes case table:

1. classify every cell from its eight scalar samples;
2. write triangle and vertex counts per cell;
3. exclusive-scan counts to assign deterministic output ranges;
4. fail before emission if a configured capacity is insufficient;
5. interpolate edge vertices and emit indexed triangles; and
6. compute analytic field-gradient normals from the same particle bins.

The first version may emit vertices per active cell rather than globally weld
edges. That matches the current triangle-soup-friendly render path and avoids a
global hash table. A later weld is allowed only if it demonstrably reduces
artifact size or rendering cost without changing the visible surface.

### 4.4 Output and artifact handoff

The final GPU buffers are copied once to host-owned `MeshResult` vectors. The
hydrology artifact serializer owns subsequent compression and storage. The
result is also eligible for immediate renderer adoption, but the serialized
copy is authoritative so a level reload does not depend on rerunning the GPU
mesher.

The water material is assigned by the artifact consumer, not baked into shader
branches. The visual mesh uses the existing registered glass/water material.

## 5. CPU visual/query split

Every accepted hydrology artifact contains three independent products:

1. a high-resolution GPU-generated visual water mesh;
2. a coarse CPU-generated mesh for editor selection, ray queries, fallback, and
   any collision-hull construction; and
3. water height/depth/velocity fields for buoyancy, current forces, rapid
   intensity, and gameplay queries.

Detailed water triangles are not the boat's buoyancy model. Gameplay samples
the baked fields. The CPU mesh is retained because it is deterministic,
headless, inexpensive at coarse resolution, and useful to systems that require
ordinary geometry.

## 6. Phase 2 terrain frontend

Terrain work begins only after Phase 1 water acceptance. It is split into three
gates; failure at one gate leaves the existing CPU terrain path untouched.

### T0 — measurement

Re-run the existing cold-fill profile on the target ravine and StreamMountain.
Record worker terrain-mesher time, fill throughput, render-thread pump time,
p50/p95/p99 frame time, GPU frame time, and queue occupancy. Existing findings
show about 30 ms of terrain meshing per sector but identify separate
render-thread costs; the new baseline decides whether GPU terrain work is worth
continuing.

### T1 — parity-only GPU terrain bake

Pack the canonical `terrain_field::FieldProgram` op tape into a read-only GPU
buffer and implement an interpreter with the same 96-op cap and noise semantics.
Reuse the existing GLSL surface-tape noise helpers where their semantics match;
do not create a third noise implementation. Pack the river height overlay as a
separate bounded sample buffer and evaluate it after the base height, matching
`RiverHeightOverlay`.

The GPU then reproduces `mesh_sector_tiled`'s surface-nets stages and reads back:

- material buckets;
- `SectorBoundary` face vertices and corner signs; and
- negative-face overlap triangles.

This path is test-only and does not publish terrain to the renderer until the
entire terrain and seam suites pass.

### T2 — scheduled visual terrain path

Create a dedicated compute submission lane only when the selected Vulkan device
can supply an additional compute-capable queue. One owner thread submits to that
queue. Timeline semaphores order mesh completion before renderer adoption.
There is no cross-queue concurrent mutation of renderer containers.

GPU terrain mesh buffers are adopted directly into device-local visual storage.
A background CPU bake still produces collision/query geometry and the canonical
cache artifact. The renderer keeps displaying the previous/coarser terrain tile
until the GPU visual replacement is complete; an incomplete job never creates a
hole.

### T3 — enablement decision

The terrain GPU backend remains opt-in unless the matched A/B run shows both:

- a material improvement in cold-fill throughput or p95 frame time; and
- no regression in p99 frame time, render GPU time outside streaming, visible
  seams, or resident memory budget.

If work merely moves from CPU workers to the graphics critical path, the
terrain backend is rejected while the water backend remains valid.

## 7. Scheduling, cancellation, and lifetime

- Water jobs run through the existing bake command's cancellation token.
- Cancellation is observed between dispatch stages. Submitted Vulkan work is
  allowed to finish; its result is discarded if the generation was superseded.
- Every job carries the world/session generation. Results from an old generation
  cannot publish into a reloaded world.
- Renderer destruction drains or cancels all meshing jobs before Vulkan
  resources are released.
- A device-lost result fails the visual bake and uses the CPU fallback; it does
  not retry on the same device inside the failed session.

## 8. Cache and compatibility identity

The visual-mesh key contains:

- particle snapshot digest;
- bounds, voxel size, radius data, blend width, and isolevel;
- field and extraction contract versions;
- compiled SPIR-V digest for every meshing stage;
- normal/smoothing/anisotropy settings; and
- output format version.

GPU vendor, device id, and driver version are recorded as provenance, not as
semantic key inputs. Once an artifact is baked it is portable. A backend or
shader contract change invalidates the artifact explicitly.

CPU query/collision output has its own key and does not churn when visual-only
settings change.

## 9. Failure behavior

The service fails closed for:

- zero or non-finite voxel/radius/blend parameters;
- non-finite particle data;
- bounds or grid dimensions exceeding declared limits;
- particle-bin allocation overflow;
- scalar-grid or mesh-output capacity overflow;
- unavailable Vulkan compute features;
- shader or descriptor creation failure;
- cancellation or stale generation; and
- device loss.

No output is truncated. The editor reports the required and configured
capacities when known. Hydrology may fall back to the CPU visual mesh, clearly
marked in status, but it may not label that result as the accepted GPU bake.

## 10. Verification

### 10.1 Pure and CPU/GPU parity tests

- particle-bin coverage at cell edges and negative coordinates;
- exclusive-scan goldens, including zero entries and maximum bounded counts;
- random scalar probes against `ProbeFieldScalar`;
- one sphere, two separated spheres, and two blended spheres;
- empty input and all-outside-bounds input;
- analytic normal agreement away from degenerate gradients;
- explicit overflow with no partial mesh returned; and
- repeat bake on one GPU produces byte-identical buffers.

Surface comparison uses a bidirectional closest-surface distance. Initial water
acceptance requires maximum error no greater than one-quarter visual voxel and
no non-manifold boundary that is absent from the CPU reference.

### 10.2 Vulkan tests

- headless/no-Vulkan selection uses CPU and never references renderer symbols;
- GPU smoke on the smallest supported grid;
- cancellation after each dispatch stage;
- result-generation rejection after world reload;
- buffer-lifetime and device-loss fault injection; and
- water mesh upload and raster/RT visibility with the existing glass material.

### 10.3 Terrain Phase 2 tests

Before any runtime terrain switch:

- all existing terrain-field and terrain-mesher tests remain green;
- the complete seam suite passes at equal and 2:1 rungs on all six faces;
- M0 column rollback output is unchanged when GPU terrain is disabled;
- M2 tiled all-air/all-solid behavior matches CPU;
- two neighboring GPU tiles agree at every shared lattice sample;
- GPU boundary records are sorted and repeatable; and
- screenshot/replay diffs cover the ravine and StreamMountain.

## 11. Expected source layout

Implementation planning may refine filenames, but responsibilities remain
separate:

- `MatterEngine3/src/render/gpu_meshing/` — service, buffers, pipelines, job
  validation, readback, and stats;
- `MatterEngine3/shaders_vk/gpu_mesh_*.comp` — binning, scan, field evaluation,
  classify, and emit stages;
- `MatterEngine3/shaders_vk/gpu_mesh_common.glsl` — shared tables and field
  equations;
- `MatterEngine3/tests/gpu_visual_mesher_tests.cpp` — pure and Vulkan smoke
  coverage;
- `MatterEngine3/tests/gpu_terrain_mesher_tests.cpp` — Phase 2 parity/seams;
- `MatterEngine3/src/provider/local_provider.*` and
  `MatterEngine3/src/matter_engine.cpp` — one renderer callback and bake-job
  handoff, following the tileset pattern; and
- the accepted Windows CMake target plus the compiler-neutral source manifests
  — compile the service without duplicating translation-unit inventories; and
- `MatterEngine3/Makefile` and its eventual shared shader manifest — preserve a
  single embedded-SPIR-V inventory for Linux and Windows.

## 12. Acceptance gates

### Foundation accepted

- CPU path remains byte-identical when GPU meshing is disabled.
- The GPU water subset passes section 10 on the target NVIDIA GPU.
- A baked visual mesh survives editor restart without regeneration.
- Missing GPU support produces a usable CPU fallback and a precise diagnostic.

### Fluid prerequisite accepted

- A synthetic PBF particle snapshot becomes a smooth Matter water mesh.
- The mesh renders with the existing glass material from at least three camera
  angles.
- Visual mesh, coarse CPU mesh, and gameplay field can coexist in one artifact
  without sharing topology.

### Terrain extension accepted

- T0 through T3 pass in order.
- No terrain seam or world-reload regression is observed.
- The matched profile demonstrates a real user-facing performance improvement;
  otherwise terrain stays on CPU.

## 13. Phase 1 measured acceptance (2026-08-23)

The accepted MSVC/Vulkan implementation was exercised on an NVIDIA GeForce
RTX 4090 with NVIDIA driver 610.74 and Vulkan API 1.4.341. The deterministic
synthetic final-particle snapshot contains 116 particles. Its visual lattice
contains 68,208 samples and 62,468 cells; 4,390 cells are active and emit
8,772 triangles (26,316 triangle-soup vertices). Two builds on the same device
produced the exact digest `9f0e981939d8f65b` and byte-identical position,
normal, and index streams.

Three fresh process runs separated cold initialization from the second,
already-initialized bake. The cold bake ranged from 200.050 to 222.619 ms. The
warm bake ranged from 34.563 to 38.107 ms (median 35.930 ms); representative
warm stage costs were 10.720 ms for deterministic binning, 1.368 ms for field
evaluation, 15.251 ms for classification/scan/compaction, and 7.223 ms for
emission/final readback. The warm number excludes editor/world startup,
pipeline creation, and the first complete bake. It is intentionally a
synchronous CPU wall-clock measurement, so it still includes command
recording, queue submissions, fences, bounded capacity readbacks, GPU
execution, and the final payload readback. These are authoring-bake
measurements, not per-frame renderer costs.

On the same 116-particle fixture, the 0.48 m coarse CPU collision/fallback
mesh took 5.833 to 6.099 ms warm and emitted 784 triangles. That is faster for
this tiny job but is not a visual-quality comparison. The closest practical
MatterSurface CPU visual lattice requests 0.12 m (roughly 0.121 m after its
power-of-two cubic-grid quantization), took 221.810 to 233.347 ms, and emitted
14,800 triangles. Against that visual-quality comparison the 0.16 m GPU mesh
was 5.82x to 6.49x faster while emitting 8,772 triangles. Particle bins and
the scalar lattice remain device-resident through emission; the three output
streams share one aligned GPU allocation and one final payload readback. Only
bounded scan totals are read back earlier for exact capacity preflight.

The single-sphere oracle remains within 0.05 m of the authored isosurface at a
0.20 m voxel (the required one-quarter-voxel maximum), with outward winding
and analytic-normal dot product at least 0.999. The synthetic artifact reload
performed no Vulkan submission, retained independent visual/coarse/gameplay
products, registered in the normal indexed raster and native-RT geometry
lanes, and completed with zero Vulkan validation errors.

Three normal-editor raster captures of the cached artifact are stored at:

- `MatterEditor/build/baselines/msvc/gpu-mesher-acceptance/overview.png`
- `MatterEditor/build/baselines/msvc/gpu-mesher-acceptance/curve.png`
- `MatterEditor/build/baselines/msvc/gpu-mesher-acceptance/low-water.png`

Matched-camera GPU, coarse-CPU, and visual-quality-CPU comparisons are stored
under `MatterEditor/build/baselines/msvc/gpu-mesher-comparison/`; the most
revealing pair is `gpu-low-water.png` versus `cpu-visual-low-water.png`.

The captures are deliberately a compact synthetic ribbon proving the GPU
mesher/artifact/renderer path; they are not a final PhysX river bake. Terrain
T0-T3 has not started, because this specification gates that work after the
fluid prerequisite and the next PhysX-fluid integration specification.

## 14. References

- Windows MSVC prerequisite:
  `docs/superpowers/specs/2026-08-22-windows-msvc-build-migration-design.md`
- PhysX particle isosurface API, used only as an architectural reference:
  <https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/_api_build/class_px_isosurface_extractor.html>
- PhysX GPU dense/sparse extractor and smoothing service API:
  <https://nvidia-omniverse.github.io/PhysX/physx/5.3.0/_api_build/class_px_physics_gpu.html>
- Local streaming measurements:
  `docs/sector-bake-time-findings-2026-07-30.md`
- Current CPU particle surface API:
  `libs/MatterSurfaceLib/include/surface.h`
- Current terrain and seam contracts:
  `MatterEngine3/src/terrain_mesher.h`,
  `MatterEngine3/src/seam_boundary.h`
