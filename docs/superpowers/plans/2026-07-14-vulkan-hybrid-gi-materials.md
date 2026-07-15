# Vulkan Hybrid GI and Material-Driven Ray Tracing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a demoable Vulkan hybrid renderer with material-selected diffuse GI, GGX reflection, glass/water transmission, foliage scattering, emissive lighting, baked AO, temporal denoising, and DLSS Super Resolution at an initial target of 60 FPS at 2560x1440 output.

**Architecture:** Rasterization remains the primary-visibility and GPU-instancing path. A Vulkan KHR ray pipeline reads the raster G-buffer, uses a device-addressable part table to shade secondary hits, emits separate noisy lighting signals, and iteratively traces a bounded material-selected path. Engine-owned temporal/A-trous filtering produces pre-tonemap HDR for Streamline DLSS SR.

**Tech Stack:** C++17, C11 material registry, Vulkan 1.3, GLSL 4.60 compiled by `glslc`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_acceleration_structure`, GPU buffer device address, Streamline 2.12 DLSS SR, PowerShell/MSYS2 Windows build and smoke tools.

## Global Constraints

- Preserve raster primary visibility, canonical Vulkan zero-to-one matrices, GPU instancing, depth, and motion vectors.
- Material lobe weights are value-driven; flags are limited to `THIN_WALLED`, `DOUBLE_SIDED`, `ALPHA_TESTED`, and `VOLUME_BOUNDARY`.
- Existing material indices stay stable and registry additions remain append-only.
- `MaterialRegistryPackForGPU` remains the 12-float legacy OpenGL pack; Vulkan RTX uses a new aligned 144-byte `MaterialGpuRecord` pack.
- Existing glass, green-glass, and water migrate legacy translucency to transmission; leaf uses explicit thin-walled subsurface scattering.
- Baked AO is the default and affects indirect diffuse/ambient only. Missing AO is `1.0`.
- The initial integrator uses one stochastic continuation per shaded pixel, at most two indirect bounces, and Russian roulette after the first bounce.
- DLSS receives denoised pre-tonemap HDR; it is not the Monte Carlo denoiser.
- No CPU readback, immediate submit, `vkDeviceWaitIdle`, or new CPU/GPU wait is allowed in the normal frame path.
- All geometry addresses, signal images, history images, descriptor sets, and pipelines obey the existing frame-fence lifetime contract.
- Unsupported RTX selects the existing raster fallback; unavailable DLSS selects the native composite fallback.
- CUDA-enabled Windows builds use `HAVE_CUDA=1`; the final build also uses `HAVE_STREAMLINE=1` and copies signed Streamline/DLSS DLLs beside `MatterViewer/viewer.exe`.

---

## File Structure

### New focused files

- `MatterEngine3/src/render/vk_gi_contract.h` - CPU-visible flags, GPU records, settings, counters, and layout assertions.
- `MatterEngine3/src/render/vk_gi_math.h/.cpp` - tested Fresnel, refraction, absorption, and lobe-probability reference functions.
- `MatterEngine3/shaders_vk/material_common.glsl` - the Vulkan raster/RT material record and shared evaluation helpers.
- `MatterEngine3/shaders_vk/rt_surface_common.glsl` - device-address geometry fetch and `RtSurface` construction.
- `MatterEngine3/shaders_vk/rt_lighting.rgen` - iterative material-driven path integrator.
- `MatterEngine3/shaders_vk/rt_radiance.rmiss` and `rt_surface.rchit` - environment miss and reusable secondary surface query.
- `MatterEngine3/shaders_vk/rt_visibility.rmiss`, `rt_visibility.rchit`, and `rt_visibility.rahit` - opaque/alpha/transmission-aware visibility.
- `MatterEngine3/shaders_vk/gi_temporal.comp` - reprojection, rejection, moments, and history length.
- `MatterEngine3/shaders_vk/gi_atrous.comp` - signal-specific spatial filtering.
- `MatterEngine3/shaders_vk/gi_combine.comp` - filtered-signal composition, atmosphere, and DLSS masks.

### Existing orchestration files

- `MatterEngine3/src/render/vk_scene_renderer.h/.cpp` owns scene uploads, descriptors, RT/SBT creation, per-frame recording, signal/history resources, and statistics.
- `MatterEngine3/src/render/raster_mesh.h/.cpp` preserves material index, UV, tint, and baked AO from `TriEx`.
- `MatterEngine3/src/matter_engine.cpp` converts loaded parts and exposes settings/stats to the world session.
- `MatterViewer/main.cpp` exposes runtime controls/debug scenes and reports active quality/timing state.
- Both Makefiles enumerate and embed every new SPIR-V shader explicitly.

---

### Task 1: Versioned Material Schema and RTX GPU Pack

**Files:**
- Create: `MatterEngine3/src/render/vk_gi_contract.h`
- Modify: `MatterSurfaceLib/include/material_registry.h`
- Modify: `MatterSurfaceLib/src/material_registry.c`
- Modify: `MatterSurfaceLib/tests/material_registry_tests.cpp`
- Modify: `MatterEngine3/src/part_asset_v2.h`
- Modify: `MatterEngine3/tests/part_asset_v2_tests.cpp`
- Modify: `MatterEngine3/src/part_asset_v2.cpp`

**Interfaces:**
- Produces: `MaterialSurfaceFlags`, `MaterialGpuRecord`, `MaterialRegistryPackRtForGPU(MaterialGpuRecord*)`, `MaterialRegistrySchemaVersion()`, `PartAssetLoadFailure`, and safe artifact rejection on schema mismatch.
- Preserves: `MaterialRegistryPackForGPU(float*)` and `MATERIAL_FLOATS_PER_DEF == 12` for the OpenGL backend.

- [ ] **Step 1: Write failing material layout and migration tests**

Add checks equivalent to:

```cpp
MaterialGpuRecord records[64]{};
MaterialRegistryPackRtForGPU(records);
CHECK(sizeof(MaterialGpuRecord) == 144, "RTX material record is 9x vec4");
CHECK(records[4].transmission[0] > 0.0f, "glass opts into transmission");
CHECK((records[4].flags_misc[0] & MATERIAL_VOLUME_BOUNDARY) != 0,
      "glass is a closed volume");
CHECK(records[7].transmission[1] > 1.32f &&
      records[7].transmission[1] < 1.34f, "water IOR is preserved");
CHECK(records[15].scattering[3] > 0.0f &&
      (records[15].flags_misc[0] & MATERIAL_THIN_WALLED) != 0,
      "leaf opts into thin scattering");
CHECK(MaterialIsTransparent(15) == 0,
      "subsurface leaf does not become a meshing carve volume");
```

In `part_asset_v2_tests.cpp`, serialize a header/body with the prior schema
version and assert load failure reports `material schema mismatch; rebake`.

- [ ] **Step 2: Run tests and verify RED**

Run:

```bash
make -C MatterSurfaceLib/tests run-reg
make -C MatterEngine3/tests run-partv2
```

Expected: compile failure for `MaterialGpuRecord`, flags, and schema accessors.

- [ ] **Step 3: Implement the source and packed layouts**

Append the new shading properties to `MaterialDef` while retaining legacy
`translucency` solely for old OpenGL shading and CPU carve compatibility. Define
the flags in `material_registry.h` so C and C++ callers share their values, and
append these fields exactly after the existing authoring/meshing fields:

```c
float opacity;
float transmission;
float emissionColor[3];
float absorptionColor[3];
float absorptionDistance;
float thickness;
float subsurface;
float scatteringColor[3];
float scatteringDistance;
float anisotropy;
float clearcoat;
float clearcoatRoughness;
float specularStrength;
float specularTint[3];
float alphaCutoff;
float shadowOpacity;
uint32_t surfaceFlags;
```

`vk_gi_contract.h` includes `material_registry.h` and adds Vulkan-only settings,
counters, and C++ layout assertions; it does not duplicate the material flags.
Define the Vulkan record exactly as:

```c
typedef struct MaterialGpuRecord {
    float base_roughness[4];       /* albedo.rgb, roughness */
    float metal_opacity_spec_coat[4]; /* metallic, opacity, specular, clearcoat */
    float specular_tint_coat_roughness[4]; /* specular tint.rgb, coat roughness */
    float emission_strength[4];    /* emission.rgb, strength */
    float transmission[4];         /* weight, IOR, thickness, absorption distance */
    float absorption_pad[4];       /* absorption.rgb, reserved */
    float scattering[4];           /* scattering.rgb, subsurface weight */
    float scattering_shape[4];     /* distance, anisotropy, alpha cutoff, shadow opacity */
    uint32_t flags_misc[4];        /* flags, ground tileset slot, reserved, reserved */
} MaterialGpuRecord;
```

Add `MATERIAL_SCHEMA_VERSION = 2`. Use explicit registry initializers for every
material. Glass/green glass/water receive transmission and volume values; leaf
receives scattering plus `THIN_WALLED | DOUBLE_SIDED`; all other new weights
default to zero. Keep `MaterialIsTransparent` based on legacy carve
translucency, not subsurface.

- [ ] **Step 4: Add diagnostic artifact invalidation and verify GREEN**

Write/read the material schema version immediately before the material count in
the common part body. Add:

```cpp
enum class PartAssetLoadFailure { None, Header, MaterialSchema, CorruptBody };
bool load_v2(/* existing arguments */,
             PartAssetLoadFailure* failure = nullptr,
             std::string* reason = nullptr);
```

Initialize outputs to `None`/empty. A mismatch returns false with
`PartAssetLoadFailure::MaterialSchema` and exact reason
`material schema mismatch; rebake`; do not consume bytes using the new struct
size. Existing call sites use the default arguments. Run both commands from
Step 2 and expect all tests to pass.

- [ ] **Step 5: Commit**

```bash
git add MatterSurfaceLib/include/material_registry.h MatterSurfaceLib/src/material_registry.c \
  MatterSurfaceLib/tests/material_registry_tests.cpp MatterEngine3/src/render/vk_gi_contract.h \
  MatterEngine3/src/part_asset_v2.h MatterEngine3/src/part_asset_v2.cpp \
  MatterEngine3/tests/part_asset_v2_tests.cpp
git commit -m "feat(materials): add value-driven RTX properties"
```

---

### Task 2: Shared Raster Material Identity and G-buffer Contract

**Files:**
- Modify: `MatterEngine3/src/render/raster_mesh.h`
- Modify: `MatterEngine3/src/render/raster_mesh.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Create: `MatterEngine3/shaders_vk/material_common.glsl`
- Modify: `MatterEngine3/shaders_vk/raster.vert`
- Modify: `MatterEngine3/shaders_vk/gbuffer.frag`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: `MaterialGpuRecord` from Task 1.
- Produces: `VkRasterVertex {position, normal, tint, surface, material_index, pad}`, integer-exact material/instance G-buffer attachment, a shared material SSBO used by raster and later RT shaders, and `VkSceneRenderer::update_materials(records, shading_revision, geometry_revision, error)`.

- [ ] **Step 1: Write failing mesh and G-buffer tests**

Construct one triangle with material 7, distinct UVs/tints, and AO values
`0.2, 0.5, 0.8`. Assert `build_raster_mesh_data` retains parallel `uvs`,
`material_ids`, and AO arrays. Extend the Vulkan readback test:

```cpp
CHECK(center.material_index == 7u, "G-buffer retains exact material id");
CHECK(center.instance_token != 0u, "draw writes stable instance history token");
CHECK(std::fabs(center.orm.z - 0.5f) < 0.01f, "baked AO survives interpolation");
```

Update only glass absorption with a new shading revision and assert the shared
material buffer changes without a part geometry upload. Then change its
`ALPHA_TESTED` flag with a new geometry revision and assert the renderer reports
that affected RT geometry classification is dirty and GI history needs reset.

- [ ] **Step 2: Run the strict Vulkan smoke build and verify RED**

Run from MSYS2:

```bash
make -C MatterViewer build/windows/vulkan_smoke_tests.exe HAVE_CUDA=1 HAVE_STREAMLINE=1
```

Expected: missing vertex fields, attachment, descriptor, and readback members.

- [ ] **Step 3: Implement one authoritative material evaluation path**

Change the Vulkan vertex to store tint rather than pre-resolved albedo:

```cpp
struct VkRasterVertex {
    matter::Float3 position;
    matter::Float3 normal;
    matter::Float4 tint;       // rgb tint, a blend strength
    matter::Float4 surface;    // uv.xy, bakedAO, AO-valid marker
    uint32_t material_index;
    uint32_t pad[3];
};
```

Preserve the existing `RasterMeshData::texcoords` material/AO channel for the
OpenGL consumer and add Vulkan source arrays:

```cpp
std::vector<float> surface_uvs;       // two floats per vertex from TriEx uv0/1/2
std::vector<uint32_t> material_ids;   // one flat id per vertex
std::vector<float> baked_ao;          // one value per vertex, default 1
```

`build_raster_mesh_data` fills all three arrays from each `TriEx`; when `TriEx`
is absent it writes UV `(0,0)`, invalid material, and AO `1.0`. The Vulkan part
conversion consumes these arrays while the OpenGL path remains unchanged.

Upload `MaterialGpuRecord[]` to a storage buffer. In
`material_common.glsl`, declare the exact eight-vec4 record and implement:

```glsl
vec3 resolveBaseColor(MaterialGpu m, vec4 tint) {
    return mix(m.base_roughness.rgb, tint.rgb, clamp(tint.a, 0.0, 1.0));
}
```

Raster vertex/fragment shaders pass a flat `uint material_index`, read the same
SSBO, and write base color, normal, ORM/AO, velocity, and `uvec2(material,
instance_token)` to `VK_FORMAT_R32G32_UINT`. Background clears to
`uvec2(0xffffffffu)`.

`update_materials` uploads scalar/color-only revisions through the frame command
buffer and requests one GI-history reset. A changed geometry revision marks
parts whose used-material classification changed for safe BLAS reclassification
in Task 4; it does not rebuild unrelated parts.

- [ ] **Step 4: Regenerate SPIR-V and verify GREEN**

Add include dependencies so changing `material_common.glsl` rebuilds every
consumer. Run:

```bash
make -C MatterEngine3 vulkan-spirv
make -C MatterViewer vulkan-smoke HAVE_CUDA=1 HAVE_STREAMLINE=1
```

Expected: material/tint/AO assertions pass and Vulkan validation count is zero.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/raster_mesh.* MatterEngine3/src/render/vk_scene_renderer.* \
  MatterEngine3/src/matter_engine.cpp MatterEngine3/shaders_vk/material_common.glsl \
  MatterEngine3/shaders_vk/raster.vert MatterEngine3/shaders_vk/gbuffer.frag \
  MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): share material identity with the G-buffer"
```

---

### Task 3: Device-Address Part Table and Reusable Secondary Surface Query

**Files:**
- Modify: `MatterEngine3/src/render/vk_gi_contract.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Create: `MatterEngine3/shaders_vk/rt_surface_common.glsl`
- Create: `MatterEngine3/shaders_vk/rt_surface.rchit`
- Create: `MatterEngine3/shaders_vk/rt_radiance.rmiss`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: pinned `PartRecord::rt_geometry`, TLAS `instanceCustomIndex`, shared material table.
- Produces: `GpuRtPartRecord`, `RtSurface`, radiance hit/miss SBT groups, and a test-only one-ray surface-query readback.

- [ ] **Step 1: Write failing secondary-hit identity test**

Create two parts and two transformed instances. Trace fixed rays to each and
require:

```cpp
CHECK(hit0.valid && hit0.part_slot == slot0 && hit0.primitive == 0,
      "first ray identifies part and primitive");
CHECK(hit1.valid && hit1.material_index == 7,
      "second ray fetches material from pinned geometry");
CHECK(close3(hit1.normal, expected_world_normal, 1e-4f),
      "inverse-transpose normal is correct");
CHECK(std::fabs(hit1.baked_ao - expected_ao) < 1e-4f,
      "secondary barycentric AO matches raster data");
```

- [ ] **Step 2: Verify RED**

Build/run the `rt-enabled` Vulkan smoke mode. Expected: no part-address table or
radiance hit group exists.

- [ ] **Step 3: Implement stable address records and GLSL fetch**

Define:

```cpp
struct alignas(16) GpuRtPartRecord {
    uint64_t vertex_address;
    uint32_t vertex_stride;
    uint32_t vertex_count;
    uint32_t primitive_count;
    uint32_t valid;
    uint32_t pad[2];
};
static_assert(sizeof(GpuRtPartRecord) == 32);
```

Upload one record per stable part slot. `rt_surface_common.glsl` uses
`GL_EXT_buffer_reference2`, `gl_InstanceCustomIndexEXT`, `gl_PrimitiveID`, and
hit barycentrics to load three `VkRasterVertex` records and construct:

```glsl
struct RtSurface {
    vec3 position; float hit_t;
    vec3 normal; uint material_index;
    vec4 tint;
    vec2 uv; float baked_ao; uint flags;
};
```

Transform normals with the object-to-world inverse transpose and orient them
using front-face state without discarding the geometric side needed by volume
logic. An invalid part-table record returns an invalid magenta/debug surface in
test mode and increments an error counter in production. Slot reuse occurs only
after every frame fence referencing the previous TLAS/part table has completed.

- [ ] **Step 4: Extend RT pipeline/SBT and verify GREEN**

Add radiance miss and surface closest-hit groups while retaining the existing
shadow group. Query and honor handle/stride/base alignment. Retain the part
table and every referenced geometry lifetime through the submitted frame.
Run `rt-enabled`, `rt-unavailable`, and two-frame resize modes; expect all hit
identity assertions and zero validation errors.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_gi_contract.h MatterEngine3/src/render/vk_scene_renderer.* \
  MatterEngine3/shaders_vk/rt_surface_common.glsl MatterEngine3/shaders_vk/rt_surface.rchit \
  MatterEngine3/shaders_vk/rt_radiance.rmiss MatterEngine3/Makefile MatterViewer/Makefile \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): expose materials at secondary RTX hits"
```

---

### Task 4: Alpha- and Transmission-Aware Visibility

**Files:**
- Create: `MatterEngine3/shaders_vk/rt_visibility.rmiss`
- Create: `MatterEngine3/shaders_vk/rt_visibility.rchit`
- Create: `MatterEngine3/shaders_vk/rt_visibility.rahit`
- Modify: `MatterEngine3/shaders_vk/rt_shadow.rgen`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: `RtSurface`, material opacity/alpha cutoff/shadow opacity/flags and the geometry-revision dirty set from Task 2.
- Produces: RGB visibility payload and per-part opaque/non-opaque BLAS classification.

- [ ] **Step 1: Write failing visibility fixture**

Trace through three aligned triangles: opaque, constant-alpha-cutout, and
shadow-transmitting glass. Assert opaque visibility is zero, cutout visibility
remains one when opacity is below cutoff, and two 50% transmitting layers
produce approximately `vec3(0.25)` before tint.

- [ ] **Step 2: Verify RED**

Run the RT smoke. Expected: current forced-opaque path reports zero for every
intersection.

- [ ] **Step 3: Implement fast opaque and any-hit paths**

Classify each part as opaque only when all used materials have opacity one,
shadow opacity one, and neither `ALPHA_TESTED` nor transmission. Preserve
`VK_GEOMETRY_OPAQUE_BIT_KHR` and `gl_RayFlagsOpaqueEXT` for that class. For the
non-opaque group, any-hit performs:

```glsl
if ((m.flags & MATERIAL_ALPHA_TESTED) != 0u && opacity < m.alphaCutoff) {
    ignoreIntersectionEXT;
    return;
}
visibility *= mix(vec3(1.0), transmissionTint, m.shadowOpacity);
if (max(visibility.r, max(visibility.g, visibility.b)) < 0.01)
    terminateRayEXT;
else
    ignoreIntersectionEXT;
```

Use closest-hit as the opaque terminating fallback. Cap continued layers at 32
in the payload to prevent pathological loops.

When a geometry revision changes classification, rebuild only affected BLAS
geometry flags after the old BLAS lifetime is safely retired, rebuild the TLAS
reference, and reset GI history. Scalar/color material edits continue to use
the existing BLAS and only update the shared material buffer.

- [ ] **Step 4: Verify and commit**

Compile SPIR-V and run RT enabled/disabled/unavailable smoke modes. Expected:
visibility fixture passes, opaque scene ray counts do not invoke any-hit, and
validation is zero.

```bash
git add MatterEngine3/shaders_vk/rt_visibility.* MatterEngine3/shaders_vk/rt_shadow.rgen \
  MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/Makefile MatterViewer/Makefile \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): trace material-aware visibility"
```

---

### Task 5: Unified One-Bounce Diffuse GI and Environment

**Files:**
- Create: `MatterEngine3/src/render/vk_gi_math.h`
- Create: `MatterEngine3/src/render/vk_gi_math.cpp`
- Create: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/shaders_vk/rt_radiance.rmiss`
- Modify: `MatterEngine3/shaders_vk/composite.frag`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Produces: `VulkanGiSettings`, raw diffuse GI image, environment miss, deterministic RNG, and one-bounce integrator dispatch.
- Initial settings: enabled, `max_bounces=1`, `samples_per_pixel=1`, `trace_scale=1.0`, GI/reflection/transmission/scattering multipliers.

- [ ] **Step 1: Write failing math and reference-scene tests**

Add CPU checks for orthonormal cosine sampling and a fixed-seed two-plane GPU
scene: a white receiver above a red floor must gain positive red indirect
radiance; disabling GI must produce zero raw diffuse. Require baked AO zero to
suppress indirect diffuse but leave direct visibility unchanged.

- [ ] **Step 2: Verify RED**

Run the strict RT smoke executable. Expected: missing GI settings/resources and
raw-diffuse readback APIs.

- [ ] **Step 3: Implement the bounded raygen loop**

Allocate `R16G16B16A16_SFLOAT` raw diffuse and clear it every dispatch. In
raygen, reconstruct the primary surface, add direct sun visibility, select a
cosine-weighted direction, trace one radiance query, and evaluate:

```glsl
vec3 bounce = hit.valid
    ? hitEmission + evaluateSunAndSky(hit) * resolveBaseColor(hit.material, hit.tint)
    : sampleEnvironment(rayDirection);
rawDiffuse = primaryDiffuseThroughput * bounce * primary.bakedAO;
```

Use a PCG hash seeded by pixel, successfully presented temporal token, sample,
and bounce. Miss evaluation includes sky gradient/environment texture and a
finite sun disk. Do not call `traceRayEXT` from closest-hit; all trace calls
remain in raygen so pipeline recursion depth one is sufficient.

- [ ] **Step 4: Composite a demoable result and verify GREEN**

Temporarily combine raw diffuse with existing direct lighting in
`composite.frag`, behind `VulkanGiSettings::enabled`. Run opaque GI, baked-AO,
RT-disabled, resize, Native, and DLSS Quality smokes. Expected: deterministic
readbacks, finite HDR, and zero validation errors.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_gi_math.* MatterEngine3/src/render/vk_scene_renderer.* \
  MatterEngine3/shaders_vk/rt_lighting.rgen MatterEngine3/shaders_vk/rt_radiance.rmiss \
  MatterEngine3/shaders_vk/composite.frag MatterEngine3/include/matter/world_session.h \
  MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): trace one-bounce diffuse GI"
```

---

### Task 6: Temporal Reprojection, Rejection, and Moments

**Files:**
- Create: `MatterEngine3/shaders_vk/gi_temporal.comp`
- Modify: `MatterEngine3/src/render/vk_gi_contract.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/vk_temporal.h`
- Modify: `MatterEngine3/src/render/vk_temporal.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: raw signal, velocity, depth, normal, material index, instance token, presented attempt token.
- Produces: ping-pong accumulated radiance, first/second luminance moments, history length, and rejection-reason image.

- [ ] **Step 1: Write failing temporal sequence tests**

Use fixed raw colors over three frames. A static pixel must reach history length
three; a camera-translated pixel must sample the expected previous coordinate;
depth, normal, material, and instance changes must each yield history length
one with their unique rejection bit. Resize, camera cut, world reload, failed
present, and Native/DLSS mode change must reset exactly once.

- [ ] **Step 2: Verify RED**

Run the temporal/RT smoke. Expected: no GI history resources or rejection bits.

- [ ] **Step 3: Implement commit-safe history**

Allocate two retained history sets containing radiance, moments, history
length, previous depth/normal, and previous material/instance identity. Record
`gi_temporal.comp` after RT. Reproject with pixel-space motion vectors and reject
on bounds, plane-distance/depth threshold, `dot(n0,n1) < 0.85`, material
mismatch, instance mismatch, or reset. Apply neighborhood luminance/chroma
clipping before blending. Cap history at 32 frames and compute alpha as
`max(1/historyLength, 0.05)` for diffuse.

Publish the new history index only after successful present, using the same
candidate/commit rule as `VkTemporalState`; a failed attempt cannot become
future history.

- [ ] **Step 4: Verify GREEN and commit**

Run temporal, failed-present, resize, RT, Native, and DLSS mode-transition
smokes. Expected: exact history/rejection assertions and zero validation errors.

```bash
git add MatterEngine3/shaders_vk/gi_temporal.comp MatterEngine3/src/render/vk_gi_contract.h \
  MatterEngine3/src/render/vk_scene_renderer.* MatterEngine3/src/render/vk_temporal.* \
  MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): accumulate GI with validated reprojection"
```

---

### Task 7: Variance-Guided A-trous Diffuse Denoising

**Files:**
- Create: `MatterEngine3/shaders_vk/gi_atrous.comp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: temporal diffuse radiance/moments/history length and current depth/normal/material.
- Produces: filtered diffuse signal and reusable signal-filter dispatch parameters.

- [ ] **Step 1: Write failing edge-preservation fixture**

Feed a synthetic noisy 9x9 signal with a depth/normal/material boundary through
the compute pass. Require variance reduction within each region, less than 2%
energy crossing the boundary, finite output, and identical constant-color
input/output.

- [ ] **Step 2: Verify RED**

Compile the smoke executable. Expected: missing filter shader/pipeline/readback.

- [ ] **Step 3: Implement five-iteration A-trous filtering**

Use ping-pong `R16G16B16A16_SFLOAT` images and step widths 1, 2, 4, 8, 16.
Weight each tap by luminance variance plus depth-plane, normal, and material
agreement. Dispatch parameters explicitly select diffuse mode and its kernel
width; background and invalid histories copy through. Insert compute write/read
barriers between iterations and a compute-to-composite barrier afterward.

- [ ] **Step 4: Verify GREEN and commit**

Run the synthetic fixture plus diffuse GI reference scene in Native and DLSS
Quality. Expected: edge test passes, no NaN/Inf readback, validation zero.

```bash
git add MatterEngine3/shaders_vk/gi_atrous.comp MatterEngine3/src/render/vk_scene_renderer.* \
  MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): denoise diffuse GI"
```

---

### Task 8: GGX Metal and Dielectric Reflection Signal

**Files:**
- Modify: `MatterEngine3/src/render/vk_gi_math.h`
- Modify: `MatterEngine3/src/render/vk_gi_math.cpp`
- Modify: `MatterEngine3/shaders_vk/material_common.glsl`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/shaders_vk/gi_temporal.comp`
- Modify: `MatterEngine3/shaders_vk/gi_atrous.comp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Produces: raw/temporal/filtered specular radiance, hit distance, roughness-aware history/filter mode, base GGX lobe selection, and optional clearcoat GGX selection.

- [ ] **Step 1: Write failing BRDF and scene tests**

CPU tests require Schlick Fresnel to equal `F0` at normal incidence and approach
one at grazing incidence; GGX PDF must remain finite for roughness in
`[0.02,1]`. GPU scenes require a mirror to reflect a colored target, a rough
metal to broaden it, a tinted dielectric specular response to use authored
specular tint, and a rough dielectric to retain a nonmetal F0 near 0.04. A
material with `clearcoat=1` must add a second untinted dielectric highlight at
`clearcoatRoughness`, while `clearcoat=0` must launch no clearcoat samples.

- [ ] **Step 2: Verify RED**

Run RT smoke. Expected: missing Fresnel/GGX reference functions and specular
signal.

- [ ] **Step 3: Implement probability-correct GGX continuation**

Compute `F0 = mix(vec3(0.04) * specularStrength * specularTint, baseColor,
metallic)`, sample the GGX visible-normal distribution, reflect the incident
direction, and divide throughput by the chosen lobe probability/PDF. When
clearcoat is nonzero, include a top-layer dielectric GGX lobe with F0 0.04,
authored coat roughness, and energy compensation of the base layer; normalize
its selection weight with every other enabled lobe. Store radiance, first hit
distance, and roughness separately from diffuse. `max_reflection_roughness` is a
quality cutoff only; default it to 1.0 for correctness scenes.

- [ ] **Step 4: Add specular-specific denoising and verify GREEN**

Use shorter maximum history for low roughness, hit-distance rejection, and
narrower A-trous kernels for sharp reflection. Run mirror/rough metal,
disocclusion, and DLSS Quality scenes. Expected: finite energy, stable reflected
edges, no diffuse/specular history contamination, validation zero.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_gi_math.* MatterEngine3/shaders_vk/material_common.glsl \
  MatterEngine3/shaders_vk/rt_lighting.rgen MatterEngine3/shaders_vk/gi_temporal.comp \
  MatterEngine3/shaders_vk/gi_atrous.comp MatterEngine3/src/render/vk_scene_renderer.* \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): trace and denoise GGX reflections"
```

---

### Task 9: Glass and Water Transmission, Refraction, and Absorption

**Files:**
- Modify: `MatterEngine3/src/render/vk_gi_math.h`
- Modify: `MatterEngine3/src/render/vk_gi_math.cpp`
- Modify: `MatterEngine3/shaders_vk/material_common.glsl`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/shaders_vk/gi_temporal.comp`
- Modify: `MatterEngine3/shaders_vk/gi_atrous.comp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Produces: dielectric reflection/transmission lobe selection, bounded medium state, raw/filtered transmission signal, and absorption by traveled distance.

- [ ] **Step 1: Write failing optical math tests**

Add exact checks:

```cpp
CHECK(close(fresnel_dielectric(1.0f, 1.0f, 1.5f), 0.04f, 1e-4f),
      "air/glass normal-incidence Fresnel");
CHECK(!refract_direction(inside_grazing, normal, 1.5f, 1.0f, out),
      "glass-to-air grazing ray reaches total internal reflection");
CHECK(close3(beer_lambert({1, 0.5f, 0.25f}, 2.0f, 2.0f),
             {1, 0.5f, 0.25f}, 1e-4f),
      "absorption color is reached at authored distance");
```

GPU fixtures: clear glass bends a background target, colored glass attenuates
with thickness, water IOR produces stronger grazing reflection, and TIR remains
finite.

- [ ] **Step 2: Verify RED**

Run optical/RT smoke. Expected: missing refraction/Beer functions and
transmission signal.

- [ ] **Step 3: Implement closed-dielectric continuation**

For `transmission > 0 && VOLUME_BOUNDARY`, classify entering/exiting using the
geometric normal. Track a bounded current medium `{material_index, ior,
entry_t}` in raygen. Select Fresnel reflection or Snell transmission, redirect
TIR into reflection, offset origins onto the chosen side, and apply:

```glsl
vec3 sigma = -log(max(absorptionColor, vec3(1e-4))) /
             max(absorptionDistance, 1e-4);
throughput *= exp(-sigma * distanceInside);
```

Use authored thickness only when no reliable exit distance exists. Reject a
third nested volume with a counter/diagnostic and treat it as the current
medium, rather than overflowing payload state.

- [ ] **Step 4: Add conservative transmission history and verify GREEN**

Transmission stores hit distance and uses material/instance/depth rejection,
history cap 8, neighborhood clipping, and a narrow filter. Run glass, water,
TIR, camera translation, and DLSS Quality fixtures. Expected: correct direction
and attenuation, no NaN/Inf, validation zero.

- [ ] **Step 5: Commit**

```bash
git add MatterEngine3/src/render/vk_gi_math.* MatterEngine3/shaders_vk/material_common.glsl \
  MatterEngine3/shaders_vk/rt_lighting.rgen MatterEngine3/shaders_vk/gi_temporal.comp \
  MatterEngine3/shaders_vk/gi_atrous.comp MatterEngine3/src/render/vk_scene_renderer.cpp \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): trace glass and water transmission"
```

---

### Task 10: Thin-Walled Foliage Scattering, Emission, AO Fallback, and Fog

**Files:**
- Modify: `MatterEngine3/shaders_vk/material_common.glsl`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Create: `MatterEngine3/shaders_vk/gi_combine.comp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Produces: thin-scattering signal, secondary emission, optional ray-AO path, environment/sun disk/fog composition, and final denoised pre-DLSS HDR.

- [ ] **Step 1: Write failing foliage/emission/AO/atmosphere scenes**

Require front-lit and backlit leaf fixtures to differ predictably; backlight
must use scattering color and remain double-sided. An emissive panel must
contribute when hit by a secondary ray. Baked AO zero must suppress indirect
diffuse only. AO-invalid dynamic geometry with fallback enabled must darken a
near-corner fixture. Fog at zero density is identity and increasing camera
distance approaches the authored fog color monotonically.

- [ ] **Step 2: Verify RED**

Run the reference fixtures. Expected: missing scattering/AO/fog paths.

- [ ] **Step 3: Implement thin scattering and secondary emission**

For `THIN_WALLED && subsurface > 0`, do not enter a medium. Evaluate front
diffuse plus backlight:

```glsl
float back = pow(clamp(dot(-toSun, geometricNormal), 0.0, 1.0),
                 mix(8.0, 1.0, 0.5 * (anisotropy + 1.0)));
vec3 scatter = scatteringColor * subsurface * back * sunVisibility;
```

Orient double-sided normals against the incident ray while retaining the
geometric side for backlight. Add `emission.rgb * emissionStrength` at every
valid hit before continuation.

- [ ] **Step 4: Implement conditional AO and deterministic combine**

When AO-valid is false and fallback AO is enabled, trace one short cosine ray
with configured max distance; otherwise consume baked AO. `gi_combine.comp`
combines direct, filtered diffuse/specular/transmission/scattering, and emission.
Apply distance fog once using `1-exp(-density*distance*distance)`, then write
pre-tonemap HDR. Do not fog UI.

- [ ] **Step 5: Verify GREEN and commit**

Run all Task 10 fixtures plus forest scene in Native/DLSS Quality. Expected:
backlit leaves, secondary emission, correct AO exclusions, monotonic fog, finite
HDR, validation zero.

```bash
git add MatterEngine3/shaders_vk/material_common.glsl MatterEngine3/shaders_vk/rt_lighting.rgen \
  MatterEngine3/shaders_vk/gi_combine.comp MatterEngine3/src/render/vk_scene_renderer.* \
  MatterEngine3/include/matter/world_session.h MatterEngine3/Makefile MatterViewer/Makefile \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): add foliage scattering and HDR GI composition"
```

---

### Task 11: Second Bounce, Russian Roulette, Quality Controls, and GPU Counters

**Files:**
- Modify: `MatterEngine3/src/render/vk_gi_contract.h`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterViewer/main.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Produces: configurable two-bounce integrator, probability-correct lobe counters, GPU timestamps, and truthful frame statistics/UI.

- [ ] **Step 1: Write failing settings and counter tests**

Assert max bounce clamps to `[0,2]`, samples to `[1,4]`, trace scale to
`[0.5,1]`, and denoiser iterations to `[0,5]`. A one-pixel deterministic scene
must increment exactly one continuation-lobe counter per bounce; sum of lobe
counts cannot exceed launched paths times bounce count. Disabled features must
record zero rays.

- [ ] **Step 2: Verify RED**

Run RT smoke. Expected: missing settings normalization, counters, and timestamp
fields.

- [ ] **Step 3: Add bounded second bounce and roulette**

Change raygen to an explicit `for (bounce=0; bounce<=max_bounces; ++bounce)`.
After bounce one, continue with probability
`p=clamp(max(throughput.r,max(throughput.g,throughput.b)),0.05,0.95)` and divide
throughput by `p` when retained. Select exactly one normalized material lobe
per continuation and divide by its selection probability. Stop on miss,
negligible throughput, invalid surface, or configured bounce limit.

Dispatch at `ceil(internalExtent * trace_scale)` and map trace pixels to the
full internal G-buffer with pixel-center coordinates; `trace_scale=0.5` is the
reduced-resolution option. For each trace pixel, loop from one to
`samples_per_pixel`. In adaptive mode, use one sample after history length 8
when temporal luminance variance is below the configured threshold and use up
to the requested sample count otherwise. Accumulate and divide by the actual
sample count before writing raw signals.

- [ ] **Step 4: Add nonblocking instrumentation and UI**

Use timestamp/query pools already associated with frame slots. Read results
only after that slot's fence is acquired. Report TLAS, RT, temporal, filter,
combine, and DLSS GPU milliseconds plus shadow/diffuse/specular/transmission/
scattering/AO rays and any-hit layers. Define a stable `GiDebugView` enum for
final, material/flags, raw/temporal/filtered diffuse, specular, transmission,
scattering, path lobe, bounce count, hit distance, AO source, history length,
history weight, rejection reason, reactive mask, and pre-DLSS HDR. The combine
shader selects the requested view without changing history contents. Viewer
controls expose all settings and debug views but print selected versus active
values separately.

- [ ] **Step 5: Verify GREEN and commit**

Run settings/counter tests, then demo and forest perf captures with bounce 1 and
2. Expected: counters balance, no query wait in the frame path, validation zero.

```bash
git add MatterEngine3/src/render/vk_gi_contract.h MatterEngine3/shaders_vk/rt_lighting.rgen \
  MatterEngine3/src/render/vk_scene_renderer.* MatterEngine3/include/matter/world_session.h \
  MatterViewer/main.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): add bounded GI quality controls and counters"
```

---

### Task 12: DLSS Masks, Reference Scenes, Windows Build, and Acceptance Gate

**Files:**
- Modify: `MatterEngine3/src/render/streamline_bridge.h`
- Modify: `MatterEngine3/src/render/streamline_bridge.cpp`
- Modify: `MatterEngine3/shaders_vk/gi_combine.comp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterViewer/main.cpp`
- Modify: `MatterViewer/tools/check_vulkan_viewer.ps1`
- Modify: `MatterViewer/tools/smoke_vulkan_viewer.ps1`
- Modify: `MatterViewer/tools/perf_vulkan_instancing.ps1`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `.superpowers/sdd/progress.md`

**Interfaces:**
- Consumes: final pre-tonemap HDR and signal confidence/history.
- Produces: supported Streamline masks, selectable deterministic material scenes, full validation/performance evidence, and updated CUDA+Streamline Windows viewer.

- [ ] **Step 1: Write failing mask/fallback and scene-selection tests**

The fake Streamline bridge must receive reactive/transparency resources only
when its capability query reports those tags. Unsupported optional tags must
still evaluate DLSS with color/depth/motion/exposure. Native mode must never tag
or evaluate. Scene selection must expose `gi-opaque`, `gi-reflection`,
`gi-glass`, `gi-water`, `gi-emissive`, `gi-foliage`, `gi-ao`, and
`gi-disocclusion` with fixed seeds.

- [ ] **Step 2: Verify RED**

Build/run the fake bridge and viewer static gate. Expected: missing mask
resources/capability path and missing reference-scene selectors.

- [ ] **Step 3: Generate supported masks and reference scenes**

`gi_combine.comp` writes reactive values from rapid luminance/history changes,
transmission, emission, and thin scattering; it writes transparency/composition
values from transmissive/alpha surfaces. Add them to Streamline tagging only
when the installed SDK/runtime reports support. Keep native and DLSS fallback
paths independent of mask availability. Add deterministic viewer scene setup
and debug-view command-line/environment selection.

- [ ] **Step 4: Run the complete automated gates**

From MSYS2/PowerShell run:

```bash
make -C MatterSurfaceLib/tests run-reg
make -C MatterEngine3 test
make -C MatterViewer vulkan-smoke HAVE_CUDA=1 HAVE_STREAMLINE=1
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/check_vulkan_viewer.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File MatterViewer/tools/smoke_vulkan_viewer.ps1
```

Expected: all CPU/GPU fixtures pass, every supported mode reports zero Vulkan
validation errors, Native-to-DLSS-to-Native resets exactly once per transition,
and unsupported RTX/DLSS paths remain functional.

- [ ] **Step 5: Build the final Windows CUDA+Streamline viewer**

Run from the MSYS2 UCRT64 shell with the locally supplied Streamline SDK path:

```bash
make -C MatterViewer windows HAVE_CUDA=1 HAVE_STREAMLINE=1 \
  STREAMLINE_PATH="$STREAMLINE_PATH" STREAMLINE_DLL_DIR="$STREAMLINE_DLL_DIR"
```

Expected: `MatterViewer/viewer.exe`, `sl.interposer.dll`, `sl.dlss.dll`, and
`nvngx_dlss.dll` exist; the interposer/DLSS DLL signature checks pass; the PE
gate reports no forbidden OpenGL/CUDA-GL imports.

- [ ] **Step 6: Run visual/performance acceptance and record evidence**

Launch each reference scene at 2560x1440 output with DLSS Quality. Require no
reversed motion, invented disocclusion edges, persistent trails, NaN/Inf pixels,
or validation messages. Run the current demo/representative world long enough
to collect stable GPU timestamps. Acceptance is 60 FPS on the reference RTX
system; if missed, retain correctness, record the per-stage timing/counters, and
tune only the exposed trace scale, roughness cutoff, bounce count, or filter
iterations until the target is met.

- [ ] **Step 7: Review and commit**

Run `git diff --check` and review the complete branch against
`docs/superpowers/specs/2026-07-14-vulkan-hybrid-gi-materials-design.md`. Record
adapter, output/internal extents, selected/active DLSS mode, all GI settings,
GPU timings, ray counters, validation count, and fallback status in progress.

```bash
git add MatterEngine3/src/render/streamline_bridge.* MatterEngine3/shaders_vk/gi_combine.comp \
  MatterEngine3/src/render/vk_scene_renderer.* MatterViewer/main.cpp \
  MatterViewer/tools/check_vulkan_viewer.ps1 MatterViewer/tools/smoke_vulkan_viewer.ps1 \
  MatterViewer/tools/perf_vulkan_instancing.ps1 MatterEngine3/tests/vulkan_smoke_tests.cpp \
  .superpowers/sdd/progress.md
git commit -m "test(vulkan): verify hybrid GI with DLSS SR"
```

---

## Completion Definition

The work is complete only when all twelve tasks are committed, every reference
scene is selectable in the updated Windows viewer, the final CUDA+Streamline
build is present beside its signed runtime DLLs, automated gates report zero
Vulkan validation errors, and the 1440p DLSS Quality reference run either meets
60 FPS or includes measured stage evidence plus an approved quality adjustment
that reaches it.
