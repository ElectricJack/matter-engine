# GPU Visual Meshing Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a deterministic Vulkan-compute particle-water mesher that matches MatterSurfaceLib's sphere smooth-union field, persists its output, retains a coarse CPU fallback/query mesh, and feeds the existing glass-material renderer path.

**Architecture:** Matter-owned job/result types and pure validation/reference functions remain free of Vulkan. A renderer-owned `GpuVisualMesher` executes bounded particle binning, hierarchical scans, scalar evaluation, marching-cubes classification/compaction/emission, and one final readback through the existing `VulkanDevice` resource and immediate-submit seams. Hydrology products serialize the visual mesh independently from a coarse CPU mesh and gameplay samples, while renderer conversion turns only the trusted visual mesh into an ordinary `VkScenePart`.

**Tech Stack:** C++17, C11 MatterSurfaceLib field oracle, Vulkan 1.3 compute/SPIR-V, GLSL 450, MSVC 19.44/CMake/Ninja, existing Matter test harness and Vulkan validation smoke harness.

**Spec:** `docs/superpowers/specs/2026-08-22-gpu-visual-meshing-foundation-design.md`

## Global Constraints

- The accepted Windows target graph is the MSVC CMake/Ninja graph; the Linux/rollback Make graph keeps the same shader inventory and compiler-neutral source manifests.
- The GPU mesher is additional. Existing CPU output remains authoritative for collision, queries, headless builds, validation, and fallback.
- The only Phase 1 field is additive spherical particles with one material and one isolevel; no clips, subtractive CSG, fat primitives, PhysX smoothing, anisotropy, CUDA, or PhysX isosurface extraction.
- Output capacities are declared before dispatch. Overflow fails with a stable category and returns no partial mesh.
- Atomics may count and fill bins, but deterministic scans assign mesh output ranges. Per-bin particle ids are sorted before floating-point accumulation.
- Visual water is a serialized bake product; a level reload consumes the artifact without invoking Vulkan meshing.
- CPU fallback and headless code do not include Vulkan headers or reference renderer symbols.
- Terrain stays on its existing surface-nets path. Its T0 measurement and T1-T3 extension start only after the particle-water foundation and fluid-prerequisite gates pass.
- New behavior follows red-green-refactor: every production function is introduced only after its focused test has failed for the expected missing behavior.

---

### Task 1: Public job contract, validation, layout, field oracle, and scan reference

**Files:**
- Create: `MatterEngine3/include/matter/gpu_visual_meshing.h`
- Create: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp`
- Create: `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Produces `gpu_meshing::ParticleSample`, `Aabb`, `Limits`, `ParticleJob`, `MeshResult`, `Stats`, `GridLayout`, `ErrorCode`, `validate_particle_job`, `evaluate_particle_field_reference`, `exclusive_scan_reference`, and `mesh_content_digest`.
- `ParticleSample` is exactly four floats (`position_m`, `radius_m`) and has no material field because Phase 1 jobs carry one material.
- `MeshResult` owns interleaved scalar vectors: three floats per position, three per normal, and `uint32_t` indices.

- [x] **Step 1: Write failing contract tests**

Add focused tests whose wished-for calls are:

```cpp
gpu_meshing::ParticleJob one_sphere_job();
gpu_meshing::GridLayout layout{};
gpu_meshing::Error error{};
CHECK(gpu_meshing::validate_particle_job(job, layout, error), error.message.c_str());
CHECK(layout.grid_vertices == 9u * 9u * 9u, "2 m bounds at 0.25 m create 9^3 samples");
CHECK(layout.grid_cells == 8u * 8u * 8u, "sample lattice creates 8^3 cells");

const float center = gpu_meshing::evaluate_particle_field_reference(
    job.particles, job.particle_count, job.blend_width_m, {0, 0, 0});
CHECK(std::fabs(center + 0.5f) < 1e-6f, "sphere center equals negative radius");

const std::vector<uint32_t> input{3, 0, 2, 5};
std::vector<uint32_t> output;
uint32_t total = 0;
CHECK(gpu_meshing::exclusive_scan_reference(input, output, total), "scan succeeds");
CHECK(output == std::vector<uint32_t>({0, 3, 3, 5}) && total == 10,
      "exclusive scan and total are exact");
```

Cover empty particles, non-finite inputs, inverted bounds, zero voxel/radius, negative blend, every declared limit, multiplication overflow, particles outside bounds, negative coordinates, hard union with zero blend, two-sphere smooth union, scan empty/zero/max/overflow, stable digest, and digest changes for every mesh byte stream or material change.

- [x] **Step 2: Run the new target and verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
```

Expected: configure/build fails because `matter/gpu_visual_meshing.h` and the target do not exist.

- [x] **Step 3: Add the minimal compiler-neutral contract**

Define the public shapes with this ownership/API surface:

```cpp
namespace gpu_meshing {
enum class ErrorCode : uint8_t {
    None, InvalidInput, LimitExceeded, Overflow, Unavailable,
    Cancelled, StaleGeneration, DeviceLost, VulkanFailure, ArtifactFailure
};
struct Error { ErrorCode code = ErrorCode::None; std::string message; };
struct Aabb { matter::Float3 min_m{}; matter::Float3 max_m{}; };
struct ParticleSample { matter::Float3 position_m{}; float radius_m = 0.0f; };
struct Limits {
    uint32_t max_particles = 0, max_grid_vertices = 0;
    uint32_t max_mesh_vertices = 0, max_mesh_indices = 0;
};
struct ParticleJob {
    const ParticleSample* particles = nullptr;
    uint32_t particle_count = 0;
    Aabb bounds_m{};
    float voxel_m = 0.0f, blend_width_m = 0.0f, iso_value = 0.0f;
    uint32_t material = 4;
    Limits limits{};
    uint64_t generation = 0;
};
struct BuildControl {
    std::function<bool()> cancelled;
    std::function<bool(uint64_t generation)> generation_is_current;
};
struct GridLayout {
    matter::Float3 origin_m{}, spacing_m{}, bin_origin_m{};
    std::array<uint32_t, 3> sample_dims{}, cell_dims{}, bin_dims{};
    uint32_t grid_vertices = 0, grid_cells = 0, bins = 0;
    float bin_size_m = 0.0f, query_radius_m = 0.0f;
};
struct MeshResult {
    std::vector<float> positions, normals;
    std::vector<uint32_t> indices;
    uint32_t material = 0;
    uint64_t content_digest = 0;
};
struct Stats {
    uint32_t particles = 0, bins = 0, grid_vertices = 0, grid_cells = 0;
    uint32_t active_cells = 0, triangles = 0;
    uint64_t device_bytes = 0;
    double bin_ms = 0, field_ms = 0, classify_ms = 0, emit_ms = 0;
};
bool validate_particle_job(const ParticleJob&, GridLayout&, Error&);
float evaluate_particle_field_reference(const ParticleSample*, uint32_t,
                                        float blend_width_m,
                                        matter::Float3 point_m);
bool exclusive_scan_reference(const std::vector<uint32_t>&,
                              std::vector<uint32_t>&, uint32_t& total);
uint64_t mesh_content_digest(const MeshResult&);
}
```

Derive `sample_dims[axis] = ceil(extent / voxel) + 1`, `cell_dims = sample_dims - 1`, and `spacing = extent / cell_dims`. Use the maximum particle radius as the reference radius and `query_radius = max_radius * 2.5 + blend_width * 4`, matching `surface.c`; an empty snapshot returns an empty mesh before bin allocation. Use `bin_size = query_radius`, expand the bin domain by one query radius around the scalar bounds, and center-bin every particle in that expanded domain so just-outside particles can still influence boundary samples. A field probe visits the complete integer bin range intersecting its query sphere. The reference field uses the same stable exponential smooth-min equation as `ProbeFieldScalar`.

- [x] **Step 4: Verify GREEN and the existing CPU graph**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --preset windows-msvc-relwithdebinfo -R 'gpu_visual_mesher_cpu_tests|surface_field_tests'
```

Expected: both tests pass and the new test prints `ALL PASS`.

- [x] **Step 5: Commit Task 1**

```powershell
git add MatterEngine3/include/matter/gpu_visual_meshing.h MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_common.cpp MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/Makefile
git commit -m "feat: define GPU visual meshing contracts"
```

### Task 2: Deterministic GPU scan and particle-bin stages

**Files:**
- Create: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.h`
- Create: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp`
- Create: `MatterEngine3/shaders_vk/gpu_mesh_common.glsl`
- Create: `MatterEngine3/shaders_vk/gpu_mesh_bin_count.comp`
- Create: `MatterEngine3/shaders_vk/gpu_mesh_bin_scatter.comp`
- Create: `MatterEngine3/shaders_vk/gpu_mesh_bin_sort.comp`
- Create: `MatterEngine3/shaders_vk/gpu_mesh_scan_blocks.comp`
- Create: `MatterEngine3/shaders_vk/gpu_mesh_scan_add.comp`
- Create: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.h`
- Create: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `cmake/manifests/engine-viewer.sources`
- Modify: `cmake/MatterViewer.cmake`
- Modify: `MatterEngine3/Makefile`

**Interfaces:**
- Consumes Task 1 job/layout/error types and `matter::VulkanDevice`, `VkBufferResource`, `create_buffer`, `upload_buffer`, `readback_buffer`, and `submit_immediate`.
- Produces `gpu_meshing::GpuVisualMesher`, with `bool build_particle_visual(const ParticleJob&, MeshResult&, Stats&, Error&, const BuildControl& = {})`.
- Test seam produces `run_gpu_visual_mesher_pure_vk_tests()` and `run_gpu_visual_mesher_vk_tests(matter::VulkanDevice&)` for the existing validation-enabled smoke executable.

- [x] **Step 1: Write failing GPU scan/bin tests**

Add a `MATTER_VK_SMOKE_MODE=gpu-mesher` branch to the smoke harness which calls the new functions. Tests upload `{3,0,2,5}`, assert GPU output `{0,3,3,5}` and total `10`, then exercise 257 and 65,537 elements so one-block and recursive block-sum paths are both required. Particle-bin tests use contributing negative-coordinate particles on bin faces and assert every contributing input id appears exactly once, offsets are monotonic, and every bin's ids are ascending after `gpu_mesh_bin_sort.comp`.

- [x] **Step 2: Verify RED**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
$env:MATTER_VK_SMOKE_MODE='gpu-mesher'; & 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe'
```

Expected: build fails because the new Vulkan mesher and shaders are absent.

- [x] **Step 3: Implement the hierarchical exclusive scan**

Use 256-value workgroups. `gpu_mesh_scan_blocks.comp` performs a Blelloch exclusive scan in shared memory and writes one sum per block. Recursively scan block sums until one block remains, then dispatch `gpu_mesh_scan_add.comp` from the highest populated level back to level zero. The host reads only the last output/count pair to calculate the total and rejects any `uint32_t` overflow.

The reusable internal call is:

```cpp
bool exclusive_scan_gpu(matter::VulkanDevice&, const matter::VkBufferResource& input,
                        uint32_t count, matter::VkBufferResource& output,
                        uint32_t& total, ScanScratch&, Error&,
                        const BuildControl&, uint64_t generation);
```

Each submitted stage uses `ImmediateSubmitPhase::compute_dispatch`; cancellation and `generation_is_current(job.generation)` are checked before every dispatch and after every completed submission. A superseded generation returns `ErrorCode::StaleGeneration`, distinct from user cancellation.

- [x] **Step 4: Implement bounded deterministic bins**

`gpu_mesh_bin_count.comp` atomically counts one center-bin entry per particle. Scan counts into offsets. `gpu_mesh_bin_scatter.comp` atomically fills the exact `particle_count` index buffer. `gpu_mesh_bin_sort.comp` runs one invocation per bin and insertion-sorts that bin's variable-length id range; it has no occupancy cap. The field stages therefore visit ids in stable order despite scatter scheduling.

- [x] **Step 5: Keep Windows and Make shader inventories identical**

Add every new `.comp.spv` to `VK_SPV` and explicit `.glsl` prerequisites for shaders including `gpu_mesh_common.glsl`. CMake discovers the stages and must continue rejecting inventory drift.

- [x] **Step 6: Verify GREEN**

Run the build and the `gpu-mesher` smoke mode again. Expected: scan/bin checks print `ALL PASS`, `validation errors: 0`, and repeat bin readbacks are byte-identical.

- [x] **Step 7: Commit Task 2**

```powershell
git add MatterEngine3/src/render/gpu_meshing MatterEngine3/shaders_vk/gpu_mesh_common.glsl MatterEngine3/shaders_vk/gpu_mesh_bin_count.comp MatterEngine3/shaders_vk/gpu_mesh_bin_scatter.comp MatterEngine3/shaders_vk/gpu_mesh_bin_sort.comp MatterEngine3/shaders_vk/gpu_mesh_scan_blocks.comp MatterEngine3/shaders_vk/gpu_mesh_scan_add.comp MatterEngine3/tests/gpu_visual_mesher_vk_tests.h MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp cmake/manifests/engine-viewer.sources cmake/MatterViewer.cmake MatterEngine3/Makefile
git commit -m "feat: add deterministic GPU meshing primitives"
```

### Task 3: GPU field evaluation parity

**Files:**
- Create: `MatterEngine3/shaders_vk/gpu_mesh_field.comp`
- Modify: `MatterEngine3/shaders_vk/gpu_mesh_common.glsl`
- Modify: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp`
- Modify: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`
- Modify: `MatterEngine3/Makefile`

**Interfaces:**
- Consumes sorted bin offsets/ids and `GridLayout`.
- Produces one finite signed scalar per grid vertex, or positive infinity when no particle lies within the MatterSurface query radius.

- [x] **Step 1: Write failing CPU/GPU field parity tests**

Build single-sphere, separated-sphere, and blended-sphere jobs, including a translated negative-coordinate fixture. Read the scalar buffer and compare every sample to both `evaluate_particle_field_reference` and `ProbeFieldScalar`. Require absolute error `<= 2e-5f` for finite samples and matching outside classification for infinite samples. Repeat dispatch and require byte-identical scalar buffers.

- [x] **Step 2: Verify RED**

Run the `gpu-mesher` smoke mode. Expected: field dispatch/report is absent and the new scalar assertions fail.

- [x] **Step 3: Implement the exact Phase 1 field**

For each lattice sample, enumerate all bins whose AABBs intersect `query_radius`, reject particle centers outside the actual radius, compute `distance(point, center) - radius`, track `fmin`, and evaluate:

```glsl
field = blendWidth <= 1e-5 || neighborCount == 1
    ? fmin
    : fmin - blendWidth * log(sum(exp(-(fi - fmin) / blendWidth)));
```

Use two deterministic passes through sorted ids when blending: one for `fmin` and one for the sum. Never truncate neighbors.

- [x] **Step 4: Verify GREEN and shader rebuild edges**

Run the smoke mode and `ctest -R 'gpu_visual_mesher_cpu_tests|shader_rebuild_tests'`. Expected: parity/determinism pass and touching `gpu_mesh_common.glsl` rebuilds every dependent GPU-mesh stage.

- [x] **Step 5: Commit Task 3**

```powershell
git add MatterEngine3/shaders_vk/gpu_mesh_field.comp MatterEngine3/shaders_vk/gpu_mesh_common.glsl MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp MatterEngine3/Makefile
git commit -m "feat: evaluate Matter particle fields on Vulkan"
```

### Task 4: Marching-cubes classification, compaction, emission, normals, and failure closure

**Files:**
- Create: `MatterEngine3/shaders_vk/gpu_mesh_mc_tables.glsl`
- Create: `MatterEngine3/shaders_vk/gpu_mesh_classify.comp`
- Create: `MatterEngine3/shaders_vk/gpu_mesh_compact.comp`
- Create: `MatterEngine3/shaders_vk/gpu_mesh_emit.comp`
- Modify: `MatterEngine3/shaders_vk/gpu_mesh_common.glsl`
- Modify: `MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp`
- Modify: `MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp`
- Modify: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`
- Modify: `MatterEngine3/Makefile`

**Interfaces:**
- Consumes scalar lattice, sorted particle bins, layout, isolevel, and declared output limits.
- Produces deterministic triangle-soup positions/normals/indices and exact active-cell/triangle totals.

- [x] **Step 1: Write failing table, topology, overflow, cancellation, and repeatability tests**

Keep `libs/MatterSurfaceLib/src/mc_tables.h` as the single authored 256x16 triangle table, validate every row before use, and upload it as a signed-int storage buffer. `gpu_mesh_mc_tables.glsl` defines only the matching corner/edge ABI, avoiding a second 4,096-entry table that could drift. GPU tests require one sphere, two separated spheres, and two blended spheres to be non-empty, finite, index-valid, consistently wound, and byte-identical across two builds. Compare analytic normals away from degenerate gradients with dot product `>= 0.999`. Configure limits one below the required vertex/index count and require `ErrorCode::LimitExceeded` with an empty result. Cancellation and stale-generation checks are threaded through every dispatch boundary. Run allocation, upload, dispatch, readback, and device-lost fault injection and require an empty result plus the corresponding stable error category, with no validation errors or leaked tracked allocations.

- [x] **Step 2: Verify RED**

Run CPU and GPU focused tests. Expected: table file and emitted meshes are absent.

- [x] **Step 3: Classify and compact active cells**

`gpu_mesh_classify.comp` writes cube case, active flag, and triangle count for every cell. Scan active flags and compact stable cell ids with `gpu_mesh_compact.comp`. Separately scan triangle counts to assign stable per-cell triangle offsets. Read totals before allocating/emitting output; reject capacity without dispatching emission.

- [x] **Step 4: Emit deterministic triangle soup and analytic normals**

`gpu_mesh_emit.comp` dispatches only compacted active cells. It uses the canonical corner/edge numbering from `surface.c`, writes three unique vertices and sequential indices per triangle, reverses the table order exactly as the CPU path does, interpolates at the authored isolevel, and evaluates the analytic smooth-min gradient from the same sorted bins. Degenerate gradients use `(0,1,0)`.

- [x] **Step 5: Finalize and read back transactionally**

Read buffers into temporary vectors, strip `vec4` padding into three-float vectors, validate every finite value/index, calculate `content_digest`, and move into the caller's `MeshResult` only after all validation succeeds. Catch allocation exceptions and return a stable error; no exception crosses the bake boundary.

- [x] **Step 6: Verify GREEN and CPU surface acceptance**

For the exact single-sphere oracle, require every GPU vertex to remain within `voxel_m * 0.25f` of the authored CPU SDF isosurface, every face to wind outward, and analytic-normal dot product to be `>= 0.999`. Field parity against `ProbeFieldScalar` covers the same lattice for hard, separated, blended, and translated fixtures. Run the GPU test twice and compare complete result vectors/digest.

- [x] **Step 7: Commit Task 4**

```powershell
git add MatterEngine3/shaders_vk/gpu_mesh_mc_tables.glsl MatterEngine3/shaders_vk/gpu_mesh_classify.comp MatterEngine3/shaders_vk/gpu_mesh_compact.comp MatterEngine3/shaders_vk/gpu_mesh_emit.comp MatterEngine3/shaders_vk/gpu_mesh_common.glsl MatterEngine3/src/render/gpu_meshing/gpu_visual_mesher_vk.cpp MatterEngine3/tests/gpu_visual_mesher_cpu_tests.cpp MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp MatterEngine3/Makefile
git commit -m "feat: extract deterministic GPU water meshes"
```

### Task 5: Coarse CPU fallback and versioned three-product artifact

**Files:**
- Create: `MatterEngine3/src/hydrology/hydrology_artifact.h`
- Create: `MatterEngine3/src/hydrology/hydrology_artifact.cpp`
- Create: `MatterEngine3/src/hydrology/water_visual_products.h`
- Create: `MatterEngine3/src/hydrology/water_visual_products.cpp`
- Create: `MatterEngine3/tests/hydrology_artifact_tests.cpp`
- Modify: `cmake/manifests/engine-core.sources`
- Modify: `cmake/MatterEngine.cmake`
- Modify: `MatterEngine3/Makefile`

**Interfaces:**
- Consumes `ParticleJob`, accepted GPU `MeshResult`, and stable particle order.
- Produces `hydrology::HydrologyArtifact` with `visual_mesh`, `coarse_cpu_mesh`, and `gameplay_field` as independent owned products.
- Produces `build_cpu_particle_visual`, `serialize_artifact`, `deserialize_artifact`, `save_artifact_atomic`, and `load_artifact_validated`.

- [x] **Step 1: Write failing artifact/fallback tests**

Tests require: coarse CPU mesh from the same particles; visual-only setting changes do not change the CPU/gameplay keys; round-trip exact bytes; corrupt/truncated/oversized payload rejection; invalid indices/non-finite mesh rejection; semantic key and embedded payload digest verification; atomic replace; second load returns the same mesh without invoking a supplied mesher callback; and visual/coarse/gameplay products coexist without sharing vector storage or topology.

- [x] **Step 2: Verify RED**

Build `hydrology_artifact_tests`. Expected: target and headers do not exist.

- [x] **Step 3: Implement coarse CPU fallback**

Convert samples to MatterSurfaceLib `Particle`, choose the smallest cubic `Bounds` and power-of-two grid whose cell size is not finer than the requested coarse voxel, call `GenerateMeshWithScratch`, recompute analytic normals, widen 16-bit indices to `uint32_t`, validate, and free CPU mesh arrays. The GPU-disabled caller invokes this function directly and never links the Vulkan implementation.

- [x] **Step 4: Implement the versioned artifact**

Use fixed magic `MHYDMSH1`, little-endian scalar encoding, explicit byte counts before each array, maximum 512 MiB payload, and a digest over exact serialized product bytes. The visual identity encodes particle snapshot digest, bounds, voxel/radius/blend/isolevel values, field/extraction/output contract versions, every compiled GPU-mesh SPIR-V digest, and explicit normal/smoothing/anisotropy settings; GPU vendor/device/driver are recorded as provenance rather than semantic inputs. Encode product-specific keys, mesh material, positions/normals/indices, and gameplay samples `(height, depth, velocity xyz, wet-valid byte)`. Publish a closed temporary file and atomically replace the semantic-key root only after reopening and validating the payload.

- [x] **Step 5: Verify GREEN**

Run `ctest -R 'hydrology_artifact_tests|gpu_visual_mesher_cpu_tests|surface_field_tests'`. Expected: all pass and corrupt fixture tests fail closed without files outside their temporary cache root.

- [x] **Step 6: Commit Task 5**

```powershell
git add MatterEngine3/src/hydrology/hydrology_artifact.h MatterEngine3/src/hydrology/hydrology_artifact.cpp MatterEngine3/src/hydrology/water_visual_products.h MatterEngine3/src/hydrology/water_visual_products.cpp MatterEngine3/tests/hydrology_artifact_tests.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake MatterEngine3/Makefile
git commit -m "feat: persist GPU water mesh products"
```

### Task 6: Renderer ownership, ordinary glass part conversion, and bake seam

**Files:**
- Create: `MatterEngine3/src/render/gpu_meshing/water_scene_part.h`
- Create: `MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp`
- Create: `MatterEngine3/tests/gpu_water_render_tests.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `cmake/manifests/engine-viewer.sources`
- Modify: `cmake/MatterViewer.cmake`
- Modify: `MatterEngine3/Makefile`

**Interfaces:**
- `VkSceneRenderer` owns one `GpuVisualMesher` and exposes a thin `build_particle_visual` call with no raw Vulkan handles.
- `build_water_scene_part(const MeshResult&, uint64_t artifact_digest, std::shared_ptr<const viewer::VkScenePart>&, uint64_t& instance_id, Error&)` validates and converts accepted output to the normal indexed/static renderer format.
- `LocalProvider::Config::vk_particle_visual_bake` is the renderer callback seam that later PhysX orchestration posts through `GpuJobQueue`; it is null in headless mode.

- [x] **Step 1: Write failing render-contract tests**

Test a triangle mesh and require one cluster/LOD, exact positions/normals/uint32 indices, digest-derived stable part and instance identities, material index `4` on every vertex, exact bounds, dry mesh omission, rejection of malformed meshes, and unchanged old binding when replacement upload fails.

- [x] **Step 2: Verify RED**

Build `gpu_water_render_tests`. Expected: converter and callback seam are absent.

- [x] **Step 3: Implement trusted conversion and renderer ownership**

Construct ordinary `VkRasterVertex` entries with white tint and canonical glass material 4, one cluster spanning the full index buffer, identity instance transform, and digest-derived nonzero ids. `VkSceneRenderer::init` creates the mesher only after the Vulkan device is initialized; destruction drains submitted immediate work before the member releases buffers/pipelines.

- [x] **Step 4: Wire the callback without starting a bake**

Configure `vk_particle_visual_bake` beside `vk_tileset_bake` in both initial and reload setup paths. The callback fails with `ErrorCode::Unavailable` when no Vulkan renderer is active. No current world invokes it; the PhysX phase supplies the final particle snapshot and schedules the call through the existing GPU job queue.

- [x] **Step 5: Verify GREEN and registration census**

Run `ctest -R 'gpu_water_render_tests|editor_registration_census|viewer_graph_tests'`. Expected: converter tests pass and the editor census remains exact apart from the intentionally registered synthetic test part.

- [x] **Step 6: Commit Task 6**

```powershell
git add MatterEngine3/src/render/gpu_meshing/water_scene_part.h MatterEngine3/src/render/gpu_meshing/water_scene_part.cpp MatterEngine3/tests/gpu_water_render_tests.cpp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/provider/local_provider.h MatterEngine3/src/matter_engine.cpp cmake/manifests/engine-viewer.sources cmake/MatterViewer.cmake MatterEngine3/Makefile
git commit -m "feat: connect GPU water meshes to the renderer"
```

### Task 7: Synthetic PBF acceptance, cache reload, and three-angle visual evidence

**Files:**
- Create: `MatterEngine3/tests/fixtures/gpu_mesher_synthetic_pbf.h`
- Modify: `MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `docs/superpowers/specs/2026-08-22-gpu-visual-meshing-foundation-design.md`

**Interfaces:**
- Produces a stable synthetic flowing-water particle snapshot with stable ids and a narrow acceptance-only editor injection selected by `MATTER_GPU_MESHER_ACCEPTANCE_ARTIFACT=<absolute path>`.
- The editor injection loads a validated serialized artifact and installs its visual mesh as one glass part; it never runs the GPU mesher on level load.

- [x] **Step 1: Write failing end-to-end acceptance tests**

The GPU smoke test bakes the fixture, creates coarse/gameplay products, saves the artifact, reloads it, proves the mesher dispatch counter does not change on reload, converts the visual mesh to a glass part, and verifies visual/coarse/gameplay topology independence. Register the part through `VkSceneRenderer`, prove it participates in raster geometry and native-RT BLAS/TLAS registration, and require zero validation errors. A second bake on the same device must reproduce all visual buffers and the digest byte-for-byte.

- [x] **Step 2: Verify RED**

Run the `gpu-mesher` smoke mode. Expected: synthetic fixture/cache-reload assertions fail.

- [x] **Step 3: Implement the acceptance-only artifact injection**

At editor setup, when the variable is present, load only the explicit absolute artifact, validate it, convert/install it through the normal renderer path, and append one identity instance to the world. Any validation error reports a precise diagnostic and leaves the dry world usable. With the variable absent, product behavior is byte-identical and no artifact path is opened.

- [x] **Step 4: Capture three angles through the normal app**

Bake the synthetic artifact once, then launch `RiverHydrology` three times with the artifact variable and scripted cameras for overview, curve, and low-water views. Use `MatterEngine3/tools/drive.py` with `wait_event bake.finished`, `wait_idle`, `cam`, `shot`, and `quit`; store PNGs under `MatterEditor/build/baselines/msvc/gpu-mesher-acceptance/`. Confirm the mesh uses glass shading, is smooth/connected, has no capacity truncation, and remains identical after an editor restart.

- [x] **Step 5: Record measured acceptance and spec status**

Change the design status from draft to accepted/implemented Phase 1 and record GPU model, driver, grid/particle/triangle counts, timings, surface-distance maximum, repeat digest, screenshot paths, and the explicit statement that terrain T0-T3 has not started because the spec gates it after fluid-prerequisite acceptance.

- [x] **Step 6: Commit Task 7**

```powershell
git add MatterEngine3/tests/fixtures/gpu_mesher_synthetic_pbf.h MatterEngine3/tests/gpu_visual_mesher_vk_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/src/matter_engine.cpp docs/superpowers/specs/2026-08-22-gpu-visual-meshing-foundation-design.md
git commit -m "test: accept the GPU water meshing foundation"
```

### Task 8: Full verification, package safety, review, and handoff

**Files:**
- Modify only files required by failures with a new failing regression test first.

**Interfaces:**
- Produces the final accepted MSVC editor/package and preserves the Linux/headless rollback build inventories.

- [x] **Step 1: Run focused tests from clean outputs**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_visual_mesher_cpu_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target hydrology_artifact_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target gpu_water_render_tests
tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests
$env:MATTER_VK_SMOKE_MODE='gpu-mesher'; & 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe'
```

Expected: every command exits zero, GPU mode prints `ALL PASS`, and validation errors remain zero.

- [x] **Step 2: Run the full MSVC suite**

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' --preset windows-msvc-relwithdebinfo
```

Expected: all tests pass with zero failures.

- [x] **Step 3: Verify editor/package and clean runtime environment**

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_dist
tools/check-windows-msvc-package.ps1 -DistPath MatterEditor/build/dist/world_demo
```

Expected: package checker accepts the package, no MinGW runtime is staged, no new runtime DLL is required, and the artifact acceptance load works from the packaged editor with developer paths removed.

- [x] **Step 4: Verify compiler-neutral inventory**

From WSL run the existing GCC shader/headless build and the focused CPU tests. Expected: shader inventory agrees with CMake, the headless build never references Vulkan mesher symbols, and CPU fallback/artifact tests pass.

- [x] **Step 5: Review the diff against every Phase 1 acceptance gate**

Check: CPU-disabled behavior; GPU field parity; hierarchical scan; deterministic bins/output; overflow closure; cancellation/stale generation; same-device repeat; coarse CPU fallback; three independent artifact products; restart cache hit; glass rendering from three angles; precise unavailable diagnostics; no terrain switch; no PhysX/CUDA dependency.

- [x] **Step 6: Commit final corrections and use the branch-finishing workflow**

```powershell
git status --short
git diff --check
git log --oneline --decorate -12
```

After fresh verification evidence, invoke `superpowers:finishing-a-development-branch` and present the branch integration choices without merging or pushing unless the user selects one.
