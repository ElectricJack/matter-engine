# Physical Atmosphere, Scalable Volumetrics, and Lit Clouds Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the procedural sky with one Rayleigh/Mie/ozone atmosphere, make froxel resolution safely selectable at runtime, and add scalable cloud self-shadowing, approximated multiple scattering, and filtered world-space cloud shadows while preserving low-lying fog and a current-cost escape hatch.

**Architecture:** Implement three cooperating representations: dirty-driven atmosphere LUTs in `VkAtmosphere`, the existing camera-aligned froxels in a runtime-sized `VkVolumetrics::FroxelBundle`, and two camera-centered sun-space cumulative-optical-depth levels in `VkCloudShadows`. A shared environment descriptor set makes the same physical sky, derived direct sun, irradiance coefficients, and cloud transmittance available to composite, RT, and volumetric shaders; a binding-free cloud-density include keeps the view-froxel and sun-space density definitions identical.

**Tech Stack:** C++17, Vulkan 1.3, GLSL 460 compute/fragment/ray-tracing shaders, SPIR-V generation through the existing Makefiles, the Matter property registry and ImGui editor, QuickJS world definitions, Bash plus `MATTER_CMD_FIFO` for engine automation, and the existing C++/Vulkan smoke-test harnesses.

## Global Constraints

- The approved design at `docs/superpowers/specs/2026-08-08-physical-atmosphere-volumetric-clouds-design.md` is authoritative.
- The physical atmosphere is the only production sky model; do not add a legacy-sky selector or retain a production call to `procedural_sky`.
- `Current cost` keeps the `160x90x128` four-RGBA16F froxel footprint and disables the additional cloud-density and cloud-shadow resources, while still using the physical atmosphere.
- Ground fog remains an independently authorable exponential low-lying medium and remains single-scattered by default.
- Cloud multiple scattering is a bounded 1-4 order approximation, not volumetric path tracing.
- Every user-facing value is registered through the existing property system and appears in the Lighting window; FIFO `set/get` reaches it without a second control path.
- Quality is manual in this version. Do not add automatic GPU detection or automatic quality scaling.
- Resource changes are transactional: create a complete replacement, swap only after success, invalidate history, and retire old Vulkan resources only after the frame-slot fence that protects their last use.
- Allocation or LUT-regeneration failure is non-fatal and leaves either the last valid resource or the specified neutral emergency resource active.
- Use `C:/msys64/ucrt64/bin/g++.exe`; native test/editor processes must receive `TMP=C:/Users/webde/AppData/Local/Temp` and `TEMP=C:/Users/webde/AppData/Local/Temp`.
- Build shader SPIR-V before the engine archive and editor whenever GLSL changes: `make -C MatterEngine3 vulkan-spirv`, then `make -C MatterEngine3`, then `make -C MatterEditor windows`.
- Run the C++ test targets sequentially with `GRAPHICS=GRAPHICS_API_OPENGL_43`; never launch the repository test suite in parallel.
- Engine CLI/FIFO automation is the primary visual harness. Every visual milestone must generate `.done`-guarded PNGs and stats, inspect the images, and embed representative absolute-path screenshots in the implementation conversation with the exact property configuration.
- Keep transient PNGs/logs under `MatterEditor/build/validation/atmosphere-clouds/<milestone>/`; do not commit them.
- Stage named files only. Do not use `git add -A`, because this worktree may contain user-owned or generated files outside this plan.

---

## File Structure

### New engine-facing files

- `MatterEngine3/include/matter/atmosphere.h` — atmosphere settings, Earth-like constants, sanitization, double-precision reference integration, and CPU direct-sun evaluation.
- `MatterEngine3/include/matter/volumetric_quality.h` — discrete froxel settings, resolved dimensions, memory accounting, enhanced-path detection, and the four manual presets.
- `MatterEngine3/include/matter/cloud_shadow_settings.h` — cloud-shadow properties, resolved clipmap dimensions, sun-frame math, invalidation policy, and memory accounting.
- `MatterEngine3/src/render/vk_atmosphere.h` / `vk_atmosphere.cpp` — LUT images, compute pipelines, dirty tracking, solar transmittance, and atmosphere diagnostics.
- `MatterEngine3/src/render/vk_cloud_shadows.h` / `vk_cloud_shadows.cpp` — sun-space level bundles, reprojection/update scheduling, safe replacement, and clear fallbacks.
- `MatterEngine3/shaders_vk/atmosphere_common.glsl` — shared Rayleigh/Mie/ozone density and ray/sphere integration routines used only by atmosphere compute shaders.
- `MatterEngine3/shaders_vk/environment_common.glsl` — binding contract and production sampling helpers for sky-view, irradiance SH, and cloud-shadow textures.
- `MatterEngine3/shaders_vk/atmosphere_transmittance.comp` — `256x64` transmittance LUT generation.
- `MatterEngine3/shaders_vk/atmosphere_multiscatter.comp` — `32x32` atmospheric multiple-scattering approximation.
- `MatterEngine3/shaders_vk/atmosphere_sky_view.comp` — `192x108` sun-relative sky-view generation.
- `MatterEngine3/shaders_vk/atmosphere_irradiance.comp` — nine RGB irradiance coefficients stored in a `3x3` RGBA16F image.
- `MatterEngine3/shaders_vk/cloud_density.glsl` — binding-free coarse/full cloud-density evaluation shared by both cloud representations.
- `MatterEngine3/shaders_vk/cloud_shadow_common.glsl` — level transforms, cumulative optical-depth sampling, edge fade, and filtered transmittance.
- `MatterEngine3/shaders_vk/cloud_shadow_reproject.comp` — reproject the previous cumulative field into the new camera/sun frame.
- `MatterEngine3/shaders_vk/cloud_shadow_density.comp` — evaluate coarse cloud extinction only for newly exposed or scheduled XY tiles.
- `MatterEngine3/shaders_vk/cloud_shadow_integrate.comp` — prefix-integrate density from the sunward boundary for scheduled columns.
- `MatterEngine3/tests/atmosphere_tests.cpp` — CPU atmosphere and direct-sun reference gates.
- `MatterEngine3/tests/volumetric_quality_tests.cpp` — grid, preset, enhanced-path, and memory-contract gates.
- `MatterEngine3/tests/cloud_shadow_tests.cpp` — sun-space coordinates, optical-depth, invalidation, filtering, and update-schedule gates.
- `MatterEngine3/tools/atmosphere_cloud_shots.sh` — one-process FIFO screenshot/stats matrix for all visual milestones.

### Existing files with focused changes

- `MatterEngine3/include/matter/world_definition.h`, `MatterEngine3/src/script/world_definition_loader.cpp`, `MatterEngine3/tests/world_definition_tests.cpp` — world-authored atmosphere, expanded volumetric settings, cloud shadows, and neutral cloud-shape extensions.
- `MatterEngine3/include/matter/cloud_layers.h`, `MatterEngine3/tests/cloud_layer_tests.cpp` — weather/detail/shape fields and the expanded GPU mirror.
- `MatterEngine3/include/matter/world_session.h`, `MatterEngine3/src/matter_engine.cpp` — render-option/session plumbing, derived physical sunlight, histories, and frame statistics.
- `MatterEngine3/src/render/vk_volumetrics.h` / `vk_volumetrics.cpp` — dynamic `FroxelBundle`, optional R16F cloud density, enhanced scatter pipeline, and per-pass timings.
- `MatterEngine3/src/render/vk_scene_renderer.h` / `vk_scene_renderer.cpp` — own/orchestrate the two new modules and the shared environment descriptor set.
- `MatterEngine3/shaders_vk/vol_common.glsl`, `vol_density.comp`, `vol_scatter.comp`, `vol_integrate.comp` — runtime dimensions, shared density, detailed self-shadowing, fog/cloud phase separation, and scattering orders.
- `MatterEngine3/shaders_vk/composite.frag`, `rt_lighting.rgen`, `MatterEngine3/shaders_vk/sky_common.glsl` — replace all production procedural-sky consumers, add primary/secondary cloud shadow sampling, then delete `sky_common.glsl`.
- `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`, `MatterEditor/Makefile`, `MatterEngine3/shaders_gen/embedded_spirv.h` — source lists, dependencies, test targets, and regenerated embedded shaders.
- `MatterEditor/src/editor_props.h` / `editor_props.cpp`, `property_editor.cpp`, `ui_lighting_controls.cpp`, `ui.h`, `main.cpp` — bindings, Lighting-window groups/presets, world baselines, effective resource readouts, and GPU timings.
- `MatterEngine3/tests/property_editor_tests.cpp`, `MatterEngine3/tests/vulkan_smoke_tests.cpp` — property/FIFO/preset behavior and Vulkan layout/lifetime/numerical tests.
- `MatterEditor/src/main.cpp`, `MatterEditor/src/viewer_commands.h`, `MatterEngine3/docs/rendering.md` — append stable stats fields and document the automated capture matrix; the generic command grammar itself remains unchanged.

## Stable Cross-Task Interfaces

Later tasks must use these names and values exactly.

```cpp
namespace matter {

struct AtmosphereSettings {
    float sea_level_y = 0.0f;
    float rayleigh_scale = 1.0f;
    float mie_scale = 1.0f;
    float mie_anisotropy = 0.8f;
    float ozone_scale = 1.0f;
    float ground_albedo = 0.1f;
};

struct AtmosphereTransmittanceResult {
    Float3 transmittance{};
    bool valid = false;
};

AtmosphereSettings sanitize_atmosphere(const AtmosphereSettings&);
AtmosphereTransmittanceResult atmosphere_transmittance_reference(
    const AtmosphereSettings&, double observer_world_y,
    const Float3& ray_direction, int sample_count = 256);
Float3 atmosphere_to_sun_from_elevation_deg(double elevation_deg);
Float3 atmosphere_direct_sun_transmittance(
    const AtmosphereSettings&, double observer_world_y,
    const Float3& to_sun, int sample_count = 256);
Float3 atmosphere_direct_sun_transmittance(
    const AtmosphereSettings&, double observer_world_y,
    double elevation_deg, int sample_count = 256);
Float3 atmosphere_direct_sun_rgb(
    const AtmosphereSettings&, double observer_world_y, const Float3& to_sun,
    const Float3& authored_modifier, const Float3& live_tint,
    float live_multiplier);

enum class FroxelXyScale : int32_t { X0_5 = 0, X0_75, X1_0, X1_5, X2_0 };
enum class FroxelDepthSlices : int32_t { D64 = 0, D96, D128, D192, D256 };

struct FroxelGridDimensions { uint32_t width, height, depth; };

struct VulkanVolumetricsSettings {
    bool enabled = false;
    float temporal_blend = 0.85f;
    float phase_g = 0.3f;
    float vol_debug_view = 0.0f;
    FroxelXyScale froxel_xy_scale = FroxelXyScale::X1_0;
    FroxelDepthSlices froxel_depth_slices = FroxelDepthSlices::D128;
    int32_t local_sun_march_steps = 8;
    float local_sun_march_distance_m = 250.0f;
    int32_t multiple_scattering_orders = 2;
    float multiple_scattering_strength = 0.55f;
    float powder_strength = 0.25f;
};

struct CloudShadowSettings {
    bool enabled = true;
    int32_t near_resolution = 1;      // labels: 128, 256, 512
    int32_t near_depth_slices = 1;    // labels: 16, 32, 48
    float near_coverage_m = 1800.0f;
    int32_t far_resolution = 1;       // labels: 64, 128, 256
    int32_t far_depth_slices = 1;     // labels: 16, 24, 32
    float far_coverage_m = 4000.0f;
    float filter_scale = 1.0f;
    float update_fraction = 0.25f;
};

struct CloudShadowLevelDesc {
    uint32_t width = 0, height = 0, depth = 0;
    float coverage_m = 0.0f;
};

struct CloudShadowFrame {
    Mat4f world_to_uvw{};
    Mat4f uvw_to_world{};
    Float3 snapped_center{};
    Float3 incoming_light_axis{};
    float voxel_xy_m = 0.0f;
    float voxel_depth_m = 0.0f;
    bool valid = false;
};

enum class VolumetricQualityPreset : int32_t {
    CurrentCost = 0, Improved, High, Ultra, Custom
};

FroxelGridDimensions resolve_froxel_grid(const VulkanVolumetricsSettings&);
uint64_t estimate_froxel_bytes(FroxelGridDimensions, bool enhanced_clouds);
bool enhanced_cloud_lighting(const VulkanVolumetricsSettings&,
                             const CloudShadowSettings&);
void apply_volumetric_quality_preset(VolumetricQualityPreset,
                                     VulkanVolumetricsSettings&,
                                     CloudShadowSettings&);
VolumetricQualityPreset identify_volumetric_quality_preset(
    const VulkanVolumetricsSettings&, const CloudShadowSettings&);
std::array<CloudShadowLevelDesc, 2> resolve_cloud_shadow_levels(
    const CloudShadowSettings&);
uint64_t estimate_cloud_shadow_bytes(const CloudShadowSettings&);
CloudShadowFrame make_cloud_shadow_frame(
    const CloudShadowLevelDesc&, const Float3& camera_world,
    const Float3& sun_direction);
bool cloud_shadow_requires_full_invalidation(
    const CloudShadowFrame& previous, const CloudShadowFrame& next,
    float sun_angle_delta_deg);

}  // namespace matter
```

Preset values are fixed for reproducible tests and screenshots:

| Preset | XY/depth | Local march | Orders / strength / powder | Near | Far | Update |
|---|---|---|---|---|---|---|
| Current cost | `1.0 / 128` | `0 / 250 m` | `1 / 0 / 0` | disabled; stored `256x256x32 / 1800 m` | disabled; stored `128x128x24 / 4000 m` | stored `0.25`, no dispatch |
| Improved | `1.0 / 128` | `8 / 250 m` | `2 / 0.55 / 0.25` | `256x256x32 / 1800 m` | `128x128x24 / 4000 m` | `0.25` |
| High | `1.5 / 192` | `12 / 350 m` | `3 / 0.70 / 0.35` | `512x512x32 / 2200 m` | `256x256x24 / 4500 m` | `0.50` |
| Ultra | `2.0 / 256` | `24 / 500 m` | `4 / 0.85 / 0.50` | `512x512x48 / 2500 m` | `256x256x32 / 5000 m` | `1.00` |

The shared production descriptor contract is set `1` in composite, RT lighting, and enhanced volumetric scattering:

```glsl
layout(set = 1, binding = 0) uniform sampler2D atmosphere_sky_view;
layout(set = 1, binding = 1) uniform sampler2D atmosphere_irradiance_sh;
layout(set = 1, binding = 2) uniform sampler3D cloud_shadow_near_0;
layout(set = 1, binding = 3) uniform sampler3D cloud_shadow_near_1;
layout(set = 1, binding = 4) uniform sampler3D cloud_shadow_far_0;
layout(set = 1, binding = 5) uniform sampler3D cloud_shadow_far_1;
layout(set = 1, binding = 6, std140) uniform EnvironmentBlock {
    mat4 cloud_world_to_uvw[2];
    vec4 cloud_state;   // enabled, near active index, far active index, edge fade
    vec4 cloud_filter;  // near voxel m, far voxel m, filter scale, sun diameter rad
} environment;
```

`VkAtmosphere` supplies bindings 0-1; `VkCloudShadows` supplies bindings 2-5 and the transform values. The renderer always binds valid resources: neutral emergency sky/irradiance and 1x1x1 clear cloud textures cover initialization, disabled settings, and allocation failure.

---

### Task 1: Establish the CLI visual/performance baseline harness

**Files:**
- Create: `MatterEngine3/tools/atmosphere_cloud_shots.sh`
- Modify: `MatterEngine3/docs/rendering.md`
- Test: `MatterEngine3/tools/atmosphere_cloud_shots.sh`

**Interfaces:**
- Consumes: existing `MATTER_CMD_FIFO` commands `set`, `get`, `cam`, `stats`, `shot`, and `quit`; existing `viewer: bake ready` log line and `<png>.done` marker.
- Produces: `tools/atmosphere_cloud_shots.sh <suite> <label> <out-dir>`, command/config logs, one positional `STATS` log, one telemetry-metrics log, and milestone PNGs from a single editor process.

- [ ] **Step 1: Write the shell syntax/readiness test before the script exists**

Run:

```powershell
C:\msys64\usr\bin\bash.exe -n MatterEngine3/tools/atmosphere_cloud_shots.sh
```

Expected: FAIL because `MatterEngine3/tools/atmosphere_cloud_shots.sh` does not exist.

- [ ] **Step 2: Add the focused FIFO driver**

Use the established lifecycle from `viewer_shots.sh`, but require both readiness lines and record every command. The public structure must be:

```bash
#!/usr/bin/env bash
set -euo pipefail
SUITE="${1:?usage: atmosphere_cloud_shots.sh <suite> <label> <out-dir>}"
LABEL="${2:?usage: atmosphere_cloud_shots.sh <suite> <label> <out-dir>}"
OUT="${3:?usage: atmosphere_cloud_shots.sh <suite> <label> <out-dir>}"
mkdir -p "$OUT"
FIFO="/tmp/matter_atmosphere_clouds_$$.fifo"
LOG="$OUT/${LABEL}_viewer.log"
COMMANDS="$OUT/${LABEL}_commands.log"

send() { printf '%s\n' "$*" | tee -a "$COMMANDS" > "$FIFO"; }
capture() {
  local name="$1" png="$OUT/${LABEL}_${1}.png"
  rm -f "$png" "${png}.done"
  send "stats $name"
  send "shot $png"
  for _ in $(seq 1 60); do [ -e "${png}.done" ] && return; sleep 1; done
  echo "ERROR: screenshot timed out: $png" >&2
  exit 1
}
```

Launch `MatterEditor/build/windows/editor.exe` once with `MATTER_WORLD`, `MATTER_CMD_FIFO`, `TMP`, and `TEMP`; trap process termination and FIFO cleanup; poll for both `viewer: bake ready` and `MATTER_CMD_FIFO: listening`; settle two seconds after a camera/property batch; always issue `quit` and `wait`. After exit, write `grep '^STATS,'` output to `${LABEL}_stats.log` and telemetry JSON lines containing `"gpu_volumetrics_ms"` to `${LABEL}_metrics.log`. Add named suites `baseline`, `atmosphere`, `froxel`, `cloud-lighting`, `cloud-shadows`, and `final`; only `baseline` is executed in this task, while later tasks activate the properties used by the other cases.

- [ ] **Step 3: Make the script executable and verify its control-flow contract**

Run:

```powershell
C:\msys64\usr\bin\bash.exe -n MatterEngine3/tools/atmosphere_cloud_shots.sh
C:\msys64\usr\bin\bash.exe -lc "grep -q 'viewer: bake ready' MatterEngine3/tools/atmosphere_cloud_shots.sh && grep -q 'MATTER_CMD_FIFO: listening' MatterEngine3/tools/atmosphere_cloud_shots.sh && grep -q '\.done' MatterEngine3/tools/atmosphere_cloud_shots.sh && grep -q 'send \"quit\"' MatterEngine3/tools/atmosphere_cloud_shots.sh"
```

Expected: both commands exit 0.

- [ ] **Step 4: Build and capture the pre-change baseline**

Run sequentially from the repository root:

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\bash.exe MatterEngine3/tools/atmosphere_cloud_shots.sh baseline pre-atmosphere MatterEditor/build/validation/atmosphere-clouds/baseline
```

Expected: editor exits cleanly; every PNG has a `.done` sibling; `pre-atmosphere_stats.log` contains a positional `STATS` row; `pre-atmosphere_metrics.log` contains `gpu_volumetrics_ms`; the command log records the camera and capture commands.

- [ ] **Step 5: Inspect and show the baseline artifacts**

Open each generated PNG, reject black/partial/UI-obscured captures, and inspect the viewer/stats logs for Vulkan errors. In the implementation conversation, embed at least one representative image using its absolute local path and caption it `pre-change procedural sky; render.volumetrics configuration from command log`. This is required evidence for the later atmosphere and current-cost comparisons.

- [ ] **Step 6: Document the suite contract and commit**

Add the invocation, readiness rules, `.done` rule, output layout, and single-process cleanup behavior to `MatterEngine3/docs/rendering.md`.

```powershell
git add MatterEngine3/tools/atmosphere_cloud_shots.sh MatterEngine3/docs/rendering.md
git commit -m "test: add atmosphere and cloud CLI capture harness"
```

---

### Task 2: Add the CPU atmosphere model and direct-sun reference

**Files:**
- Create: `MatterEngine3/include/matter/atmosphere.h`
- Create: `MatterEngine3/tests/atmosphere_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `matter::Float3` from `matter/math_types.h` and the engine sun convention where `sun_direction` points from sun toward scene.
- Produces: `AtmosphereSettings`, `sanitize_atmosphere`, `atmosphere_transmittance_reference`, `atmosphere_direct_sun_transmittance`, `atmosphere_direct_sun_rgb`, and fixed Earth-like coefficients mirrored by `atmosphere_common.glsl` in Task 6.

- [ ] **Step 1: Write failing noon/horizon/below-horizon tests**

The test file must pin these contracts:

```cpp
void test_direct_sun_changes_with_elevation() {
    matter::AtmosphereSettings s{};
    const auto noon = matter::atmosphere_direct_sun_transmittance(s, 0.0, 90.0);
    const auto low  = matter::atmosphere_direct_sun_transmittance(s, 0.0, 5.0);
    const auto down = matter::atmosphere_direct_sun_transmittance(s, 0.0, -5.0);
    CHECK(noon.x > 0.0f && noon.z > 0.0f, "noon sun survives atmosphere");
    CHECK(low.x / low.z > noon.x / noon.z, "low sun is warmer than noon");
    CHECK(low.x + low.y + low.z < noon.x + noon.y + noon.z,
          "low sun is dimmer than noon");
    CHECK(down.x == 0.0f && down.y == 0.0f && down.z == 0.0f,
          "planet occludes below-horizon direct sun at sea level");
}
```

Also test finite sanitization, `T` in `[0,1]`, monotonic optical depth with path length, elevated-observer horizon extension, and `atmosphere_direct_sun_rgb = extraterrestrial * transmittance * authored modifier * tint * multiplier` component by component.

- [ ] **Step 2: Add the target and verify red**

Add `ATMOSPHERE_TARGET`, compile `atmosphere_tests.cpp`, and add `run-atmosphere` to `.PHONY`.

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: FAIL because the atmosphere settings/functions are not defined.

- [ ] **Step 3: Implement the settings, constants, and reference integral**

Use metres and linear RGB. Pin the constants in the header so CPU and GLSL copies can be reviewed together:

```cpp
inline constexpr double kAtmospherePlanetRadiusM = 6360000.0;
inline constexpr double kAtmosphereTopRadiusM = 6460000.0;
inline constexpr double kRayleighScaleHeightM = 8000.0;
inline constexpr double kMieScaleHeightM = 1200.0;
inline constexpr double kOzoneCenterHeightM = 25000.0;
inline constexpr double kOzoneHalfWidthM = 15000.0;
inline constexpr double kRayleighScattering[3] =
    {5.802e-6, 13.558e-6, 33.100e-6};
inline constexpr double kMieScattering[3] =
    {3.996e-6, 3.996e-6, 3.996e-6};
inline constexpr double kMieExtinction[3] =
    {4.440e-6, 4.440e-6, 4.440e-6};
inline constexpr double kOzoneAbsorption[3] =
    {0.650e-6, 1.881e-6, 0.085e-6};
inline constexpr double kExtraterrestrialSolarRgb[3] = {1.0, 1.0, 1.0};
```

`atmosphere_transmittance_reference` uses double precision, ray/sphere intersections, midpoint integration, and 256 samples by default. Observer radius is `planet_radius + clamp(observer_world_y - sea_level_y, 0, 100000)`; this phase deliberately does not support an orbit camera. Rayleigh density is `exp(-height / 8000)`, Mie density is `exp(-height / 1200)`, ozone is `max(0, 1 - abs(height-25000)/15000)`, and non-finite optical depth returns clear transmittance plus `valid=false`. Direct sunlight returns zero when the observer-to-sun ray intersects the planet before exiting the atmosphere.

- [ ] **Step 4: Run the CPU atmosphere tests green**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: PASS with finite transmittance and all solar-elevation assertions green.

- [ ] **Step 5: Commit**

```powershell
git add MatterEngine3/include/matter/atmosphere.h MatterEngine3/tests/atmosphere_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat: add physical atmosphere reference model"
```

---

### Task 3: Define froxel, cloud-shadow, and preset quality contracts

**Files:**
- Create: `MatterEngine3/include/matter/volumetric_quality.h`
- Create: `MatterEngine3/include/matter/cloud_shadow_settings.h`
- Create: `MatterEngine3/tests/volumetric_quality_tests.cpp`
- Create: `MatterEngine3/tests/cloud_shadow_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: the stable settings declarations and preset table in this plan.
- Produces: the exact helper signatures under Stable Cross-Task Interfaces, plus `CloudShadowLevelDesc`, `resolve_cloud_shadow_levels`, `estimate_cloud_shadow_bytes`, `make_cloud_shadow_frame`, and `cloud_shadow_requires_full_invalidation`.

- [ ] **Step 1: Write failing discrete-grid and memory tests**

Pin all scale/depth values, including the half-pixel rounding rule:

```cpp
const matter::FroxelGridDimensions expected_xy[5] = {
    {80, 45, 128}, {120, 68, 128}, {160, 90, 128},
    {240, 135, 128}, {320, 180, 128}
};
```

For base dimensions, assert four RGBA16F images consume `160*90*128*8*4 = 58982400` bytes and the enhanced R16F cloud-density image adds `160*90*128*2 = 3686400` bytes. Test all 25 XY/depth combinations, enum sanitization to `1.0/128`, and monotonic memory growth.

- [ ] **Step 2: Write failing preset and clipmap tests**

For each of the four named presets, call `apply_volumetric_quality_preset`, compare every field to the table in this plan, and assert `identify_volumetric_quality_preset` returns the same preset. Change one field and expect `Custom`. Assert only Current cost makes `enhanced_cloud_lighting` false.

For clipmaps, assert Improved resolves to near `256x256x32` and far `128x128x24`; persistent memory includes one R16F density scratch plus two R16F cumulative ping-pong images per level:

```cpp
expected = 3ull * 2ull *
    (256ull*256ull*32ull + 128ull*128ull*24ull);
```

Also assert a sun-frame round-trip maps camera center to lateral UV `(0.5,0.5)`, its incoming-light axis is orthonormal, a `2.1 degree` change invalidates history while `0.5 degree` does not, and non-finite transforms fail closed.

- [ ] **Step 3: Add targets and prove both tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-volumetric-quality CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-shadows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: both FAIL on missing headers/functions.

- [ ] **Step 4: Implement exact enum lookup tables, sanitizers, and presets**

Use `std::lround(160*scale)` and `std::lround(90*scale)`. Store enum-valued fields as `enum class : int32_t` so `PropBuilder::enums` can expose labels directly. `enhanced_cloud_lighting` is true when any of these is true: local march steps greater than zero, scattering orders greater than one, powder strength greater than zero, or cloud shadows enabled.

```cpp
inline FroxelGridDimensions resolve_froxel_grid(
    const VulkanVolumetricsSettings& s) {
    const float xy[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    const uint32_t z[] = {64, 96, 128, 192, 256};
    const int xi = std::clamp(static_cast<int>(s.froxel_xy_scale), 0, 4);
    const int zi = std::clamp(static_cast<int>(s.froxel_depth_slices), 0, 4);
    return {static_cast<uint32_t>(std::lround(160.0f * xy[xi])),
            static_cast<uint32_t>(std::lround(90.0f * xy[xi])), z[zi]};
}
```

The sun frame uses incoming light (`-sun_direction`) as +Z; choose a fallback axis when it is within `0.99` of world up, then construct normalized X/Y by cross products. Span a camera-centered world cube of `coverage_m` in the sun frame on all three axes, with the outer `8%` as a guard band; this keeps both local cloud decks and receivers inside the cumulative path without adding a separate authored depth-range property. Snap lateral clipmap centers to one XY voxel and the sun-axis center to one depth voxel to stabilize translation. Full invalidation occurs for no history, non-finite input, coverage/dimension changes, movement beyond the guard band, or a sun-direction delta above `2.0` degrees.

- [ ] **Step 5: Run both targets green**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-volumetric-quality CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-shadows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: PASS.

- [ ] **Step 6: Commit**

```powershell
git add MatterEngine3/include/matter/volumetric_quality.h MatterEngine3/include/matter/cloud_shadow_settings.h MatterEngine3/tests/volumetric_quality_tests.cpp MatterEngine3/tests/cloud_shadow_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat: define scalable volumetric quality settings"
```

---

### Task 4: Add world authoring and render/session plumbing

**Files:**
- Modify: `MatterEngine3/include/matter/world_definition.h`
- Modify: `MatterEngine3/include/matter/cloud_layers.h`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEditor/src/ui.h`
- Modify: `MatterEditor/src/main.cpp`
- Modify: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `MatterEngine3/tests/cloud_layer_tests.cpp`

**Interfaces:**
- Consumes: `AtmosphereSettings`, `VulkanVolumetricsSettings`, and `CloudShadowSettings` from Tasks 2-3.
- Produces: `WorldSettings::atmosphere`, `WorldSettings::volumetrics`, `WorldSettings::cloud_shadows`; matching `RenderOptions` fields; `WorldSession::world_atmosphere`, `world_volumetrics`, and `world_cloud_shadows`; expanded `CloudLayer`/`GpuCloudLayer` data.

- [ ] **Step 1: Write failing world-loader tests**

Add a world fixture with:

```javascript
static atmosphere = {
  seaLevelY: 12, rayleighScale: 1.1, mieScale: 0.8,
  mieAnisotropy: 0.76, ozoneScale: 1.2, groundAlbedo: 0.18
};
static volumetrics = {
  enabled: true, temporalBlend: 0.7, phaseG: 0.2,
  froxelXyScale: "1.5x", froxelDepthSlices: 192,
  localSunMarchSteps: 12, localSunMarchDistanceM: 350,
  multipleScatteringOrders: 3, multipleScatteringStrength: 0.7,
  powderStrength: 0.35
};
static cloudShadows = {
  enabled: true, nearResolution: 512, nearDepthSlices: 32,
  nearCoverageM: 2200, farResolution: 256, farDepthSlices: 24,
  farCoverageM: 4500, filterScale: 1.25, updateFraction: 0.5
};
static fog = { clouds: [{
  minHeight: 300, maxHeight: 650, maxDensity: 0.02,
  weatherScale: 0.00025, weatherInfluence: 0.6,
  detailScale: 0.012, detailErosion: 0.35, shapeBias: -0.1
}] };
```

Assert exact extraction. Add rejection/sanitization cases for non-finite atmosphere values, unrecognized discrete labels, order `0/5`, update fraction outside `[0,1]`, and a fifth cloud layer. Existing worlds with none of the new fields must compare equal to compiled defaults.

- [ ] **Step 2: Write failing cloud packing tests**

Extend `CloudLayer` with neutral defaults:

```cpp
float weather_scale = 0.00025f;
float weather_influence = 0.0f;
float detail_scale = 0.012f;
float detail_erosion = 0.0f;
float shape_bias = 0.0f;
```

Expect `GpuCloudLayer` to become 96 bytes by appending two `vec4`-equivalent groups: `(weather_scale, weather_influence, detail_scale, detail_erosion)` and `(shape_bias, 0, 0, 0)`. Assert `pack_cloud_layer` preserves all five values and `sanitize_cloud_layer` clamps influences/erosion to `[0,1]`, positive scales to safe defaults, and shape bias to `[-1,1]`.

- [ ] **Step 3: Run the loader and cloud tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-world-definition CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-layers CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: FAIL on missing fields/settings.

- [ ] **Step 4: Implement extraction with exact script spellings**

Add `extract_atmosphere`, extend `extract_volumetrics`, and add `extract_cloud_shadows`. Parse the camelCase names shown in Step 1. Accept froxel scale labels `"0.5x"`, `"0.75x"`, `"1x"`, `"1.5x"`, and `"2x"`; accept slice integers `64/96/128/192/256`; reject any other discrete value with the full property path. Call the sanitizers before storing. Append the five optional cloud fields to `extract_cloud_layers` without changing the established height/coverage arithmetic.

```cpp
if (!extract_atmosphere(context, world_class, desc, definition, error) ||
    !extract_fog(context, world_class, desc, definition, error) ||
    !extract_volumetrics(context, world_class, desc, definition, error) ||
    !extract_cloud_shadows(context, world_class, desc, definition, error)) {
    return false;
}
definition.settings.atmosphere =
    sanitize_atmosphere(definition.settings.atmosphere);
```

Remove the old inline `VulkanVolumetricsSettings` definition from `world_definition.h`; include `matter/atmosphere.h`, `matter/volumetric_quality.h`, and `matter/cloud_shadow_settings.h` there so every consumer uses one canonical type definition.

- [ ] **Step 5: Plumb settings through session and editor state**

Add these fields to `RenderOptions` and `ViewerStats`:

```cpp
matter::AtmosphereSettings atmosphere{};
matter::VulkanVolumetricsSettings volumetrics{};
matter::CloudShadowSettings cloud_shadows{};
```

Mirror authored defaults in `WorldSession::Impl`, expose the three `world_*` accessors, adopt them after `BakeFinished` before property baselines are captured, and pass the live values through each `WorldSession::render`. Preserve the existing replay rule: environment/FIFO property overrides remain able to drive a replay even when world-authored values are not adopted.

- [ ] **Step 6: Run loader/cloud tests and compile the engine**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-world-definition CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-layers CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: all PASS; engine archive builds.

- [ ] **Step 7: Commit**

```powershell
git add MatterEngine3/include/matter/world_definition.h MatterEngine3/include/matter/cloud_layers.h MatterEngine3/src/script/world_definition_loader.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEditor/src/ui.h MatterEditor/src/main.cpp MatterEngine3/tests/world_definition_tests.cpp MatterEngine3/tests/cloud_layer_tests.cpp
git commit -m "feat: plumb atmosphere and cloud quality settings"
```

---

### Task 5: Register every setting and add Lighting-window presets

**Files:**
- Modify: `MatterEditor/src/editor_props.h`
- Modify: `MatterEditor/src/editor_props.cpp`
- Modify: `MatterEditor/src/property_editor.cpp`
- Modify: `MatterEditor/src/ui_lighting_controls.cpp`
- Modify: `MatterEditor/src/ui.h`
- Modify: `MatterEditor/src/main.cpp`
- Modify: `MatterEngine3/tests/property_editor_tests.cpp`
- Modify: `MatterEngine3/tests/props_tests.cpp`

**Interfaces:**
- Consumes: the settings and preset helpers from Tasks 2-4 and the existing generic `parse_and_set` FIFO property path.
- Produces: World-scoped bindings `render.atmosphere`, `render.volumetrics`, `render.cloud_shadows`, and the expanded `render.clouds`; a derived Current cost/Improved/High/Ultra/Custom display; preset buttons that change ordinary registered fields.

- [ ] **Step 1: Write failing schema and generic-set tests**

Bind compiled-default instances and assert these exact generic edits succeed and round-trip through property serialization/parsing:

```text
set render.atmosphere.mie_anisotropy 0.72
set render.volumetrics.froxel_xy_scale 1.5x
set render.volumetrics.froxel_depth_slices 192
set render.volumetrics.multiple_scattering_orders 3
set render.cloud_shadows.near_resolution 512
set render.cloud_shadows.update_fraction 0.5
set render.clouds.layer1_detail_erosion 0.4
```

Assert enum labels are case-insensitive, invalid labels do not mutate storage, all four bindings are `Scope::World`, and no new field carries `RequiresReload`. Add a preset test that applies Improved through the same descriptors/setters, then changes one value and expects the derived state `Custom`.

- [ ] **Step 2: Run the property tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-props CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-property-editor CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: FAIL because the groups/descriptors do not exist.

- [ ] **Step 3: Register exact atmosphere and volumetric descriptors**

Use these labels/ranges:

```cpp
// render.atmosphere
sea_level_y:          -1000..10000 m
rayleigh_scale:        0..4
mie_scale:             0..4
mie_anisotropy:       -0.2..0.99
ozone_scale:           0..4
ground_albedo:         0..1

// render.volumetrics additions
froxel_xy_scale:       {0.5x,0.75x,1x,1.5x,2x}
froxel_depth_slices:   {64,96,128,192,256}
local_sun_march_steps: 0..32
local_sun_march_distance_m: 0..1000 m
multiple_scattering_orders: 1..4
multiple_scattering_strength: 0..1
powder_strength:       0..1
```

Keep `phase_g` documented as ground fog/haze anisotropy. Add `MATTER_ATMOSPHERE_*`, `MATTER_FROXEL_XY_SCALE`, `MATTER_FROXEL_DEPTH_SLICES`, and `MATTER_CLOUD_SCATTER_ORDERS` environment names for deterministic replay automation, matching the existing lighting/volumetrics replay rationale.

- [ ] **Step 4: Register exact cloud-shadow and cloud-shape descriptors**

Use enum labels from the stable interfaces and ranges: coverage `250..10000 m`, filter scale `0..4`, update fraction `0.0625..1`. Expand the existing cloud-layer macro with `weather_scale`, `weather_influence`, `detail_scale`, `detail_erosion`, and `shape_bias` for all four layers. Neutral defaults must serialize sparsely, so opening/saving an old world without editing does not add these fields.

```cpp
#define CLOUD_SHAPE_FIELDS(i) \
    CLOUD_PROP(i, weather_scale, "layer" #i "_weather_scale", matter::props::Type::Float).label("Weather scale").range(1e-6f, 0.1f).log(), \
    CLOUD_PROP(i, weather_influence, "layer" #i "_weather_influence", matter::props::Type::Float).label("Weather influence").range(0.0f, 1.0f), \
    CLOUD_PROP(i, detail_scale, "layer" #i "_detail_scale", matter::props::Type::Float).label("Detail scale").range(1e-5f, 0.2f).log(), \
    CLOUD_PROP(i, detail_erosion, "layer" #i "_detail_erosion", matter::props::Type::Float).label("Detail erosion").range(0.0f, 1.0f), \
    CLOUD_PROP(i, shape_bias, "layer" #i "_shape_bias", matter::props::Type::Float).label("Shape bias").range(-1.0f, 1.0f)
```

- [ ] **Step 5: Draw groups, presets, and effective readouts in Lighting**

Draw in this order: Lighting, Atmosphere, Volumetrics, Fog, Cloud Shadows, Clouds. Extend `Reset to World` to all six bindings and update its tooltip. Above Volumetrics, show four preset buttons and derive the selected name by `identify_volumetric_quality_preset`; never store a duplicate preset backing field.

Apply a preset as one UI event by constructing target structs, then calling the appropriate `matter::props::set_enum`, `set_int`, `set_float`, and `set_bool` functions on the two bindings for every changed descriptor. This marks persistence dirty through the normal property path. Under the buttons show read-only text sourced from `ViewerStats`: requested/effective froxel dimensions, froxel MiB, cloud-shadow MiB, and last allocation error.

```cpp
template <class T>
void apply_registered_fields(matter::props::Binding& binding,
                             const T& target) {
    for (uint32_t i = 0; i < binding.schema().field_count; ++i) {
        const matter::props::Desc& d = binding.schema().fields[i];
        switch (d.type) {
        case matter::props::Type::Bool:
            matter::props::set_bool(binding, d,
                matter::props::get_bool(&target, d)); break;
        case matter::props::Type::Enum:
            matter::props::set_enum(binding, d,
                matter::props::get_enum(&target, d)); break;
        case matter::props::Type::Int:
            matter::props::set_int(binding, d,
                matter::props::get_int(&target, d)); break;
        case matter::props::Type::Float:
            matter::props::set_float(binding, d,
                matter::props::get_float(&target, d)); break;
        default: break;
        }
    }
}
VulkanVolumetricsSettings target_vol = *live_vol;
CloudShadowSettings target_shadows = *live_shadows;
apply_volumetric_quality_preset(clicked, target_vol, target_shadows);
apply_registered_fields(*vol_binding, target_vol);
apply_registered_fields(*shadow_binding, target_shadows);
```

- [ ] **Step 6: Reset/adopt baselines and run tests green**

Extend `reset_world_scope_controls` and post-bake adoption for the two new structs before `EditorProps::on_world_connected` captures baselines.

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-props CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-property-editor CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: tests PASS and editor builds. Start the editor once and use FIFO `set/get` for all seven commands from Step 1; verify the `get` responses in the log without using the mouse.

- [ ] **Step 7: Commit**

```powershell
git add MatterEditor/src/editor_props.h MatterEditor/src/editor_props.cpp MatterEditor/src/property_editor.cpp MatterEditor/src/ui_lighting_controls.cpp MatterEditor/src/ui.h MatterEditor/src/main.cpp MatterEngine3/tests/property_editor_tests.cpp MatterEngine3/tests/props_tests.cpp
git commit -m "feat: expose atmosphere and cloud quality properties"
```

---

### Task 6: Build `VkAtmosphere` and validate its LUTs on GPU

**Files:**
- Create: `MatterEngine3/src/render/vk_atmosphere.h`
- Create: `MatterEngine3/src/render/vk_atmosphere.cpp`
- Create: `MatterEngine3/shaders_vk/atmosphere_common.glsl`
- Create: `MatterEngine3/shaders_vk/atmosphere_transmittance.comp`
- Create: `MatterEngine3/shaders_vk/atmosphere_multiscatter.comp`
- Create: `MatterEngine3/shaders_vk/atmosphere_sky_view.comp`
- Create: `MatterEngine3/shaders_vk/atmosphere_irradiance.comp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEditor/Makefile`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Regenerate: `MatterEngine3/shaders_gen/embedded_spirv.h`

**Interfaces:**
- Consumes: `AtmosphereSettings` and CPU reference routines from Task 2.
- Produces: `viewer::VkAtmosphere`, fixed-size sky resources, a committed settings serial, and images exposed by `sky_view()` and `irradiance_sh()`; no production shader consumes them until Task 7.

- [ ] **Step 1: Add a failing Vulkan smoke case for LUT numerics and dirtiness**

The smoke case must initialize `VkAtmosphere`, record its passes, read back transmittance texels representing sea-level zenith, sea-level near-horizon, and 25 km zenith, and compare RGB to the 256-step CPU reference with absolute error `<= 0.025`. Assert every value is finite and `[0,1]`. Record an unchanged second frame and assert the generation serial does not advance; change `mie_scale`, record, and assert it advances once.

- [ ] **Step 2: Add shader targets and prove compilation/tests red**

Append the four `.spv` outputs to `VK_SPV`, add include dependencies on `atmosphere_common.glsl`, and include the renderer source in engine/editor source lists.

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: FAIL until the module/shaders exist.

- [ ] **Step 3: Implement the shared GPU model and LUT mappings**

Mirror every Task 2 coefficient literally in `atmosphere_common.glsl`. Use these image contracts:

```glsl
// transmittance: observer radius/altitude vs zenith cosine
layout(set=0,binding=0, rgba16f) uniform writeonly image2D transmittance_out; // 256x64
// multiscatter: altitude vs sun zenith cosine
layout(set=0,binding=0) uniform sampler2D transmittance_tex;
layout(set=0,binding=1, rgba16f) uniform writeonly image2D multiscatter_out;  // 32x32
// sky view: relative azimuth vs view zenith; sun angle and camera radius in PC
layout(set=0,binding=2, rgba16f) uniform writeonly image2D sky_view_out;      // 192x108
// irradiance: one invocation per SH coefficient, stored row-major in 3x3
layout(set=0,binding=3, rgba16f) uniform writeonly image2D irradiance_out;   // 3x3
```

Map transmittance `v` linearly over altitude `0..100 km` and `u` over zenith cosine `-1..1`; rays intersecting the planet before the atmosphere boundary store zero. Use 40 midpoint samples for that integral. For each `32x32` multiscatter texel, integrate 16 deterministic Fibonacci-sphere directions with 20 ray steps, accumulate single-scatter source `L1` and the fraction `r` scattered back into the medium, and store the bounded geometric-series result `L1 / max(1-r, 0.05)` with `r` clamped to `0.95`. For sky view, map `u` to relative azimuth `[-pi,pi]` and `v` to view zenith `[0,pi]`, then integrate 32 midpoint samples of `sun_radiance * transmittance_to_sun * (rayleigh_sigma*rayleigh_phase + mie_sigma*mie_phase) + multiscatter_source`; a planet hit adds Lambertian ground albedo illuminated by direct sun plus sky. The irradiance pass uses a fixed `32x16` spherical quadrature over the sky-view image and writes the nine real-SH coefficients row-major. Apply Kahan-style compensated RGB accumulation per coefficient so results do not depend on reduction order.

- [ ] **Step 4: Implement transactional LUT ownership and dirty rules**

`VkAtmosphere` must expose:

```cpp
class VkAtmosphere {
public:
    bool init(matter::VulkanDevice&, std::string& error);
    void request_settings(const matter::AtmosphereSettings&);
    bool record(VkCommandBuffer, float camera_world_y,
                const matter::Float3& to_sun, std::string& error);
    const matter::VkImageResource& sky_view() const;
    const matter::VkImageResource& irradiance_sh() const;
    matter::Float3 direct_sun_transmittance(float camera_world_y,
                                             const matter::Float3& to_sun) const;
    bool readback_transmittance_for_test(matter::VulkanDevice&,
                                         uint32_t x, uint32_t y,
                                         matter::Float3& out,
                                         std::string& error) const;
    uint64_t generation_serial() const;
    bool generated_this_frame() const;
    void destroy();
};
```

Allocate both a valid neutral emergency set and the physical LUT set at init. Atmosphere coefficient changes dirty all four outputs; sun direction or observer altitude movement above `10 m` dirties only sky view and irradiance. Commit requested settings only after all required dispatches/barriers record successfully; otherwise retain the last valid LUTs. The initial physical generation failure leaves the neutral emergency resources selected and reports the renderer error.

- [ ] **Step 5: Build shaders and run the GPU comparison**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: SPIR-V and embedded header regenerate; smoke case PASS; validation log contains no descriptor/layout errors.

- [ ] **Step 6: Commit generated shader artifacts with the sources**

```powershell
git add MatterEngine3/src/render/vk_atmosphere.h MatterEngine3/src/render/vk_atmosphere.cpp MatterEngine3/shaders_vk/atmosphere_common.glsl MatterEngine3/shaders_vk/atmosphere_transmittance.comp MatterEngine3/shaders_vk/atmosphere_multiscatter.comp MatterEngine3/shaders_vk/atmosphere_sky_view.comp MatterEngine3/shaders_vk/atmosphere_irradiance.comp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/Makefile MatterEditor/Makefile MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/shaders_gen/embedded_spirv.h
git commit -m "feat: generate physical atmosphere lookup tables"
```

---

### Task 7: Replace every procedural-sky and direct-sun consumer

**Files:**
- Create: `MatterEngine3/shaders_vk/environment_common.glsl`
- Modify: `MatterEngine3/shaders_vk/composite.frag`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Delete: `MatterEngine3/shaders_vk/sky_common.glsl`
- Modify: `MatterEngine3/shaders_vk/vol_scatter.comp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/vk_volumetrics.h`
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/tests/atmosphere_tests.cpp`
- Modify: `MatterEngine3/tests/shader_source_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/tools/atmosphere_cloud_shots.sh`
- Regenerate: `MatterEngine3/shaders_gen/embedded_spirv.h`

**Interfaces:**
- Consumes: `VkAtmosphere::sky_view`, `irradiance_sh`, and `direct_sun_transmittance` from Task 6.
- Produces: shared set-1 environment descriptors; `sample_physical_sky`, `sample_sky_irradiance`, and the one assembled direct-sun radiance used by raster, RT, and volumetrics.

- [ ] **Step 1: Add failing source and render tests**

Extend `shader_source_tests.cpp` to scan `shaders_vk` production entries and fail if `procedural_sky`, `sky_with_sun`, or `#include "sky_common.glsl"` remains. Add GPU cases that render sky pixels at sun elevations `90`, `5`, and `-5` and assert finite nonblack output, a warmer/dimmer direct-sun constant at `5`, zero direct sun at `-5`, and nonzero twilight sky at `-5`.

- [ ] **Step 2: Prove the new gates red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-shader-source CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
```

Expected: source gate FAIL while the procedural include/calls remain.

- [ ] **Step 3: Implement the shared environment descriptor set**

Create the set-1 layout exactly as specified under Stable Cross-Task Interfaces. Allocate one `EnvironmentBlock` UBO per frame slot; initially bind Task 6 sky/SH plus four 1x1x1 R16F clear cloud textures. Add this set layout to composite, RT lighting, and volumetric scatter pipeline layouts, and bind the current frame-slot set when recording each consumer.

`environment_common.glsl` must provide:

```glsl
vec2 atmosphere_sky_uv(vec3 world_dir, vec3 to_sun);
vec3 sample_physical_sky(vec3 world_dir, vec3 to_sun, vec3 sky_modifier);
vec3 sample_sky_irradiance(vec3 normal, vec3 sky_modifier);
float sample_cloud_transmittance(vec3 world_pos, float receiver_distance_m);
```

The cloud helper returns `1.0` in this task because the emergency textures are bound and `cloud_state.x == 0`.

- [ ] **Step 4: Assemble direct sunlight once on the CPU**

Before uploading `VkSceneLighting`, calculate:

```cpp
final_sun_rgb = atmosphere_direct_sun_rgb(
    atmosphere_settings, camera_y, to_sun,
    authored_world_sun_color, lighting_overrides.sun_tint,
    lighting_overrides.sun_multiplier);
```

Store the result as the final `sun_color`; set `sun_intensity` to `1.0` so downstream code cannot apply the multiplier twice. Keep authored `sky_color * sky_tint * sky_multiplier` as the componentwise modifier passed to LUT sampling. Atmosphere or sun-angle changes must invalidate RT/GI, atmosphere sky-view/SH, and volumetric lighting history.

- [ ] **Step 5: Replace shader consumers and remove the old include**

In `composite.frag`, sample the physical sky for empty G-buffer pixels, then add the existing analytic solar disc using the already attenuated final sun RGB and existing disc cosine thresholds. Replace raster ambient `sky_irradiance` with the nine-coefficient sampler.

In `rt_lighting.rgen`, use the same sky sampler for misses/reflections and the same SH sampler at secondary diffuse hits. In `vol_scatter.comp`, replace the fixed upward-gradient ambient with SH irradiance while leaving its current solid-geometry query and single-HG path intact. Delete `sky_common.glsl` and its Makefile dependencies.

```glsl
vec3 sky = sample_physical_sky(ray, to_sun, lighting.sky_color);
float disc = smoothstep(lighting.sun_disc_cos_edge,
                        lighting.sun_disc_cos_core, dot(ray, to_sun));
sky += disc * lighting.sun_color;
vec3 ambient = diffuse_albedo *
    sample_sky_irradiance(surface_normal, lighting.sky_color);
```

- [ ] **Step 6: Build and run atmosphere/source/GPU tests**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-shader-source CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: all PASS; repository production-shader search has no procedural-sky symbol.

- [ ] **Step 7: Run, inspect, and show the atmosphere visual milestone**

The `atmosphere` suite must use one process and issue sun elevations `90`, `45`, `5`, `0`, and `-5`, `get` each setting, settle, capture stats, and wait for every `.done` marker. It also applies Current cost and writes the fixed comparison name `physical-sky_current-cost.png` at the baseline camera/sun angle used later by Task 9.

```powershell
C:\msys64\usr\bin\bash.exe MatterEngine3/tools/atmosphere_cloud_shots.sh atmosphere physical-sky MatterEditor/build/validation/atmosphere-clouds/atmosphere
```

Inspect all five PNGs and logs. Show at least the `90`, `5`, and `-5` images in the implementation conversation with exact `render.lighting.sun_elevation_deg`, `render.atmosphere.*`, sun/sky modifier, and exposure values. Also show the Task 1 baseline beside the nearest matching physical-sky view. Reject the milestone if direct surface light, RT misses/reflections, and sky change incoherently or if the below-horizon frame still has a direct solar disc.

- [ ] **Step 8: Commit**

```powershell
git add MatterEngine3/shaders_vk/environment_common.glsl MatterEngine3/shaders_vk/composite.frag MatterEngine3/shaders_vk/rt_lighting.rgen MatterEngine3/shaders_vk/vol_scatter.comp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/vk_volumetrics.h MatterEngine3/src/render/vk_volumetrics.cpp MatterEngine3/src/matter_engine.cpp MatterEngine3/Makefile MatterEngine3/tests/atmosphere_tests.cpp MatterEngine3/tests/shader_source_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tools/atmosphere_cloud_shots.sh MatterEngine3/shaders_gen/embedded_spirv.h
git add -u MatterEngine3/shaders_vk/sky_common.glsl
git commit -m "feat: light the renderer from the physical atmosphere"
```

---

### Task 8: Make froxel dimensions live and resource replacement safe

**Files:**
- Modify: `MatterEngine3/src/render/vk_volumetrics.h`
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp`
- Modify: `MatterEngine3/shaders_vk/vol_common.glsl`
- Modify: `MatterEngine3/shaders_vk/vol_density.comp`
- Modify: `MatterEngine3/shaders_vk/vol_scatter.comp`
- Modify: `MatterEngine3/shaders_vk/vol_integrate.comp`
- Modify: `MatterEngine3/shaders_vk/composite.frag`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/vk_volumetrics.h`
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEditor/src/ui.h`
- Modify: `MatterEditor/src/main.cpp`
- Modify: `MatterEngine3/tests/volumetric_quality_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/tools/atmosphere_cloud_shots.sh`
- Regenerate: `MatterEngine3/shaders_gen/embedded_spirv.h`

**Interfaces:**
- Consumes: `resolve_froxel_grid`, `estimate_froxel_bytes`, and the registered discrete properties.
- Produces: `VkVolumetrics::FroxelBundle`, effective-grid/status statistics, a test-only next-allocation failure hook, and dimension-independent shader slice helpers.

- [ ] **Step 1: Extend failing CPU tests for slice mappings**

Move the exponential depth mapping to pure helpers and, for depth counts `64/96/128/192/256`, assert: first/last depths remain within the existing `3000 m` range, depth is strictly increasing, `depth_to_slice_n(slice_to_depth(i+0.5,D),D)` round-trips within `1e-5`, and integrated transmittance for nonnegative extinction never increases down a column.

- [ ] **Step 2: Add a failing Vulkan resize/lifetime test**

In one device/session, request this sequence across successive completed frame slots:

```text
1x/128 -> 0.5x/64 -> 2x/256 -> 1.5x/192 -> 1x/128
```

After each successful swap, assert image extents, descriptor image views, dispatch coverage, history invalidation, and finite output. Enable `set_fail_next_bundle_creation_for_test(true)`, request `2x/256`, and assert the old bundle remains active, effective properties report the prior dimensions, and rendering continues. The validation callback must record zero use-after-free, stale descriptor, or layout messages.

- [ ] **Step 3: Prove tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-volumetric-quality CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: FAIL on fixed `VOL_W/H/D` and missing bundle swap/status API.

- [ ] **Step 4: Replace fixed GLSL dimensions with image-derived dimensions**

Change `vol_common.glsl` to:

```glsl
const float VOL_FROXEL_FAR = 3000.0;
float slice_to_depth(float slice_index, float depth_slices);
float depth_to_slice_n(float depth, float depth_slices);
```

Each shader obtains its dimensions from `imageSize` or `textureSize` once per invocation, uses ceil-divided host dispatches, checks all three bounds, and passes `grid.z` into slice conversion. Composite uses `textureSize(vol_integrated_texture, 0).z`; no production shader retains `VOL_W`, `VOL_H`, or `VOL_D`.

- [ ] **Step 5: Refactor resources into a complete bundle**

Define:

```cpp
struct FroxelBundle {
    matter::FroxelGridDimensions dimensions{};
    bool enhanced_clouds = false;
    matter::VkImageResource media;
    matter::VkImageResource scatter[2];
    matter::VkImageResource integrated;
    matter::VkImageResource cloud_density; // empty until Task 9 requests enhanced
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet density_set = VK_NULL_HANDLE;
    VkDescriptorSet scatter_sets[2]{};
    VkDescriptorSet integrate_sets[2]{};
    uint32_t ping_index = 0;
};
```

`create_froxel_bundle` creates all images and dimension-dependent descriptors into a temporary bundle. `record` first collects bundles protected by the now-completed `frame_slot`, then creates a pending requested bundle. On success, move the old active bundle into the retirement list associated with the other in-flight slot, install the new bundle, clear history, and increment `resource_generation`. On failure, destroy only the temporary bundle, keep the active bundle, publish requested dimensions/bytes plus the error, and report the active enum values so the editor restores its property fields on the next frame.

- [ ] **Step 6: Publish effective state without a second setting store**

Append to `FrameStats`/`ViewerStats`: `vol_grid_w/h/d`, `vol_memory_bytes`, `vol_effective_xy_scale`, `vol_effective_depth_slices`, `vol_resource_generation`, `vol_allocation_rejected`, and `vol_allocation_error`. In `main.cpp`, only when a new rejected generation arrives, write the two live property fields back to the effective values and mark the binding clean for that rejected edit; keep the console error containing requested dimensions and MiB.

```cpp
if (fs.vol_allocation_rejected &&
    fs.vol_resource_generation != last_seen_vol_generation) {
    stats.volumetrics.froxel_xy_scale = fs.vol_effective_xy_scale;
    stats.volumetrics.froxel_depth_slices = fs.vol_effective_depth_slices;
    last_seen_vol_generation = fs.vol_resource_generation;
}
```

- [ ] **Step 7: Build and run resize tests green**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-volumetric-quality CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: all PASS and validation reports no lifetime/layout hazards across the full sequence and injected failure.

- [ ] **Step 8: Run, inspect, and show the froxel-resolution milestone**

The `froxel` suite must set/get all five XY scales and all five depth choices for smoke coverage, capture representative `0.5x/64`, `1x/128`, `1.5x/192`, and `2x/256` images, and emit stats/memory after each settled swap.

```powershell
C:\msys64\usr\bin\bash.exe MatterEngine3/tools/atmosphere_cloud_shots.sh froxel froxel-grid MatterEditor/build/validation/atmosphere-clouds/froxel
```

Inspect for black frames, stale history, dimension-dependent depth banding, and descriptor errors. Show the four representative PNGs in the conversation with exact dimensions, persistent MiB, and `gpu_volumetrics_ms`; explicitly state that all 25 discrete options completed even though only representative combinations are pictured.

- [ ] **Step 9: Commit**

```powershell
git add MatterEngine3/src/render/vk_volumetrics.h MatterEngine3/src/render/vk_volumetrics.cpp MatterEngine3/shaders_vk/vol_common.glsl MatterEngine3/shaders_vk/vol_density.comp MatterEngine3/shaders_vk/vol_scatter.comp MatterEngine3/shaders_vk/vol_integrate.comp MatterEngine3/shaders_vk/composite.frag MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEditor/src/ui.h MatterEditor/src/main.cpp MatterEngine3/tests/volumetric_quality_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tools/atmosphere_cloud_shots.sh MatterEngine3/shaders_gen/embedded_spirv.h
git commit -m "feat: resize volumetric froxels at runtime"
```

---

### Task 9: Share cloud density and add the enhanced cloud-only froxel channel

**Files:**
- Create: `MatterEngine3/shaders_vk/cloud_density.glsl`
- Modify: `MatterEngine3/shaders_vk/vol_common.glsl`
- Modify: `MatterEngine3/shaders_vk/vol_density.comp`
- Modify: `MatterEngine3/src/render/vk_volumetrics.h`
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEditor/src/editor_props.cpp`
- Modify: `MatterEngine3/tests/cloud_layer_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/tools/atmosphere_cloud_shots.sh`
- Regenerate: `MatterEngine3/shaders_gen/embedded_spirv.h`

**Interfaces:**
- Consumes: expanded 96-byte `GpuCloudLayer`, dynamic `FroxelBundle`, and `enhanced_cloud_lighting`.
- Produces: binding-free `CloudDensitySample evaluate_cloud_density(...)`, two density pipeline families selected by specialization, and optional full-resolution R16F cloud extinction.

- [ ] **Step 1: Add failing cloud-shape invariant tests**

Pin these CPU-side invariants and mirror them with fixed-position GPU samples: neutral new fields (`weather_influence=0`, `detail_erosion=0`, `shape_bias=0`) reproduce the pre-extension density within `1e-6`; coarse and full density are finite/nonnegative; full density remains zero outside the layer height range; coverage `0` is clear and `1` is maximally filled; detail erosion never creates density where coarse density is zero; identical layers with distinct derived seeds are decorrelated.

- [ ] **Step 2: Add failing Vulkan allocation/write tests**

Assert Current cost creates exactly four grid-sized RGBA16F volumes, leaves `FroxelBundle::cloud_density` empty, and binds only the `VkVolumetrics`-owned 1x1x1 R16F dummy. Switch to Improved, assert a `VK_FORMAT_R16_SFLOAT` image matching the active grid appears, clear ground fog and emitters, inject a constant cloud fixture, dispatch density, and read back: `vol_cloud_density == cloud extinction`, `vol_media.a == cloud extinction`, and `vol_media.rgb == vec3(0.99) * cloud extinction`. Switch back to Current cost and assert the grid-sized extra image retires safely.

- [ ] **Step 3: Prove cloud tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-layers CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: FAIL because density is still private to `vol_density.comp` and no cloud-only image exists.

- [ ] **Step 4: Implement one shared coarse/full density definition**

Move `cloud_height_profile` and current FBM/coverage arithmetic without reordering operations. Define:

```glsl
struct CloudDensitySample { float coarse; float full; };
CloudDensitySample evaluate_cloud_density(
    GpuCloudLayer layer, vec3 world_pos, float time_seconds);
```

Coarse density uses height, coverage, weather modulation, and at most the first two base FBM octaves. Compute weather as normalized two-octave FBM at `world_pos.xz * weather_scale`; shift coverage by `(weather-0.5)*2*weather_influence`, clamped to `[0,1]`. Full density evaluates the old all-authored-octave coverage shape first, applies `clamp(shape + shape_bias, 0, 1)`, then multiplies by `mix(1, smoothstep(0.2,0.8,detail01), detail_erosion)` where `detail01` is normalized three-octave FBM at `detail_scale`; finally multiply by height profile and `max_density`. This multiplicative erosion can remove density but cannot create it outside the base body. Weather influence zero, erosion zero, and bias zero must preserve the old full-density FBM arithmetic exactly; add a comment naming the CPU parity test.

- [ ] **Step 5: Add current/enhanced density pipelines and medium separation**

Add specialization constant `ENHANCED_CLOUDS` at constant id `1`. Use one superset density descriptor layout with binding `4` as `r16f writeonly image3D vol_cloud_density`: Current cost binds a `VkVolumetrics`-owned 1x1x1 R16F dummy created with both storage and sampled usage and the specialized-false pipeline performs no cloud-density write; enhanced bundles bind their grid-sized R16F image, clear it for invalid/near slices, sum full cloud extinction across layers, add near-white cloud scattering (`0.99 * cloud_extinction`) to the existing fog/emitter scattering, and write combined extinction to `vol_media.a`.

```glsl
layout(constant_id = 1) const bool ENHANCED_CLOUDS = false;
layout(r16f, set = 0, binding = 4) uniform writeonly image3D vol_cloud_density;
if (ENHANCED_CLOUDS)
    imageStore(vol_cloud_density, voxel, vec4(cloud_extinction, 0, 0, 0));
vec3 scattering = fog_emitter_scattering + vec3(0.99) * cloud_extinction;
imageStore(vol_media, voxel, vec4(scattering, total_extinction));
```

Build all `(cloud_count 0..4) x (enhanced false/true)` density pipeline combinations up front. Current cost allocates no grid-sized cloud image and the compiler eliminates the specialized-false write branch; the 1x1x1 dummy only satisfies the stable Vulkan layout and is excluded from froxel memory accounting.

- [ ] **Step 6: Run parity, allocation, and validation gates**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-layers CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: all PASS; Current cost reports exactly `58982400` base froxel bytes at `1x/128`; Improved adds `3686400` bytes before cloud-shadow resources; no validation errors on preset toggles.

- [ ] **Step 7: Capture and show density-parity evidence**

Add a `Cloud density` label to the existing `viewer.debug.volumetric_view` enum (continue to mirror that session-scoped debug value into `vol_debug_view`; do not register a duplicate persisted field under `render.volumetrics`). Current cost shows black for that channel because it has no allocation. Use the CLI to capture Current cost before/after the refactor under neutral shape fields and compare against Task 7's physical-sky Current cost frame:

```powershell
C:\msys64\usr\bin\bash.exe MatterEngine3/tools/atmosphere_cloud_shots.sh cloud-lighting shared-density MatterEditor/build/validation/atmosphere-clouds/cloud-density
C:\msys64\usr\bin\python3.exe MatterEngine3/tools/img_diff.py MatterEditor/build/validation/atmosphere-clouds/atmosphere/physical-sky_current-cost.png MatterEditor/build/validation/atmosphere-clouds/cloud-density/shared-density_current-cost.png
```

Expected: pixel-identical or only the documented atmosphere temporal settling epsilon; investigate any cloud-shape movement. Show the current-cost comparison and enhanced cloud-density debug image in the implementation conversation with all neutral/new field values.

- [ ] **Step 8: Commit**

```powershell
git add MatterEngine3/shaders_vk/cloud_density.glsl MatterEngine3/shaders_vk/vol_common.glsl MatterEngine3/shaders_vk/vol_density.comp MatterEngine3/src/render/vk_volumetrics.h MatterEngine3/src/render/vk_volumetrics.cpp MatterEngine3/Makefile MatterEditor/src/editor_props.cpp MatterEngine3/tests/cloud_layer_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tools/atmosphere_cloud_shots.sh MatterEngine3/shaders_gen/embedded_spirv.h
git commit -m "feat: separate full-resolution cloud extinction"
```

---

### Task 10: Create sun-space cloud-shadow resources and coordinate contracts

**Files:**
- Create: `MatterEngine3/src/render/vk_cloud_shadows.h`
- Create: `MatterEngine3/src/render/vk_cloud_shadows.cpp`
- Create: `MatterEngine3/shaders_vk/cloud_shadow_common.glsl`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/shaders_vk/environment_common.glsl`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEditor/Makefile`
- Modify: `MatterEngine3/tests/cloud_shadow_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: `CloudShadowSettings`, `CloudShadowLevelDesc`, Task 3 sun-frame math, the renderer set-1 descriptor contract, and frame-slot retirement discipline from Task 8.
- Produces: `viewer::VkCloudShadows`, two level bundles with R16F density plus cumulative ping-pong images, stable transform UBO data, active image indices, and clear emergency textures.

- [ ] **Step 1: Add failing coordinate/filter/edge tests**

For near/far frames, test world -> UVW -> world round-trip error below `1e-3 m`, camera-lateral translation below one voxel leaves the snapped transform unchanged, crossing one voxel moves it exactly one texel, and the two lateral basis vectors remain stable when the sun is near world up. Pin edge fade to clear over the outer `8%` of lateral UV and assert filtered transmittance stays `[0,1]` for clear, opaque, non-finite, and boundary samples.

- [ ] **Step 2: Add a failing Vulkan resource/fallback test**

Initialize Improved and assert each level owns one density image plus two cumulative images, all `VK_FORMAT_R16_SFLOAT` with the exact resolved extents. Disable shadows and assert set-1 still binds valid 1x1x1 clear images and `cloud_state.x == 0`. Inject `set_fail_next_bundle_creation_for_test(true)`, request High, and assert sampling switches to clear transmittance, the failure string names both requested levels/MiB, and the renderer remains usable.

- [ ] **Step 3: Prove tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-shadows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: FAIL because the module/resources do not exist.

- [ ] **Step 4: Implement level bundles and safe recreation**

Use:

```cpp
struct CloudShadowLevelBundle {
    matter::CloudShadowLevelDesc desc{};
    matter::VkImageResource density;
    matter::VkImageResource cumulative[2];
    uint32_t active_index = 0;
    matter::CloudShadowFrame current_frame{};
    matter::CloudShadowFrame previous_frame{};
    bool history_valid = false;
};

class VkCloudShadows {
public:
    bool init(matter::VulkanDevice&, std::string& error);
    void request_settings(const matter::CloudShadowSettings&);
    bool prepare_frame(uint32_t frame_slot, const matter::Float3& camera,
                       const matter::Float3& sun_direction,
                       float sun_angular_diameter_deg, std::string& error);
    bool record(VkCommandBuffer, float frame_time, std::string& error);
    const CloudShadowLevelBundle& level(uint32_t index) const;
    uint64_t persistent_bytes() const;
    bool active() const;
    void destroy();
};
```

Creation uses a temporary pair of bundles. On success, rewrite set-1 bindings 2-5 to the new four cumulative image views at a frame boundary and retire the old pair after its protecting frame-slot fence. On creation failure, keep the failed resources out of the descriptor set, bind clear emergency textures, set `active=false`, and publish the diagnostic. Dimension/coverage changes invalidate both levels; enable/disable never destroys the emergency resources.

- [ ] **Step 5: Implement sampling math but no density generation yet**

`cloud_shadow_common.glsl` selects cumulative ping-pong images from `cloud_state.yz`, transforms world positions with `cloud_world_to_uvw[level]`, fades optical depth to zero in the outer `8%`, blends near into far across the near guard band, clamps finite optical depth to `[0,80]`, and returns `exp(-tau)`. Filter radius is `sun_angular_radius * receiver_distance / voxel_size * filter_scale`, clamped to `0..4` texels; use a fixed 5-tap cross so cost is bounded. All textures are clear in this task, so every sampling consumer remains visually unchanged.

```glsl
float cloud_shadow_transmittance_level(int level, vec3 world_pos,
                                       float receiver_distance_m) {
    vec3 uvw = (environment.cloud_world_to_uvw[level] *
                vec4(world_pos, 1.0)).xyz;
    if (any(lessThan(uvw, vec3(0))) || any(greaterThan(uvw, vec3(1))))
        return 1.0;
    float tau = filtered_cumulative_tau(level, uvw, receiver_distance_m);
    if (isnan(tau) || isinf(tau)) return 1.0;
    float edge_distance = min(min(uvw.x, uvw.y),
                              min(1.0-uvw.x, 1.0-uvw.y));
    float edge = smoothstep(0.0, 0.08, edge_distance);
    return exp(-clamp(tau * edge, 0.0, 80.0));
}
```

- [ ] **Step 6: Run tests and validation green**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-shadows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: PASS with clear sampling and no layout/lifetime errors.

- [ ] **Step 7: Commit**

```powershell
git add MatterEngine3/src/render/vk_cloud_shadows.h MatterEngine3/src/render/vk_cloud_shadows.cpp MatterEngine3/shaders_vk/cloud_shadow_common.glsl MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/shaders_vk/environment_common.glsl MatterEngine3/Makefile MatterEditor/Makefile MatterEngine3/tests/cloud_shadow_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat: allocate sun-space cloud shadow clipmaps"
```

---

### Task 11: Generate, prefix-integrate, and reproject cloud optical depth

**Files:**
- Create: `MatterEngine3/shaders_vk/cloud_shadow_reproject.comp`
- Create: `MatterEngine3/shaders_vk/cloud_shadow_density.comp`
- Create: `MatterEngine3/shaders_vk/cloud_shadow_integrate.comp`
- Modify: `MatterEngine3/src/render/vk_cloud_shadows.h`
- Modify: `MatterEngine3/src/render/vk_cloud_shadows.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/tests/cloud_shadow_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/tools/atmosphere_cloud_shots.sh`
- Regenerate: `MatterEngine3/shaders_gen/embedded_spirv.h`

**Interfaces:**
- Consumes: shared `evaluate_cloud_density(...).coarse`, Task 10 level resources/transforms, cloud-layer SSBO, and `update_fraction`.
- Produces: valid cumulative sun-to-point optical depth, temporal reprojection, rotating tile refresh, below-horizon pause, and debug capture of near/far fields.

- [ ] **Step 1: Add failing analytical slab tests**

For a constant-density `sigma=0.02 m^-1` slab and voxel length `10 m`, read every Z slice and assert cumulative optical depth is monotonic and equals `(z+1)*0.2` within half-float tolerance. Assert transmittance equals `exp(-tau)` within `0.01`, is always `[0,1]`, and replacing one density sample with NaN yields clear at that sample rather than blacking out later slices. Add two non-overlapping layers and assert the lower receiver includes both optical depths while a point between layers includes only the sunward layer.

- [ ] **Step 2: Add failing reprojection/update tests**

Render a known checker density, move the camera by one snapped voxel, and assert overlapping world positions preserve optical depth while only the new border is invalid/scheduled. For `update_fraction=0.25`, assert every tile is refreshed within four frames and newly exposed tiles refresh immediately. Advance cloud time with a fixed camera and assert one rotating quarter changes each frame. Change cloud authoring or sun direction by `2.1` degrees and assert both histories invalidate; set direct sun below the geometric horizon and assert generation dispatch counts are zero while the active field samples clear.

- [ ] **Step 3: Prove GPU tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: FAIL because all cumulative images are still clear.

- [ ] **Step 4: Implement reprojection and deterministic update selection**

`cloud_shadow_reproject.comp` maps each current voxel center to world and then previous UVW; in-bounds samples copy previous cumulative optical depth, out-of-bounds samples write zero. `cloud_shadow_density.comp` updates a column when previous UVW is out of bounds or:

```glsl
uint phase_count = uint(round(1.0 / clamp(update_fraction, 0.0625, 1.0)));
bool rotating = hash(tile_xy) % phase_count == frame_index % phase_count;
```

The final selector is `!history_valid || previous_out_of_bounds || rotating`. Use 8x8 XY tiles. For selected columns, loop Z and write the sum of `evaluate_cloud_density(layer,...).coarse` for all active layers; never inject ground fog or emitters.

- [ ] **Step 5: Implement sunward prefix integration and barriers**

Use one invocation per selected XY column in `cloud_shadow_integrate.comp`, loop from the sunward Z boundary to the receiver side, accumulate `tau += max(density,0) * voxel_depth_m`, clamp finite tau to `80`, and write the inactive cumulative image. Non-selected columns retain the reprojected result. Insert explicit compute-write -> compute-read barriers between reprojection, density, and prefix; insert compute-write -> fragment/ray/compute sampled-read before consumers. Flip the active index only after the level's dispatches are recorded, write those known destination indices into the current frame-slot `EnvironmentBlock`, then flush that UBO before queue submission.

```glsl
float tau = 0.0;
for (int z = 0; z < grid_depth; ++z) {
    float sigma = imageLoad(cloud_density, ivec3(column, z)).r;
    if (isnan(sigma) || isinf(sigma)) sigma = 0.0;
    tau = clamp(tau + max(sigma, 0.0) * pc.voxel_depth_m, 0.0, 80.0);
    imageStore(cumulative_out, ivec3(column, z), vec4(tau, 0, 0, 0));
}
```

```cpp
barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
```

- [ ] **Step 6: Run numerical/reprojection/validation gates**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-shadows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: PASS with zero validation errors.

- [ ] **Step 7: Run and show the clipmap stability milestone**

Add a debug mode that shows filtered cloud transmittance on the ground in grayscale without changing the stored field. The `cloud-shadows` suite must capture: centered near field, a camera translation across near/far overlap, a near outer boundary, and four moving-cloud frames at fixed camera/sun.

```powershell
C:\msys64\usr\bin\bash.exe MatterEngine3/tools/atmosphere_cloud_shots.sh cloud-shadows optical-depth MatterEditor/build/validation/atmosphere-clouds/cloud-shadows
```

Inspect for hard square edges, near/far seams, stale strips, and temporal flashes. Use `img_diff.py` on adjacent translated/moving frames and record mean/max differences with the command logs. Show at least the centered, boundary, and one moving pair in the implementation conversation with near/far sizes, coverage, filter scale, and update fraction.

- [ ] **Step 8: Commit**

```powershell
git add MatterEngine3/shaders_vk/cloud_shadow_reproject.comp MatterEngine3/shaders_vk/cloud_shadow_density.comp MatterEngine3/shaders_vk/cloud_shadow_integrate.comp MatterEngine3/src/render/vk_cloud_shadows.h MatterEngine3/src/render/vk_cloud_shadows.cpp MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/Makefile MatterEngine3/tests/cloud_shadow_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tools/atmosphere_cloud_shots.sh MatterEngine3/shaders_gen/embedded_spirv.h
git commit -m "feat: generate temporal cloud optical depth"
```

---

### Task 12: Add detailed cloud self-shadowing and selectable scattering orders

**Files:**
- Modify: `MatterEngine3/shaders_vk/vol_scatter.comp`
- Modify: `MatterEngine3/shaders_vk/vol_common.glsl`
- Modify: `MatterEngine3/src/render/vk_volumetrics.h`
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/tests/cloud_shadow_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/tools/atmosphere_cloud_shots.sh`
- Regenerate: `MatterEngine3/shaders_gen/embedded_spirv.h`

**Interfaces:**
- Consumes: full-resolution `vol_cloud_density`, cumulative coarse optical depth, physical direct sun/SH, and registered march/order/strength/powder properties.
- Produces: bounded local full-density self-shadow, coarse remaining-path sampling without double counting, cloud-only dual-lobe phase, and 1-4 scattering-order accumulation; Current cost retains its existing scatter pipeline.

- [ ] **Step 1: Add failing constant-slab and no-double-count tests**

For a constant cloud slab, compare the shader's local march to `exp(-sigma * marched_distance)` within one half-float/step tolerance. Construct coarse cumulative tau where `tau(start)=4`, `tau(end)=2`, and detailed local tau is `2.5`; assert total tau is `4.5`, not `6.5`. Put the endpoint outside the camera froxel frustum and assert the local loop stops cleanly while the sun-space remainder still supplies broad shadowing.

- [ ] **Step 2: Add failing scattering-order/medium-separation tests**

With fixed direct/ambient inputs, assert orders `2`, `3`, and `4` monotonically brighten an optically thick shadowed cloud relative to order `1`; the normalized added energy stays below `1 + 2*multiple_scattering_strength`; order `1` is independent of the strength knob; changing orders does not change a ground-fog-only fixture; and `FogLab` with no cloud layers has a zero cloud-only channel while retaining nonzero low-lying haze.

- [ ] **Step 3: Prove tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-shadows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: FAIL because enhanced scatter does not exist.

- [ ] **Step 4: Add robust world-space froxel sampling for sun rays**

Keep push constants within the device's 256-byte limit by extending the current 208-byte scatter block with exactly 48 bytes:

```cpp
float camera_fwd[3];   float tan_half_fov;
float camera_right[3]; float aspect_ratio;
float camera_up[3];    float local_march_distance_m;
```

Derive current view depth and NDC from camera basis rather than adding another mat4. `world_to_froxel_uvw` rejects points behind the camera, outside UV `[0,1]`, or outside near/3000 m; otherwise it maps depth through the active texture's Z count. This is the explicit world-space sampling path for sun rays: detailed sampling works wherever the ray remains inside the camera-aligned grid, and the sun-space clipmap supplies the remainder and every out-of-frustum segment.

- [ ] **Step 5: Implement detailed local optical depth without overlap**

For a cloud froxel, jitter `steps` midpoint samples along `min(local_march_distance_m, distance_to_volume_exit)` toward the sun, sample R16F cloud extinction through `world_to_froxel_uvw`, and accumulate detailed tau. Let `end_pos` be the last covered point; sample cumulative coarse tau at `end_pos`, then:

```glsl
float tau_total = clamp(tau_local_full + tau_remaining_coarse, 0.0, 80.0);
float cloud_sun_transmittance = exp(-tau_total);
```

Do not sample coarse tau at the starting point and multiply it by the local result. Reuse the existing 2x2 temporal/Bayer schedule and seed jitter by froxel/frame; local-march setting or cloud-shape changes invalidate scatter history.

- [ ] **Step 6: Separate fog and cloud lighting and add bounded orders**

Recover cloud scattering as `vec3(0.99) * cloud_extinction` and ground-fog/emitter scattering as `max(vol_media.rgb - cloud_scattering, 0)`. Keep the latter on the existing `phase_g`, one TLAS visibility query, and one SH term. Use a cloud dual lobe:

```glsl
float cloud_phase(float mu, float anisotropy_scale) {
    return 0.8 * hg_phase(mu, 0.85 * anisotropy_scale) +
           0.2 * hg_phase(mu, -0.30 * anisotropy_scale);
}
```

For `order=0..orders-1`, use `extinction_scale=pow(0.55,order)`, `anisotropy_scale=pow(0.5,order)`, and energy `1` for order zero or `multiple_scattering_strength*pow(0.5,order-1)` for later orders. Multiply each by `exp(-tau_total*extinction_scale)` and its phase; clamp the accumulated normalized order energy to `1 + 2*strength`. Apply powder as a final bounded factor `mix(1, min(2, 1 + (1-exp(-2*tau_local_full))), powder_strength)`.

Compile two scatter pipelines against one superset layout: Current cost binds the 1x1x1 cloud-density dummy and specializes out every read, retaining the previous single-HG path; enhanced binds the grid-sized cloud density and samples set-1 cloud optical depth. Switching pipelines does not recreate resources unless `enhanced_cloud_lighting` changes the bundle footprint.

- [ ] **Step 7: Build and run numerical/GPU gates green**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-shadows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: PASS; no non-finite scatter output; Current cost validation and memory remain unchanged.

- [ ] **Step 8: Run, inspect, and show self-shadow/multiple-scattering progress**

The `cloud-lighting` suite must hold camera, cloud fields, sun elevation, and exposure fixed while capturing orders `1`, `2`, `3`, and `4`; also capture local march `0` versus `8` and a two-layer cross-shadow view.

```powershell
C:\msys64\usr\bin\bash.exe MatterEngine3/tools/atmosphere_cloud_shots.sh cloud-lighting lit-clouds MatterEditor/build/validation/atmosphere-clouds/cloud-lighting
```

Inspect for bright sun-facing rims, dark optical-depth cores, coherent self-shadow, one layer shadowing the other, and bounded rather than washed-out order-4 energy. Show side-by-side order `1/2/4` and march `0/8` screenshots in the implementation conversation. Caption exact march steps/distance, order, strength, powder, sun elevation, froxel dimensions, and clipmap settings.

- [ ] **Step 9: Commit**

```powershell
git add MatterEngine3/shaders_vk/vol_scatter.comp MatterEngine3/shaders_vk/vol_common.glsl MatterEngine3/src/render/vk_volumetrics.h MatterEngine3/src/render/vk_volumetrics.cpp MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/tests/cloud_shadow_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tools/atmosphere_cloud_shots.sh MatterEngine3/shaders_gen/embedded_spirv.h
git commit -m "feat: self-shadow clouds with scalable scattering"
```

---

### Task 13: Apply cloud shadows to surfaces, RT hits, and low fog

**Files:**
- Modify: `MatterEngine3/shaders_vk/environment_common.glsl`
- Modify: `MatterEngine3/shaders_vk/composite.frag`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/shaders_vk/vol_scatter.comp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/tools/atmosphere_cloud_shots.sh`
- Regenerate: `MatterEngine3/shaders_gen/embedded_spirv.h`

**Interfaces:**
- Consumes: `sample_cloud_transmittance`, final physical sun RGB, and completed cumulative clipmaps.
- Produces: filtered cloud attenuation of primary raster direct light, RT secondary-hit direct light, and direct sun in ground fog, while leaving sky ambient and indirect illumination unshadowed.

- [ ] **Step 1: Add failing receiver tests**

Create a deterministic overhead constant cloud slab and receivers at ground, inside low fog, on an object, on vegetation, and at an RT secondary hit. Assert direct light below the slab is lower than an identical clear control and ambient SH is unchanged. Disable `render.cloud_shadows` and assert all receivers return to the clear control. Move each receiver above the slab and assert transmittance approaches one. Add a high-cloud/ground case where increased sun diameter/filter scale increases penumbra width without changing mean optical depth materially.

- [ ] **Step 2: Prove receiver tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: FAIL because surface/RT direct sun ignores cloud optical depth.

- [ ] **Step 3: Reconstruct primary world position and attenuate raster direct light**

Extend `VkSceneLighting`/composite push constants from 120 to the guaranteed 128-byte limit by appending `camera_pos_x` and `camera_pos_z`; reuse existing `camera_y`. The reversed-Z depth reconstruction yields view-axis depth rather than distance along an off-axis normalized ray, so reconstruct with `ray_t = linear_depth / max(dot(view_ray, camera_fwd), 1e-4)` and `world_pos = camera_pos + view_ray * ray_t`. Estimate receiver-to-cloud distance from the active cloud top and sun direction, and multiply only the direct term:

```glsl
direct *= sample_cloud_transmittance(world_pos, receiver_distance_m);
```

This operates after the existing TLAS visibility texture, so solid geometry visibility and cloud transmittance remain separate factors. It covers terrain, objects, impostors, and vegetation without another ray.

- [ ] **Step 4: Attenuate RT secondary direct light**

At each secondary surface hit in `rt_lighting.rgen`, sample the same set-1 helper at the hit world position and multiply the direct-sun estimator before BRDF weighting. Miss/environment and SH ambient remain unattenuated. Primary raster and secondary RT paths must use the same active clipmap indices and `EnvironmentBlock` for the frame.

```glsl
float cloud_visibility = sample_cloud_transmittance(
    hit_world_pos, receiver_to_cloud_distance_m);
vec3 direct_radiance = constants.sun_color * geometry_visibility *
                       cloud_visibility;
```

- [ ] **Step 5: Attenuate direct sun in ground fog**

In enhanced `vol_scatter.comp`, multiply the ground-fog/emitter sun contribution by the broad clipmap transmittance at each froxel. Keep its TLAS visibility query. Do not feed fog extinction into clipmap generation, do not apply cloud multiple-scattering orders to fog, and do not attenuate fog's SH ambient term. Current cost remains the established no-cloud-shadow scatter path.

```glsl
vec3 fog_sun = fog_emitter_scattering * fog_phase * pc.sun_color *
               geometry_visibility *
               sample_cloud_transmittance(world_pos,
                                           receiver_to_cloud_distance_m);
vec3 fog_ambient = fog_emitter_scattering *
                   sample_sky_irradiance(vec3(0,1,0), pc.sky_color);
```

- [ ] **Step 6: Build and run all receiver/validation tests**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: PASS, including raster/RT agreement and FogLab isolation; no set-layout/push-constant validation errors.

- [ ] **Step 7: Run, inspect, and show the world-shadow milestone**

Extend the `cloud-shadows` suite with normal lit views (debug off) of ground, object/vegetation, cross-layer, and low fog, each with shadows disabled/enabled under otherwise identical settings.

```powershell
C:\msys64\usr\bin\bash.exe MatterEngine3/tools/atmosphere_cloud_shots.sh cloud-shadows receivers MatterEditor/build/validation/atmosphere-clouds/receivers
```

Inspect every enabled/disabled pair and their `img_diff.py` result. Show ground, object/vegetation, and low-fog pairs in the implementation conversation with exact cloud layer heights, sun elevation/diameter, clipmap coverage/filter/update, and fog density/floor/falloff. Explicitly call out cloud self-shadow versus cast ground shadow so the evidence proves both capabilities.

- [ ] **Step 8: Commit**

```powershell
git add MatterEngine3/shaders_vk/environment_common.glsl MatterEngine3/shaders_vk/composite.frag MatterEngine3/shaders_vk/rt_lighting.rgen MatterEngine3/shaders_vk/vol_scatter.comp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/tools/atmosphere_cloud_shots.sh MatterEngine3/shaders_gen/embedded_spirv.h
git commit -m "feat: cast cloud shadows across world lighting"
```

---

### Task 14: Expose pass timings and complete automated acceptance

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/vk_volumetrics.h`
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEditor/src/ui.h`
- Modify: `MatterEditor/src/ui.cpp`
- Modify: `MatterEditor/src/property_editor.cpp`
- Modify: `MatterEditor/src/main.cpp`
- Modify: `MatterEditor/src/viewer_commands.h`
- Modify: `MatterEditor/src/issue_reporter.cpp`
- Modify: `MatterEngine3/tools/atmosphere_cloud_shots.sh`
- Modify: `MatterEngine3/docs/rendering.md`
- Modify: `MatterEngine3/tests/property_editor_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: all completed renderer modules, quality properties, effective memory/status fields, and the CLI suites.
- Produces: separate GPU zones/stat fields, final Lighting/Performance readouts, backward-compatible appended CLI stats, full visual/performance matrix, and completion evidence.

- [ ] **Step 1: Add failing stats/UI contract tests**

Keep existing GPU zone indices `0..11` stable. Append:

```cpp
static constexpr uint32_t kGpuZoneAtmosphere    = 12;
static constexpr uint32_t kGpuZoneCloudShadows = 13;
static constexpr uint32_t kGpuZoneVolDensity   = 14;
static constexpr uint32_t kGpuZoneVolScatter   = 15;
static constexpr uint32_t kGpuZoneVolIntegrate = 16;
static constexpr uint32_t kGpuZoneCount         = 17;
```

Add tests that the five new values appear in `FrameStats`, `ViewerStats`, Lighting/Performance text, issue-report JSON, and appended `STATS` fields without reordering the existing CSV prefix. Assert combined volumetric time remains the established `gpu_vol_ms`, and detail fields sum within timestamp tolerance.

- [ ] **Step 2: Prove stats tests red**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-property-editor CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: FAIL on missing zones/stat fields.

- [ ] **Step 3: Instrument exact command-buffer regions**

Timestamp only actual work: atmosphere covers LUT dispatches and reads zero on steady frames; cloud shadows covers reproject+density+prefix; the three froxel zones wrap their individual passes; existing `kGpuZoneVolumetrics` continues to wrap the combined froxel sequence. Append `gpu_atmosphere_ms`, `gpu_cloud_shadows_ms`, `gpu_vol_density_ms`, `gpu_vol_scatter_ms`, and `gpu_vol_integrate_ms` to frame/editor stats. Preserve `VkVolumetrics::record` as the orchestration entry point and add an optional typed boundary callback so that module does not depend on renderer zone IDs:

```cpp
enum class VolumetricPass : uint8_t { Density, Scatter, Integrate };
using VolumetricPassBoundary =
    std::function<void(VolumetricPass pass, bool is_end)>;

auto boundary = [&](VolumetricPass pass, bool is_end) {
    const uint32_t zone[] = {kGpuZoneVolDensity, kGpuZoneVolScatter,
                             kGpuZoneVolIntegrate};
    write_gpu_timestamp(cmd, zone[static_cast<uint32_t>(pass)],
                        is_end, frame_slot);
};
volumetrics_->record(cmd, frame_slot, depth, tlas, matrices, frame_time,
                     boundary, error);
```

- [ ] **Step 4: Finish UI and CLI reporting**

In Lighting show requested/effective dimensions, froxel persistent MiB, cloud-shadow persistent MiB, each new GPU time, and combined atmosphere+cloud-shadow+froxel time. In Performance show the same timing lanes without property controls. Append to `STATS` after every existing column:

```text
gpu_atmosphere_ms,gpu_cloud_shadows_ms,gpu_vol_density_ms,
gpu_vol_scatter_ms,gpu_vol_integrate_ms,vol_grid_w,vol_grid_h,vol_grid_d,
vol_memory_bytes,cloud_shadow_memory_bytes,vol_resource_generation
```

Update `viewer_commands.h` and `rendering.md` with field order and the exact `set/get/cam/stats/shot/quit` examples from the approved design.

- [ ] **Step 5: Run the complete sequential headless/GPU suite**

```powershell
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-volumetric-quality CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-shadows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-cloud-layers CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-world-definition CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-props CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-property-editor CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-sun-angles CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-shader-source CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-vk-scene-renderer CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
& MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: every target PASS sequentially; shader header is current; Vulkan validation has zero errors/warnings caused by this feature.

- [ ] **Step 6: Run the final one-process visual matrix**

The `final` suite must cover sun elevations `90/45/5/0/-5`, all four presets, one custom configuration, all XY/depth options as stability-only cases, cloud self/cross/ground/fog shadows, near/far boundaries, and at least four moving-cloud frames. It must `get` every changed property, settle after resource changes, capture stats before each shot, wait for `.done`, and exit cleanly.

```powershell
C:\msys64\usr\bin\bash.exe MatterEngine3/tools/atmosphere_cloud_shots.sh final acceptance MatterEditor/build/validation/atmosphere-clouds/final
```

Expected: no missing `.done`, no process left running, no renderer errors, and every requested/effective setting agrees.

- [ ] **Step 7: Evaluate visual and performance acceptance**

Inspect every PNG, `get` response, stats row, and log. Run deterministic diffs for enabled/disabled and order comparisons. Required recorded conclusions:

- Current cost at `1x/128` has the same four-volume memory and is within `max(5%, 0.1 ms)` of Task 1's volumetric baseline after excluding atmosphere dirty frames.
- Atmosphere steady-state reports approximately zero LUT-generation time; dirty frames report the work separately.
- Improved combined atmosphere-steady/cloud-shadow/froxel target is `<= 2 ms` on an RTX 3070-class GPU at 1440p/DLSS Quality; if the test machine differs, report hardware/resolution honestly and retain raw stats without claiming that hardware-specific gate.
- High and Ultra complete without instability; their memory and timing are reported without a cap.
- Ground fog remains visibly low-lying and lit at all relevant sun angles.
- Edge lighting, dark cores, self-shadow, cross-layer shadow, surface/vegetation shadow, and fog shadow are each visible in at least one inspected image.
- Near/far translation has no hard square seam and moving-cloud refresh has no stale full-frame flash.

- [ ] **Step 8: Show final screenshot evidence in the implementation conversation**

Embed representative absolute-path images for: atmosphere noon/sunset/twilight, Current cost/Improved/High/Ultra, orders `1/2/4`, self/cross-layer shadows, ground/object/fog shadows, and near/far translation. Each caption must state exact property configuration and relevant GPU/memory stats. Do not report the milestone complete merely because files exist.

- [ ] **Step 9: Commit the final instrumentation/docs**

```powershell
git add MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/vk_volumetrics.h MatterEngine3/src/render/vk_volumetrics.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEditor/src/ui.h MatterEditor/src/ui.cpp MatterEditor/src/property_editor.cpp MatterEditor/src/main.cpp MatterEditor/src/viewer_commands.h MatterEditor/src/issue_reporter.cpp MatterEngine3/tools/atmosphere_cloud_shots.sh MatterEngine3/docs/rendering.md MatterEngine3/tests/property_editor_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "test: validate atmosphere and volumetric cloud quality"
```

---

## Final Acceptance Checklist

- [ ] `rg -n "procedural_sky|sky_with_sun|sky_common" MatterEngine3/shaders_vk MatterEngine3/src/render` returns no production use.
- [ ] Direct sun, sky, RT misses/reflections, fog, and cloud lighting respond coherently to solar elevation.
- [ ] Low exponential haze remains independently authorable and visible.
- [ ] Current cost uses four grid-sized RGBA16F froxel images only; enhanced mode adds one grid-sized R16F cloud image (the shared 1x1x1 descriptor dummy is not a froxel allocation).
- [ ] Every froxel option applies live or restores the prior valid property without losing the renderer.
- [ ] Clouds self-shadow, shadow other layers, and expose bounded orders 1-4.
- [ ] Clouds cast filtered shadows on fog, terrain, vegetation, objects, and RT secondary hits.
- [ ] All new fields persist through registered World-scoped properties and FIFO `set/get`.
- [ ] Atmosphere, cloud-shadow, and three froxel timing lanes plus memory are visible in UI and CLI stats.
- [ ] CLI suites always wait for readiness and `.done`, inspect logs/images, send `quit`, and leave no editor process.
- [ ] Representative milestone and final screenshots were embedded in the implementation conversation with exact configurations.
- [ ] All named C++ tests, SPIR-V build, editor build, and Vulkan validation smoke test pass sequentially.
