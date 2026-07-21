# Ray-Traced Emission Sampling for Volumetric Fog

> Design and implementation spec for making emissive materials illuminate the froxel fog volume — scaling to any number of emissive surfaces anywhere in the world, with no CPU-side light lists.

- **Target:** `vol_scatter.comp` + `VkVolumetrics`
- **Baseline:** `7cb2708e` (local main, includes VkVolumetrics host module)
- **Status:** Spec — ready to implement

---

## Part I — Design Spec

Why fog should pick up light from emissive geometry, which architecture gets us there, and what it costs.

### I.1 Problem statement

The froxel volumetric system (160 × 90 × 128 grid, three compute passes: `vol_density` → `vol_scatter` → `vol_integrate`) currently lights fog with exactly two sources: the sun (one `rayQueryEXT` shadow ray per active froxel) and a flat sky-ambient term. Emissive materials — lava, lamps, screens, the registry's "cool light" and "warm light" materials — already glow in the surface passes (`composite.frag:163-171`, `rt_lighting.rgen:153-155`) but contribute nothing to the fog around them. A lantern in mist produces no halo; a lava pool under smoke casts no orange glow into it.

The classical fix — a CPU-gathered point-light list looped in the scatter shader — was rejected up front: emissive surfaces in MatterEngine3 are arbitrary triangles authored per-vertex (material index lives in `VkRasterVertex` word 14, not per-instance), can be large-area (a lava lake is not a point), and can exist in unbounded numbers anywhere in a streamed world. We want a solution whose cost is fixed per froxel regardless of emitter count.

### I.2 Goals and non-goals

**Goals:**

- Emissive materials (`RtMaterialGpu.emission_strength`: `.rgb` color, `.w` strength) illuminate fog around them, in correct color, with distance falloff.
- Scales to *any* number of emissive surfaces: cost is O(rays per froxel), independent of emitter count and world size.
- Area emitters work naturally — a large lava pool lights fog more than a small ember, with no special-casing.
- Deterministic: same camera, same frame index → same result (seeded noise, no `gl_SubgroupInvocationID`-style nondeterminism).
- Fits the existing temporal budget: 1–2 extra rays only for the 1-in-4 Bayer-active froxel columns; history blending amortizes convergence.
- Zero new GPU memory allocations — reuse the per-frame part table and material SSBO the RT pipeline already builds.

**Non-goals:**

- **Not** a converged path-traced volume. 1–2 uniform rays per active froxel is a biased, heavily temporally-filtered estimate. Fireflies are traded away for stability (see falloff discussion, §I.4).
- **No** next-event estimation / explicit emissive-triangle light list this iteration (recorded as the natural follow-up, §I.3-C).
- **No** shadowing of emission *within* the fog (no secondary transmittance march along the emission ray).
- **No** analytic point/spot `WorldLight` fog contribution — this spec covers mesh emission only.
- **No** change to the density or integrate passes, the composite shader, or the fog emitter (`GpuVolumeEmitter`) path.

### I.3 Approach comparison

#### A — Ray query + BDA vertex lookup **(chosen)**

Trace 1–2 closest-hit ray queries per active froxel in uniform random directions against the existing TLAS. On hit, resolve the material exactly the way `rt_surface.rchit` does (`load_rt_surface()` in `rt_surface_common.glsl:124-169`): `instanceCustomIndex` → `GpuRtPartRecord` → index buffer via BDA → leading vertex's `material_index` (word 14) → `rt_materials[]` → `emission_strength`.

- **Pro:** exact per-triangle materials. Parts that mix emissive and non-emissive vertices in one BLAS resolve correctly, because the lookup happens at triangle granularity — same fidelity as the RT surface pipeline.
- **Pro:** zero CPU work and zero new memory. `frame.rt_parts` and `frame.materials` are already built and flushed every RT frame (`emit_ray_instances()`, vk_scene_renderer.cpp:5116-5143).
- **Pro:** emission-only lookup is cheap: unlike the full `load_rt_surface()` (3 vertices, normals, barycentrics), we need one index load and one 4-byte vertex word — 4 dependent loads per hit total.
- **Con:** incoherent random rays cost more per ray than the coherent sun rays; dependent BDA loads add latency on hit.

#### B — CPU-built per-instance emission table

Populate a `vec4 emission[part_slot]` SSBO on the CPU alongside the part records; the shader does one load per hit.

- **Pro:** single load per hit; trivially simple shader.
- **Con — disqualifying:** material index is *per-vertex*, not per-instance. A part_slot can span many materials, so "the" emission of an instance is undefined. Computing a representative value would require the CPU to scan vertex buffers it doesn't otherwise touch (they live in GPU-side part geometry), reintroducing exactly the per-emitter CPU cost this feature is meant to avoid — and it would still be wrong for mixed parts (a lamp mesh whose bulb is 2% of its triangles would smear emission across the whole housing, or lose it entirely).
- **Con:** new buffer + upload + lifetime management for strictly less accuracy.

#### C — Emissive-triangle light list with explicit sampling (NEE)

Build a GPU list of emissive triangles (compute-pass scan of part geometry), sample it directly per froxel with a shadow ray toward the sampled point.

- **Pro:** dramatically better convergence for small, bright emitters — this is the "correct" long-term answer.
- **Con:** requires a new scan pass, a CDF/alias table, power-weighted sampling, and invalidation when parts stream in/out — a project several times this one's size. Deferred; approach A's shader structure (seeded RNG, material fetch helper, push-constant ray count) is forward-compatible with adding NEE rays later.

#### D — Screen-space / probe approximations

Sampling emission from the G-buffer or from GI probes was rejected: screen-space misses off-screen emitters entirely (a lava pool behind the camera should still tint fog in view), and the engine has no world-space probe volume to borrow from. Both violate "anywhere in the world."

> **Decision:** Approach **A**. The per-vertex material layout makes B semantically lossy, and A reuses proven infrastructure — the same TLAS the shadow ray already binds (binding 4) and the same two SSBOs the RT pipeline binds every frame. C is the designated successor if quality demands it.

### I.4 Chosen design

Inside the existing `is_active && extinction > 1e-6` block of `vol_scatter.comp` (after the sun and sky terms, lines 59-99), each active froxel:

1. Seeds a PCG stream from `(voxel.x, voxel.y, z, frame_index)` — the same `pcg_hash`/`random_float` pair as `rt_lighting.rgen:51-60`. Deterministic per voxel per frame; decorrelated across slices and frames.
2. Traces `emission_rays` (default 1, max 4) uniform-sphere ray queries with `gl_RayFlagsOpaqueEXT`, tMin 0.1 (matching the shadow ray), tMax `emission_range` (default 100, clamped to `VOL_FAR`).
3. On a committed triangle hit, fetches emission via part table → index BDA → vertex word 14 → material table, and accumulates `emission · falloff(t) · HG(dot(view_dir, ray_dir), g)`.
4. Multiplies the sum by `scattering` (albedo·extinction, same as sun/sky terms), by `emission_intensity`, and by `1/N`, then adds it into `scatter_result.rgb`. Temporal reprojection and integration are untouched — they operate on the summed in-scatter exactly as today.

#### Falloff model

Per the feature requirements, hits attenuate with inverse-square distance. We use the bounded form `falloff = 1 / (1 + t^2)`: identical to 1/t^2 in the far field, but capped at 1 as t → 0 so a froxel adjacent to a lava surface cannot produce an unbounded firefly that temporal blending then smears for half a second.

> **Physics note — deliberate bias:** A hit-sampled area emitter already exhibits geometric falloff statistically (distant emitters subtend less solid angle and are hit less often); radiance along a ray is distance-invariant, so the physically neutral estimator would apply *no* explicit falloff and multiply by 4π/N instead. The explicit 1/(1+t^2) term is an intentional artistic damping: it converts the estimator's variance (rare, full-strength hits) into a smooth, stable near-field glow that suits a 1-ray budget. `emission_intensity` exists precisely to rebalance overall energy after this bias, and folds the 4π constant. If NEE (§I.3-C) lands later, this term is removed in favor of the correct geometry term.

#### Data flow

```
CPU (per frame, already exists)                    GPU vol_scatter.comp (new)
───────────────────────────────                    ──────────────────────────
emit_ray_instances()                               ray query (binding 4 TLAS)
  ├─ TLAS  instanceCustomIndex = part_slot   ──►     │ committed triangle hit
  ├─ frame.rt_parts  GpuRtPartRecord[]       ──►   binding 5: part_slot → vertex/index BDA
  └─ frame.materials MaterialGpuRecord[]     ──►   binding 6: material_index → emission_strength
                                                     │
raster: material_index per vertex (word 14)  ──►   index[3·prim] → vertex word 14
```

### I.5 Visual and behavioral expectations

- **Halos:** fog near an emissive surface picks up its emission color; brightness falls off with distance and scales with local fog density. A lantern in mist reads as a soft sphere of light; a lava lake tints the smoke column above it.
- **Directionality:** the HG phase term (shared `phase_g`) makes glow slightly stronger when looking toward the emitter through fog, matching the sun's behavior.
- **Convergence:** with 2x2 Bayer subsampling each column refreshes every 4th frame and history blends at 0.85, so emission converges with a time constant of roughly 25-30 frames (~0.5 s at 60 fps). Static scenes settle to a stable, low-noise glow; a fast-moving emissive object leaves a brief (<0.5 s) trailing glow — same lag class as the existing sun-shadow term.
- **Noise character:** low ray counts on small emitters show gentle temporal shimmer in the glow, not black/white speckle — bounded falloff caps single-sample energy.
- **Controls:** Volumetrics UI panel gains *Emission boost* (intensity), *Emission rays* (0-4; 0 disables and restores today's image bit-exactly), and *Emission range*. Defaults on with 1 ray.
- **Fallbacks:** no ray query → volumetrics already disabled (unchanged). RT buffers unavailable in a frame → emission rays forced to 0 for that frame; fog otherwise normal.

### I.6 Performance budget

| Quantity | Value | Notes |
|---|---|---|
| Froxel columns | 14,400 | 160 x 90 |
| Bayer-active columns / frame | 3,600 | 1 of 4 (frame_index & 3) |
| Active froxels / frame (worst case) | 460,800 | 3,600 x 128 slices, all with extinction > 1e-6 |
| Existing shadow rays / frame | <= 460,800 | terminate-on-first-hit, coherent direction |
| New emission rays / frame @ 1 rpv | +460,800 | closest-hit, incoherent — expect ~1.5-2x per-ray cost of the shadow ray |
| New emission rays / frame @ 2 rpv | +921,600 | ~45% of a 1080p full-screen ray pass (2.07 M) |
| Hit shading cost | 4 loads | part record → index[3*prim] → vertex word 14 → material record |
| New GPU memory | 0 B | rebinds existing `frame.rt_parts` + `frame.materials` |
| Push constants | 208 → 224 B | within the 256 B desktop norm; device already required to accept 208 |
| Descriptor pool | +4 | 2 STORAGE_BUFFER descriptors x 2 ping-pong sets |

**Budget:** <= 0.6 ms added to `gpu_vol_ms` at the default (1 ray) on RTX-3070-class hardware, <= 1.2 ms at 2 rays. The `gpu_vol_ms` HUD timer (kGpuZoneVolumetrics) already brackets exactly this pass, so the cost is directly observable. If over budget: first lower `emission_range` (traversal-bounded), then drop to fog-only slices (extinction gate already does this), then reduce rays to alternate frames via `frame_index` parity.

### I.7 Risks and mitigations

| Risk | Mitigation |
|---|---|
| Fireflies from small bright emitters at 1-2 rays | Bounded 1/(1+t^2) falloff caps single-sample energy; 0.85 temporal blend integrates ~27 samples; `emission_intensity` is a user clamp of last resort. |
| Push-constant growth (208 → 224 B) exceeds a device limit | Vulkan guarantees only 128 B, but the pass already requires 208 and the engine gates volumetrics on RT-capable desktop hardware where 256 B is universal. Assert-fail cleanly at pipeline-layout creation (existing `vk_fail` path reports it). |
| Stale TLAS handle vs. fresh part table in a growth frame | `RasterRecord.tlas` is captured before `emit_ray_instances()` may recreate the TLAS (pre-existing latent issue). Part/material buffers are passed as `VkBufferResource*` so their handles are read at dispatch-record time; worst case is one frame of mismatched emission lookups when the TLAS is regrown, visually negligible under temporal blend. Optional hardening noted in §II.6. |
| Materials copy-to-device barrier doesn't cover compute | The upload barrier (`record_material_upload_commands`, vk_scene_renderer.cpp:234-236) currently targets VERTEX\|FRAGMENT only. §II.6 widens it to include COMPUTE_SHADER — required for correctness, not just for this feature's comfort. |
| Unwritten bindings 5/6 when RT buffers are absent | The shader statically references both SSBOs, so descriptors must always be valid: fall back to binding the always-alive emitter SSBO and force `emission_rays = 0` for the frame (§II.5). |
| Non-opaque (mask 0x02) instances intercept rays oddly | Emission rays use cull mask 0xFF + `gl_RayFlagsOpaqueEXT`, same as the shadow ray: glass both occludes and can emit. Consistent with the existing fog-shadow behavior; revisit only if glass emitters become common. |
| Regression risk to existing fog look | `emission_rays = 0` short-circuits every new code path; the acceptance checklist (§II.8) includes an off/on A-B. |

---

## Part II — Implementation Spec

Ordered steps with file paths, line anchors (at baseline 7cb2708e), and exact code. Steps II.1-II.2 are shader-only; II.3-II.6 are C++; II.7 rebuilds. No further design decisions required.

### II.1 Extract a binding-free geometry-types include

`vol_scatter.comp` needs the `GpuRtPartRecord`/`RtMaterialGpu` structs and BDA loaders, but cannot include `rt_surface_common.glsl`: that file hard-codes `set 0, bindings 3/4/5` (RtPartTable / RtMaterialTable / RtErrorCounter), which collide with the scatter set's depth (3) and TLAS (4). Split the binding-free portion into a new include rather than duplicating ~60 lines that must track the C++ contracts.

**NEW** `MatterEngine3/shaders_vk/rt_geometry_types.glsl`:

```glsl
// rt_geometry_types.glsl -- binding-free RT geometry/material types and BDA
// loaders, shared by RT pipeline shaders (via rt_surface_common.glsl) and
// ray-query compute shaders (vol_scatter.comp). Structs must match
// GpuRtPartRecord in vk_gi_contract.h (48 B) and MaterialGpuRecord in
// material_registry.h (144 B). No layout(set=...) declarations here.
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_buffer_reference_uvec2 : require

struct GpuRtPartRecord {
    uvec2 vertex_address;
    uvec2 index_address;
    uint vertex_stride;
    uint vertex_count;
    uint primitive_count;
    uint valid;
    uint pad0; uint pad1; uint pad2; uint pad3;
};

struct RtRasterVertex {
    vec3 position;
    vec3 normal;
    vec4 tint;
    vec4 surface;
    uint material_index;
    uint pad0;
    uint pad1;
    uint pad2;
};

layout(buffer_reference, buffer_reference_align = 4) readonly buffer
RtVertexBuffer {
    uint words[];
};

layout(buffer_reference, buffer_reference_align = 4) readonly buffer
RtIndexBuffer {
    uint indices[];
};

struct RtMaterialGpu {
    vec4 base_roughness;
    vec4 metal_opacity_spec_coat;
    vec4 specular_tint_coat_roughness;
    vec4 emission_strength;
    vec4 transmission;
    vec4 absorption_pad;
    vec4 scattering;
    vec4 scattering_shape;
    uvec4 flags_misc;
};

vec3 rt_load_vec3(RtVertexBuffer geometry, uint word) { /* moved verbatim */ }
vec4 rt_load_vec4(RtVertexBuffer geometry, uint word) { /* moved verbatim */ }
RtRasterVertex rt_load_vertex(RtVertexBuffer geometry, uint vertex,
                              uint stride) { /* moved verbatim */ }
```

Move the marked declarations *verbatim* out of `rt_surface_common.glsl` (current lines 1-2 extensions, 23-51 structs/buffer refs, 58-68 `RtMaterialGpu`, 84-108 loaders). Then edit `rt_surface_common.glsl` to open with:

```glsl
#include "rt_geometry_types.glsl"
```

Everything else in `rt_surface_common.glsl` stays: `RtSurface`, `RtSurfacePayload`, the three set-0 SSBO declarations (bindings 3/4/5), `invalid_rt_surface()`, and `load_rt_surface()`. The five RT shaders that include it (`rt_surface.rchit`, `rt_visibility.rahit`, `rt_radiance.rmiss`, `rt_lighting.rgen`, `rt_surface_test.rgen`) compile unchanged — the include chain provides identical text.

### II.2 Shader changes — vol_scatter.comp

`MatterEngine3/shaders_vk/vol_scatter.comp`

#### 2a — Includes and new bindings (after line 13)

```glsl
#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_ray_query : require

#include "vol_common.glsl"
#include "rt_geometry_types.glsl"   // NEW

layout(local_size_x = 4, local_size_y = 4, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler3D vol_media_tex;
layout(rgba16f, set = 0, binding = 1) uniform writeonly image3D vol_scatter_out;
layout(set = 0, binding = 2) uniform sampler3D vol_scatter_history;
layout(set = 0, binding = 3) uniform sampler2D depth_tex;
layout(set = 0, binding = 4) uniform accelerationStructureEXT tlas;
layout(std430, set = 0, binding = 5) readonly buffer VolRtParts {   // NEW
    GpuRtPartRecord vol_rt_parts[];
};
layout(std430, set = 0, binding = 6) readonly buffer VolRtMaterials { // NEW
    RtMaterialGpu vol_rt_materials[];
};
```

#### 2b — Push constants (replace `float pad2;` at line 29)

```glsl
    uint history_valid;
    float camera_near;
    float camera_far;
    float emission_intensity;   // artistic multiplier, folds 4*pi
    uint  emission_rays;        // 0 disables; host clamps to 4
    float emission_range;       // max ray t, world units
    float pad3;
    float pad4;
} pc;
```

GLSL std430 push-constant size becomes 224 bytes; it must stay in lockstep with the C++ mirror in §II.4.

#### 2c — Helper functions (insert after `bayer2x2`, line 41)

`pcg_hash` and `random_float` are copied byte-for-byte from `rt_lighting.rgen:51-60` (the established pattern — that file owns its own copies too).

```glsl
// PCG hash + float stream (matches rt_lighting.rgen).
uint pcg_hash(uint value) {
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random_float(inout uint seed) {
    seed = pcg_hash(seed);
    return float(seed) * (1.0 / 4294967296.0);
}

vec3 uniform_sphere_direction(inout uint seed) {
    float cos_z = random_float(seed) * 2.0 - 1.0;
    float phi = 6.28318530718 * random_float(seed);
    float r = sqrt(max(0.0, 1.0 - cos_z * cos_z));
    return vec3(r * cos(phi), r * sin(phi), cos_z);
}

// Emission (color * strength) of the triangle hit by an emission ray.
// Mirrors load_rt_surface() in rt_surface_common.glsl, but loads only the
// leading vertex's material_index (word 14 of the 72-byte VkRasterVertex),
// exactly the field the rchit path uses for surface.material_index.
vec3 fetch_triangle_emission(uint part_slot, uint prim) {
    GpuRtPartRecord part = vol_rt_parts[part_slot];
    if (part.valid == 0u || part.vertex_stride != 72u ||
        prim >= part.primitive_count) {
        return vec3(0.0);
    }
    RtIndexBuffer index_buffer = RtIndexBuffer(part.index_address);
    uint i0 = index_buffer.indices[prim * 3u];
    if (i0 >= part.vertex_count) return vec3(0.0);
    RtVertexBuffer geometry = RtVertexBuffer(part.vertex_address);
    uint material_index =
        geometry.words[i0 * (part.vertex_stride / 4u) + 14u];
    RtMaterialGpu material = vol_rt_materials[material_index];
    return material.emission_strength.rgb *
           max(material.emission_strength.w, 0.0);
}
```

#### 2d — Emission ray loop (insert after `sky_contrib`, line 97; replaces the assignment at line 99)

```glsl
            // Sky ambient (isotropic approximation using upward hemisphere).
            vec3 sky_contrib = scattering * sky_irradiance(vec3(0.0, 1.0, 0.0));

            // Emission rays: uniform-sphere ray queries pick up radiance
            // from emissive triangles anywhere in the TLAS. Deterministic
            // seed: voxel coords + slice + frame index.
            vec3 emission_contrib = vec3(0.0);
            if (pc.emission_rays > 0u) {
                uint seed = pcg_hash(uint(voxel.x) * 1973u ^
                                     uint(voxel.y) * 9277u ^
                                     uint(z) * 26699u ^
                                     pc.frame_index * 2699u);
                for (uint r = 0u; r < pc.emission_rays; ++r) {
                    vec3 dir = uniform_sphere_direction(seed);
                    rayQueryEXT erq;
                    rayQueryInitializeEXT(erq, tlas,
                        gl_RayFlagsOpaqueEXT, 0xFF,
                        world_pos, 0.1, dir, pc.emission_range);
                    while (rayQueryProceedEXT(erq)) {}
                    if (rayQueryGetIntersectionTypeEXT(erq, true) ==
                        gl_RayQueryCommittedIntersectionTriangleEXT) {
                        uint part_slot = uint(
                            rayQueryGetIntersectionInstanceCustomIndexEXT(
                                erq, true));
                        uint prim = uint(
                            rayQueryGetIntersectionPrimitiveIndexEXT(
                                erq, true));
                        float hit_t =
                            rayQueryGetIntersectionTEXT(erq, true);
                        vec3 emission =
                            fetch_triangle_emission(part_slot, prim);
                        if (max(emission.r,
                                max(emission.g, emission.b)) > 0.0) {
                            // Bounded inverse-square falloff (see design
                            // spec I.4): ~1/t^2 far field, capped near 0.
                            float falloff = 1.0 / (1.0 + hit_t * hit_t);
                            float e_phase = hg_phase(
                                dot(view_dir, dir), pc.phase_g);
                            emission_contrib +=
                                emission * falloff * e_phase;
                        }
                    }
                }
                emission_contrib *= scattering * pc.emission_intensity *
                                    (1.0 / float(pc.emission_rays));
            }

            scatter_result = vec4(sun_contrib + sky_contrib +
                                  emission_contrib, extinction);
```

Notes: `dir` points from the froxel toward the hit, so it is the incoming-light direction and `dot(view_dir, dir)` matches the sun convention (`dot(view_dir, to_sun)`, line 89). tMin 0.1 matches the shadow ray (line 79). `gl_RayFlagsOpaqueEXT` forces the single-`Proceed` loop to commit the closest hit without candidate handling; cull mask 0xFF matches the shadow ray so opaque (0x01) and non-opaque (0x02) instances both participate.

### II.3 Settings plumbing

#### 3a — World definition

`MatterEngine3/include/matter/world_definition.h` — struct `VulkanVolumetricsSettings` (lines 42-52):

```cpp
struct VulkanVolumetricsSettings {
    bool  enabled        = false;
    float temporal_blend = 0.85f;
    float phase_g        = 0.3f;
    float fog_density_mul  = 1.0f;
    float fog_floor_offset = 0.0f;
    float fog_falloff_mul  = 1.0f;
    float fog_color_mul[3] = {1.0f, 1.0f, 1.0f};
    float fog_wind_mul[3]  = {1.0f, 1.0f, 1.0f};
    float vol_debug_view   = 0.0f;
    bool  emission_enabled   = true;        // NEW
    float emission_intensity = 1.0f;        // NEW
    float emission_range     = 100.0f;      // NEW
    int   emission_rays      = 1;           // NEW — clamped to [0, 4] by VkVolumetrics
};
```

#### 3b — Viewer UI

`MatterViewer/ui.cpp` — Volumetrics panel (insert after "Fog falloff", line 620):

```cpp
        ImGui::SliderFloat("Fog falloff", &s.volumetrics.fog_falloff_mul, 0.1f, 4.0f, "%.2f");
        ImGui::Checkbox("Emission##vol", &s.volumetrics.emission_enabled);
        if (s.volumetrics.emission_enabled) {
            ImGui::SliderInt("Emission rays", &s.volumetrics.emission_rays, 0, 4);
            ImGui::SliderFloat("Emission boost", &s.volumetrics.emission_intensity,
                               0.0f, 8.0f, "%.2f");
            ImGui::SliderFloat("Emission range", &s.volumetrics.emission_range,
                               5.0f, 300.0f, "%.0f");
        }
        const char* vol_views[] = { "Off", "Density", "Scatter", "Integrated" };
```

No engine-facade change is needed: `matter_engine.cpp:4009` already forwards the whole `opts.vulkan_volumetrics` struct into `VkSceneRenderer::set_volumetrics_settings()` (vk_scene_renderer.cpp:3795), which hands it to `VkVolumetrics::update_settings()` intact.

### II.4 VkVolumetrics header

`MatterEngine3/src/render/vk_volumetrics.h`

#### 4a — Constant (after `kVolNoiseSize`, line 48)

```cpp
static constexpr uint32_t kVolMaxEmissionRays = 4;
```

#### 4b — `record()` signature (lines 73-79) — add the two buffer parameters

```cpp
    // Record the three compute dispatches into |cmd|.  |rt_parts| and
    // |materials| are the scene renderer's per-frame RT part table and
    // material SSBO; the scatter pass samples them for emission rays.
    // Their handles are read here (not at job-construction time) because
    // both buffers can be replaced when they grow mid-record.
    bool record(VkCommandBuffer cmd,
                uint32_t frame_slot,
                matter::VkImageResource& depth_image,
                VkAccelerationStructureKHR tlas,
                const matter::VkBufferResource& rt_parts,     // NEW
                const matter::VkBufferResource& materials,    // NEW
                const FrameMatrices& matrices,
                float frame_time,
                std::string& error);
```

#### 4c — `ScatterConstants` (lines 108-124) — replace `float pad2;`, update assert

```cpp
        uint32_t history_valid;
        float camera_near;
        float camera_far;
        float emission_intensity;     // NEW
        uint32_t emission_rays;       // NEW
        float emission_range;         // NEW
        float pad3;                   // NEW
        float pad4;                   // NEW
    };
    static_assert(sizeof(ScatterConstants) == 224);
```

#### 4d — Latched settings (after `fog_wind_mul_`, line 194)

```cpp
    bool  emission_enabled_ = true;
    float emission_intensity_ = 1.0f;
    float emission_range_ = 100.0f;
    uint32_t emission_rays_ = 1;
```

### II.5 VkVolumetrics implementation

`MatterEngine3/src/render/vk_volumetrics.cpp`

#### 5a — Scatter set layout — two new bindings (bindings array, lines 479-490; count at line 494)

```cpp
    // Bindings:
    //   0 = combined image sampler (vol_media, read)
    //   1 = storage image (vol_scatter[current], write)
    //   2 = combined image sampler (vol_scatter[history], read)
    //   3 = combined image sampler (depth texture)
    //   4 = acceleration structure (TLAS)
    //   5 = storage buffer (GpuRtPartRecord[], per-frame)     // NEW
    //   6 = storage buffer (RtMaterialGpu[], per-frame)       // NEW
    const VkDescriptorSetLayoutBinding bindings[] = {
        ...existing five...
        make_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
        make_binding(6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     VK_SHADER_STAGE_COMPUTE_BIT),
    };
    ...
    set_info.bindingCount = 7;
```

#### 5b — Descriptor pool (lines 541-550)

```cpp
    const VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6},    // 3 per set x 2
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},             // 1 per set x 2
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2},// 1 per set x 2
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},            // 2 per set x 2 — NEW
    };
    ...
    pool_info.poolSizeCount = 4;
```

#### 5c — `update_settings()` (lines 736-757) — latch the new fields

```cpp
    fog_falloff_mul_ = vol.fog_falloff_mul;
    emission_enabled_ = vol.emission_enabled;
    emission_intensity_ = vol.emission_intensity;
    emission_range_ = vol.emission_range;
    emission_rays_ = static_cast<uint32_t>(std::clamp(
        vol.emission_rays, 0, static_cast<int>(kVolMaxEmissionRays)));
```

(`<algorithm>` is already included at line 3.)

#### 5d — `record()` — signature, per-frame descriptor writes (lines 806-850)

```cpp
bool VkVolumetrics::record(VkCommandBuffer cmd,
                           uint32_t frame_slot,
                           matter::VkImageResource& depth_image,
                           VkAccelerationStructureKHR tlas,
                           const matter::VkBufferResource& rt_parts,     // NEW
                           const matter::VkBufferResource& materials,    // NEW
                           const FrameMatrices& matrices,
                           float frame_time,
                           std::string& error) {
    ...
    const bool emission_buffers_ok =
        rt_parts.buffer != VK_NULL_HANDLE &&
        materials.buffer != VK_NULL_HANDLE;

    // --- Update per-frame scatter descriptors (depth + TLAS + SSBOs) ---
    {
        ...existing depth_info and as_write...

        // Bindings 5/6: RT part table + material table. The shader
        // statically references both, so they must always hold a valid
        // buffer; fall back to the always-alive emitter SSBO and rely on
        // emission_rays == 0 to keep it unread.
        VkDescriptorBufferInfo parts_info{
            emission_buffers_ok ? rt_parts.buffer : emitter_ssbo_.buffer, 0,
            emission_buffers_ok ? rt_parts.size : emitter_ssbo_.size};
        VkDescriptorBufferInfo mats_info{
            emission_buffers_ok ? materials.buffer : emitter_ssbo_.buffer, 0,
            emission_buffers_ok ? materials.size : emitter_ssbo_.size};

        VkWriteDescriptorSet writes[4]{};   // was [2]
        ...existing writes[0] (binding 3) and writes[1] (binding 4)...
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = scatter_sets_[current];
        writes[2].dstBinding = 5;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &parts_info;
        writes[3] = writes[2];
        writes[3].dstBinding = 6;
        writes[3].pBufferInfo = &mats_info;
        vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
    }
```

#### 5e — `record()` — push-constant fill (replace `scatter_pc.pad2 = 0.0f;`, line 981)

```cpp
    scatter_pc.camera_far = matrices.view_to_clip.m[11] /
                            (matrices.view_to_clip.m[10] + 1.0f);
    scatter_pc.emission_intensity = emission_intensity_;
    scatter_pc.emission_rays =
        (emission_enabled_ && emission_buffers_ok) ? emission_rays_ : 0u;
    scatter_pc.emission_range = std::min(emission_range_, kVolFarRange);
    scatter_pc.pad3 = 0.0f;
    scatter_pc.pad4 = 0.0f;
```

No new barriers are needed inside `record()`: the part table is host-visible/coherent (host writes are made visible by queue submission), and the device-local material buffer is covered by the widened upload barrier in §II.6c.

### II.6 Scene renderer integration

`MatterEngine3/src/render/vk_scene_renderer.cpp`

#### 6a — `RasterRecord` struct (fields end at line 339) — two pointer fields

```cpp
    uint32_t volumetrics_zone = 0;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    // Pointers (not handles): both buffers can be replaced inside
    // record_ray_traced_shadows(), which runs after this struct is built
    // but before the volumetrics record below reads them.
    const matter::VkBufferResource* vol_rt_parts = nullptr;    // NEW
    const matter::VkBufferResource* vol_materials = nullptr;   // NEW
};
```

#### 6b — Volumetrics call site inside `record_raster()` (lines 487-501)

```cpp
    // --- Volumetrics pass: froxel density + scatter + integrate ---
    if (record.volumetrics && record.volumetrics->active() &&
        record.vol_rt_parts && record.vol_materials) {
        ...
        record.volumetrics->record(
            command_buffer, record.frame_slot,
            *record.depth, record.tlas,
            *record.vol_rt_parts, *record.vol_materials,
            *record.matrices, record.frame_time, vol_error);
        ...
    }
```

At the aggregate construction site (lines 5579-5620), append the two initializers after `selected.rt_tlas.handle`:

```cpp
                        kGpuZoneVolumetrics,
                        selected.rt_tlas.handle,
                        &selected.rt_parts,
                        &selected.materials};
```

#### 6c — Widen the material upload barrier (lines 234-236)

The transfer→shader barrier after the material copy currently makes the write visible to VERTEX|FRAGMENT only. The scatter pass reads the same buffer from a compute stage, so extend the destination scope:

```cpp
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
```

#### 6d — Lifetime retention (attachments block, lines 5495-5505)

`selected.materials.lifetime` is already retained (line 5497). Retain the part table alongside the integrated volume:

```cpp
    if (volumetrics_ && volumetrics_->active()) {
        attachments.push_back(volumetrics_->vol_integrated().lifetime);
        attachments.push_back(selected.rt_parts.lifetime);
    }
```

#### 6e — Optional hardening (recommended, small)

Change `RasterRecord.tlas` from a captured handle to `const matter::VkAccelerationStructureResource*` pointing at `selected.rt_tlas`, dereferenced at the volumetrics call. This closes the pre-existing one-frame stale-TLAS window on growth frames (design spec §I.7) for the volumetrics consumer. Purely mechanical; skip if minimizing diff.

### II.7 Build integration

#### 7a — Makefile dependencies

`MatterEngine3/Makefile` (lines 240-244 and 252-255):

```makefile
build/shaders_vk/rt_surface.rchit.spv build/shaders_vk/rt_visibility.rahit.spv \
    build/shaders_vk/rt_radiance.rmiss.spv \
    build/shaders_vk/rt_lighting.rgen.spv \
    build/shaders_vk/rt_surface_test.rgen.spv: \
    shaders_vk/rt_surface_common.glsl shaders_vk/rt_geometry_types.glsl

...

build/shaders_vk/vol_density.comp.spv build/shaders_vk/vol_scatter.comp.spv \
    build/shaders_vk/vol_integrate.comp.spv \
    build/shaders_vk/composite.frag.spv: \
    shaders_vk/vol_common.glsl

build/shaders_vk/vol_scatter.comp.spv: shaders_vk/rt_geometry_types.glsl
```

#### 7b — Regenerate SPIR-V and rebuild (MSYS2 UCRT64, per CLAUDE.md)

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
TMPARGS='TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp'

# 1. Recompile shaders and regenerate the embedded header
#    (requires glslc; produces MatterEngine3/shaders_gen/embedded_spirv.h,
#    which is checked in — commit the regenerated header).
make -C MatterEngine3 vulkan-spirv $TMPARGS

# 2. Kernel library, then viewer.
make -C MatterEngine3 $TMPARGS
make -C MatterViewer windows $TMPARGS
```

`vol_scatter.comp.spv` is loaded by name from the embedded header (`create_shader_module_from_spirv("vol_scatter.comp.spv", ...)`, vk_volumetrics.cpp:520); no C++ loading changes are needed. Compile order matters: build the shader header before `vk_volumetrics.cpp`, which the Makefile dependency already enforces via `shaders_gen/embedded_spirv.h`.

### II.8 Validation checklist

1. **Compile gates.** `make -C MatterEngine3 vulkan-spirv` succeeds (glslc validates the ray-query + buffer-reference combination); full kernel + viewer builds clean.
2. **Bit-exact off state.** With *Emission rays = 0*, captured `vol_integrated` output matches pre-change output (the new code is fully gated behind `pc.emission_rays > 0u`).
3. **Glow appears.** Load the Demo world (`MatterEngine3/examples/world_demo/worlds/Demo.js` — has height fog + ChimneySmoke/WaterfallMist emitters), place or approach geometry using an emissive material (registry indices 25/26, "cool/warm light" — `emission_strength` packs color in rgb, strength in w). Enable Volumetrics: fog near the surface shows a colored glow that decays with distance, strongest where fog is densest.
4. **Debug views.** Vol debug combo -> "Scatter" shows emission in the raw scatter volume; "Integrated" shows it attenuated by transmittance.
5. **Determinism.** Freeze the camera: after ~1 s the glow is stable (no crawling), because the seed depends only on voxel + frame index and history converges.
6. **Controls respond.** Emission boost scales linearly; range slider visibly truncates far pickup; rays 1->2 reduces shimmer.
7. **Budget.** `gpu_vol_ms` HUD delta <= 0.6 ms at 1 ray, <= 1.2 ms at 2 rays (RTX-3070-class, default fog).
8. **Validation layers clean.** Run with `VK_LAYER_KHRONOS_validation`: no unwritten-descriptor or barrier hazards on bindings 5/6 (exercises the fallback path by toggling RT off if the build exposes it).
9. **Headless tests.** `make -C MatterEngine3/tests run-world-definition ... GRAPHICS=GRAPHICS_API_OPENGL_43` still passes; extend `world_definition_tests.cpp` (existing volumetrics-settings coverage) with default-value checks for the four new `VulkanVolumetricsSettings` fields.

### Touched files, summary

| File | Change |
|---|---|
| `MatterEngine3/shaders_vk/rt_geometry_types.glsl` | **new** — binding-free structs + BDA loaders |
| `MatterEngine3/shaders_vk/rt_surface_common.glsl` | replace moved block with include |
| `MatterEngine3/shaders_vk/vol_scatter.comp` | bindings 5/6, push constants, RNG + emission loop |
| `MatterEngine3/include/matter/world_definition.h` | 4 new settings fields |
| `MatterEngine3/src/render/vk_volumetrics.h` | constants, ScatterConstants 224 B, record() params, latched fields |
| `MatterEngine3/src/render/vk_volumetrics.cpp` | layout/pool/writes/push-constant fill, update_settings |
| `MatterEngine3/src/render/vk_scene_renderer.cpp` | RasterRecord fields + call site, barrier widen, lifetime retention |
| `MatterViewer/ui.cpp` | 3 controls in Volumetrics panel |
| `MatterEngine3/Makefile` | 2 dependency lines |
| `MatterEngine3/shaders_gen/embedded_spirv.h` | regenerated by `make vulkan-spirv` |
| `MatterEngine3/tests/world_definition_tests.cpp` | default-value checks (optional but recommended) |
