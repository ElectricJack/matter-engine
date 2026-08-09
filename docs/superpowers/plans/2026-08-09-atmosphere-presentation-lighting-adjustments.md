# Atmosphere Presentation and Lighting Adjustments Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove visible sky-view banding, add deterministic half-LSB scene dither, and separate visible-sky, physical irradiance, direct-world, and analytic-disc lighting through one atomic resolved atmosphere transaction with reproducible raster/native-RT/fog acceptance evidence.

**Architecture:** Keep the `192x108` physical sky-view LUT and fix its presentation at the environment descriptor with one dedicated periodic linear sampler plus a shared centred-bin UV helper. Resolve all atmosphere and lighting policy on the CPU into one committed snapshot whose four RGB lanes are uploaded through the shared environment UBO; candidate LUT images, direction, nine SH coefficients, atmospheric direct base, and descriptors commit together or the renderer replays current non-atmosphere controls against the complete last-valid atmosphere state. The display pass adds the exact static 8x8 achromatic pattern after ACES and explicit sRGB encoding, while the editor exposes only requested controls and a separate read-only resolved-status surface for deterministic FIFO capture.

**Tech Stack:** C++17, Vulkan 1.3, GLSL 460 compute/fragment/ray-tracing shaders, SPIR-V embedding through the existing Makefiles, Matter property registry/ImGui/FIFO commands, QuickJS world fixtures, Bash/Python image metrics, and the existing CPU and Vulkan smoke harnesses.

## Global Constraints

- The approved design at `docs/superpowers/specs/2026-08-09-atmosphere-presentation-lighting-adjustments-design.md` is authoritative.
- Execute this plan after the physical-atmosphere plan's completed Task 11 and before its existing Task 12. Do not edit, renumber, squash, or replace Tasks 12, 13, or 14 in `docs/superpowers/plans/2026-08-09-physical-atmosphere-volumetric-clouds.md`; those tasks consume the resolved ABI introduced here.
- Keep the physical atmosphere and the `192x108` sky-view LUT. Do not add a legacy sky, auto exposure, a time-of-day profile, a constant night term, or a larger LUT unless the post-filter vertical/edge/seam gates fail.
- `atmosphere_sky_view` alone receives linear min/mag filtering, nearest mip selection, repeat U, and clamp-to-edge V/W. G-buffer, material, depth, ID, irradiance-coefficient, and cloud-shadow sampling retain their current contracts.
- Dither is achromatic, deterministic, viewport/scene-only, after tone mapping and explicit sRGB OETF, before 8-bit quantization, and before ImGui composition. It never reads frame index, jitter, time, camera state, DLSS phase, or random state.
- The atmosphere irradiance resource remains a `3x3` image containing nine directional SH coefficients. Every diffuse/fog/cloud consumer evaluates 9 SH before multiplying `sky_irradiance_modifier_rgb`.
- Raster, native RT, GI, and volumetrics receive one committed resolved snapshot per frame. No shader derives elevation, direct-world ratio, ambient ratio, or a substitute flat ambient colour.
- Candidate generation/allocation/validation/descriptor-publication is transactional. Failure retains the complete last-valid LUT set, direction, nine SH coefficients, `atmospheric_direct_base_rgb`, generation serial, and descriptors, then resolves current sanitized non-atmosphere controls against that last-valid state.
- The four new editable fields remain in `render.lighting`, use the generic property panel/FIFO path, default old settings/worlds without schema conversion, and are registered to their exact `MATTER_*` environment names.
- Use `C:/msys64/ucrt64/bin/g++.exe`. Pass `TMP=C:/Users/webde/AppData/Local/Temp` and `TEMP=C:/Users/webde/AppData/Local/Temp` as explicit make variables, and set them explicitly for every native test/editor process; exporting them alone is insufficient because MSYS2 make overwrites them.
- When GLSL changes, build in this order: `make -C MatterEngine3 vulkan-spirv`, `make -C MatterEngine3`, `make -C MatterEditor windows`. Run C++ targets sequentially with `GRAPHICS=GRAPHICS_API_OPENGL_43`; do not run the repository test suite in parallel.
- Treat `D:\` as protected external/user storage: do not create, modify, delete, or use captures/caches there. Keep all generated PNGs, `.done` files, command logs, metrics, and temporary test data under `MatterEditor/build/validation/atmosphere-presentation/` or `C:/Users/webde/AppData/Local/Temp`; commit assertions and fixture metadata, never captures.
- Preserve the existing user-owned worktree state, including the deleted shader junction entries and `.tmp-task*` directories. Stage named files only; never use `git add -A` or a destructive cleanup command.

---

## File Structure and Stable Interfaces

### New files

- `MatterEngine3/include/matter/display_dither.h` — the canonical row-major rank table, FNV oracle, coordinate-to-rank mapping, and CPU code-space oracle.
- `MatterEngine3/include/matter/atmosphere_lighting.h` — sanitized curve helpers, source inputs, resolved snapshot, component-wise RGB helpers, change mask, and history decision.
- `MatterEngine3/shaders_vk/environment_sampling_test.comp` — test-only compute entry that samples the production `environment_common.glsl` sky helper into an SSBO for vertical/seam GPU comparison.
- `projects/world_demo/objects/AtmospherePresentationReceiver.js` / `projects/world_demo/worlds/AtmospherePresentationFixture.js` — deterministic white Lambert receiver and vertical occluder plus fixed fog, no clouds, and fixed camera/world seed.
- `MatterEngine3/tools/atmosphere_presentation_metrics.py` — strict FIFO log/status/PNG parser and numerical/ROI acceptance evaluator.

### Existing files with focused changes

- `MatterEngine3/src/render/vk_scene_renderer.h` / `.cpp` — dedicated sky sampler, sampling/display test fixtures, resolved environment ABI, candidate publication, narrow history invalidation, status snapshot, and test failure injection.
- `MatterEngine3/src/render/vk_atmosphere.h` / `.cpp` — candidate-owned LUT sets, irradiance readback/validation, full committed state, retirement after the protecting frame fence, and exact failure retention.
- `MatterEngine3/shaders_vk/atmosphere_sky_view.comp` and `environment_common.glsl` — centred periodic bins and the shared `fract`/centre-clamped lookup.
- `MatterEngine3/shaders_vk/display_transform.frag` — explicit OETF, exact 8x8 offset, clamp, and UNORM/sRGB storage branches.
- `MatterEngine3/shaders_vk/composite.frag`, `rt_lighting.rgen`, and `vol_scatter.comp` — consume the four independent resolved RGB lanes from `EnvironmentBlock`.
- `MatterEngine3/src/render/vk_volumetrics.h` / `.cpp` — remove independent sun/sky colour mirrors and consume the committed environment lighting block; expose ambient-history invalidation count to tests.
- `MatterEngine3/include/matter/world_session.h`, `MatterEngine3/src/matter_engine.cpp`, `MatterEngine3/src/render/vk_lighting_controls.h` / `.cpp` — new editable fields, sanitization, source assembly, resolved-status propagation, and requested-versus-committed separation.
- `MatterEngine3/include/matter/props.h`, `MatterEngine3/src/props/props.cpp`, and `MatterEditor/src/property_editor.cpp` — add exact `uint64_t` read-only display/FIFO formatting as `Type::UInt64`; persistence and editable widgets remain disabled for the status fields.
- `MatterEditor/src/editor_props.h` / `.cpp`, `ui.h`, `ui_lighting_controls.cpp`, `main.cpp`, and `viewer_commands.h` — Lighting properties/docs, session/status groups, requested/resolved updates, typed FIFO commands, successful-present serial, and zero-settle capture.
- `MatterEngine3/tests/atmosphere_tests.cpp`, `shader_source_tests.cpp`, `vk_scene_renderer_tests.cpp`, `vulkan_smoke_tests.cpp`, `property_editor_tests.cpp`, and `props_tests.cpp` — CPU equations, source/ABI inspection, descriptor/sampler/GPU sampling, dither readback, properties/back compatibility, candidate failures, replay, histories, and FIFO grammar.
- `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`, `MatterEditor/Makefile`, and `MatterEngine3/shaders_gen/embedded_spirv.h` — new header/test shader dependencies and regenerated SPIR-V.
- `MatterEngine3/tools/atmosphere_cloud_shots.sh` and `MatterEngine3/docs/rendering.md` — one-process `atmosphere-presentation` suite, protocol, status grammar, commands, matrix, and evidence instructions.

Use these C++ interfaces exactly:

```cpp
// MatterEngine3/include/matter/display_dither.h
namespace matter {
inline constexpr std::array<uint8_t, 64> kDisplayDitherRanks = {
    37,12,54, 1,46,27,61, 8, 18,43, 5,58,31,50,14,40,
    63,22,35,10,48, 3,56,29, 16,45, 7,60,25,52,11,38,
    33, 0,47,20,57,15,42,30,  9,53,24,62, 4,36,19,51,
    41,13,55,28,59, 6,44,21, 26,49, 2,39,17,34,23,32};
inline constexpr uint32_t kDisplayDitherFnv1a32 = 0xdc0d948bu;
constexpr uint32_t display_dither_fnv1a32() noexcept;
constexpr uint8_t display_dither_rank(uint32_t pixel_x,
                                      uint32_t pixel_y) noexcept;
constexpr float display_dither_code_offset(uint32_t pixel_x,
                                           uint32_t pixel_y) noexcept;
Float3 apply_display_dither_code(Float3 encoded_code,
                                uint32_t pixel_x,
                                uint32_t pixel_y) noexcept;
}  // namespace matter
```

```cpp
// MatterEngine3/include/matter/atmosphere_lighting.h
namespace matter {
struct AtmosphereLightingSources {
    Float3 atmospheric_direct_base_rgb{};
    Float3 authored_display_sky_chroma_rgb{};
    Float3 authored_irradiance_chroma_rgb{};
    Float3 live_sun_tint_rgb{1.0f, 1.0f, 1.0f};
    Float3 live_sky_tint_rgb{1.0f, 1.0f, 1.0f};
    float sun_multiplier = 1.67f;
    float sky_multiplier = 0.77f;
    float sky_irradiance_multiplier = 1.0f;
    float day_ambient_multiplier = 0.25f;
    float twilight_ambient_multiplier = 1.0f;
    float sunset_direct_ratio = 0.25f;
    float elevation_deg = 54.525963f;
};

struct ResolvedAtmosphereLighting {
    Float3 atmospheric_direct_base_rgb{};
    Float3 direct_base_rgb{};
    Float3 direct_world_sun_rgb{};
    Float3 sun_disc_rgb{};
    Float3 sky_display_modifier_rgb{};
    Float3 sky_irradiance_modifier_rgb{};
    float direct_world_ratio = 0.0f;
    float sky_ambient_ratio = 0.0f;
    float resolved_elevation_deg = 0.0f;
};

float direct_world_ratio(float elevation_deg,
                         float sunset_direct_ratio) noexcept;
float sky_twilight_mix(float elevation_deg) noexcept;
float sky_ambient_ratio(float elevation_deg,
                        float day_ambient_multiplier,
                        float twilight_ambient_multiplier) noexcept;
ResolvedAtmosphereLighting resolve_atmosphere_lighting(
    const AtmosphereLightingSources&) noexcept;

enum AtmosphereLightingChange : uint32_t {
    kAtmosphereChangeNone       = 0,
    kAtmosphereChangeDirect     = 1u << 0,
    kAtmosphereChangeDisplay    = 1u << 1,
    kAtmosphereChangeIrradiance = 1u << 2,
    kAtmosphereChangeEmission   = 1u << 3,
    kAtmosphereChangeExposure   = 1u << 4,
    kAtmosphereChangeDisc       = 1u << 5,
    kAtmosphereChangeShadow     = 1u << 6,
};
struct AtmosphereHistoryDecision {
    bool reset_diffuse_gi = false;
    bool reset_reflection_miss = false;
    bool reset_volumetric = false;
};
AtmosphereHistoryDecision atmosphere_history_decision(uint32_t change_mask,
                                                       bool full_commit) noexcept;
}  // namespace matter
```

The resolver implements these equations verbatim, with `smoothstep(a,b,x)` using `q=clamp((x-a)/(b-a),0,1)` and `q*q*(3-2*q)`:

```text
e <= 0:       direct_world_ratio = 0
0 < e < 5:    direct_world_ratio = s * smoothstep(0,5,e)
5 <= e < 45: direct_world_ratio = s + (1-s) * smoothstep(5,45,e)
e >= 45:      direct_world_ratio = 1

twilight_mix(e)      = 1 - smoothstep(-6,5,e)
sky_ambient_ratio(e) = mix(d,t,twilight_mix(e))

direct_base_rgb = atmospheric_direct_base_rgb * live_sun_tint_rgb
                  * sun_multiplier
sun_disc_rgb = direct_base_rgb
direct_world_sun_rgb = direct_base_rgb * direct_world_ratio(e)
sky_display_modifier_rgb = authored_display_sky_chroma_rgb
                           * sky_multiplier * live_sky_tint_rgb
sky_irradiance_modifier_rgb = authored_irradiance_chroma_rgb
                              * sky_irradiance_multiplier
                              * sky_ambient_ratio(e)
```

The committed atmosphere and GPU ABI are:

```cpp
struct AtmosphereRequest {
    matter::AtmosphereSettings settings{};
    float camera_world_y = 0.0f;
    matter::Float3 normalized_to_sun{0.0f, 1.0f, 0.0f};
    matter::Float3 authored_sun_rgb{1.0f, 1.0f, 1.0f};
};

struct AtmosphereCommittedState {
    matter::AtmosphereSettings settings{};
    matter::Float3 normalized_to_sun{0.0f, 1.0f, 0.0f};
    float camera_world_y = 0.0f;
    std::array<matter::Float3, 9> irradiance_sh{};
    matter::Float3 atmospheric_direct_base_rgb{};
    uint64_t generation_serial = 0;
};

struct alignas(16) EnvironmentLightingGpu {
    float direct_world_sun_ratio[4];       // RGB, direct_world_ratio
    float sun_disc_reserved[4];            // RGB, 0
    float sky_display_reserved[4];         // RGB, 0
    float sky_irradiance_ambient_ratio[4]; // RGB, sky_ambient_ratio
};
static_assert(sizeof(EnvironmentLightingGpu) == 64);
```

Append those four `vec4`s after `cloud_filter` in GLSL `EnvironmentBlock` with member names `direct_world_sun_ratio`, `sun_disc_reserved`, `sky_display_reserved`, and `sky_irradiance_ambient_ratio`. Remove `sun_color`/`sky_color` duplicates from composite, RT/GI, and volumetric push constants; all consumers read the same UBO values. `sample_physical_sky` multiplies only `sky_display_reserved.rgb`; `sample_sky_irradiance` evaluates all nine coefficients and then multiplies only `sky_irradiance_ambient_ratio.rgb`; direct BRDF/fog/cloud terms use only `direct_world_sun_ratio.rgb`; the analytic disc uses only `sun_disc_reserved.rgb`.

---

### Task 1: Add periodic linear sky sampling and deterministic scene code-space dither

**Files:**
- Create: `MatterEngine3/include/matter/display_dither.h`
- Create: `MatterEngine3/shaders_vk/environment_sampling_test.comp`
- Modify: `MatterEngine3/shaders_vk/atmosphere_sky_view.comp`
- Modify: `MatterEngine3/shaders_vk/environment_common.glsl`
- Modify: `MatterEngine3/shaders_vk/display_transform.frag`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `MatterEditor/Makefile`
- Test: `MatterEngine3/tests/atmosphere_tests.cpp`
- Test: `MatterEngine3/tests/shader_source_tests.cpp`
- Test: `MatterEngine3/tests/vk_scene_renderer_tests.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Regenerate: `MatterEngine3/shaders_gen/embedded_spirv.h`

**Interfaces:**
- Consumes: the existing `192x108` RGBA16F sky-view producer, per-frame environment descriptor set, `display.srgb_output`, ACES pass, and swapchain readback test path.
- Produces: `sky_view_linear_sampler_`; exact `atmosphere_sky_uv`; `kDisplayDitherRanks`, checksum, and CPU oracle; `test_dispatch_environment_sampling_fixture`; and `test_dispatch_display_transform_fixture`. Task 2 keeps these bindings and uploads its resolved lighting block beside them.

- [ ] **Step 1: Add failing CPU rank, checksum, UV, and curve-location tests**

Add these exact assertions to `atmosphere_tests.cpp` (with a local `fract`/clamp CPU oracle for UV) and call the test from `main()`:

```cpp
void test_periodic_sky_uv_and_display_dither_oracles() {
    CHECK(matter::display_dither_fnv1a32() == 0xdc0d948bu,
          "display dither rank bytes match the approved FNV oracle");
    bool seen[64]{};
    double sum = 0.0;
    for (uint32_t y = 0; y < 8; ++y) for (uint32_t x = 0; x < 8; ++x) {
        const uint8_t rank = matter::display_dither_rank(x, y);
        CHECK(rank < 64 && !seen[rank], "display dither is a 0..63 permutation");
        seen[rank] = true;
        sum += matter::display_dither_code_offset(x, y);
    }
    CHECK(matter::display_dither_rank(0, 0) == 37 &&
              matter::display_dither_rank(7, 7) == 32,
          "display dither uses exact row-major pixel indexing");
    CHECK(std::fabs(sum) <= 1.0e-12,
          "one complete dither tile has zero mean");
    CHECK(matter::display_dither_code_offset(1, 4) == -0.5f / 255.0f &&
              matter::display_dither_code_offset(0, 2) == 0.5f / 255.0f,
          "dither extrema are exact half-LSB offsets");
    CHECK(sky_uv(-0.25f, 0.0f).x == 0.75f &&
              sky_uv(1.25f, 1.0f).x == 0.25f,
          "sky U wraps periodically");
    CHECK(sky_uv(0.0f, 0.0f).y == 0.5f / 108.0f &&
              sky_uv(0.0f, 1.0f).y == 107.5f / 108.0f,
          "sky V clamps to edge-texel centres");
}
```

Add source assertions to `shader_source_tests.cpp` that `display_transform.frag` contains the 64 decimal ranks in order, `gl_FragCoord`, `linear_to_srgb`, `srgb_to_linear`, and `code_dithered`, and does not contain `frame_index`, `jitter`, `time`, `random`, or `pcg`. Assert `environment_common.glsl` contains `fract(azimuth_u)` and `clamp(v, 0.5 / 108.0, 107.5 / 108.0)`, while `atmosphere_sky_view.comp` contains `(float(pixel.x) + 0.5) / 192.0` and no duplicate endpoint expression `float(pixel.x) / 191.0`.

- [ ] **Step 2: Add failing sampler/descriptor, GPU sampling, and pre-quantization dither fixtures**

Declare these exact test-only types and methods under `MATTER_VK_TEST_FAULT_INJECTION` in `vk_scene_renderer.h`:

```cpp
struct EnvironmentSamplingGpuFixture {
    std::array<matter::Float3, 192 * 108> lut{};
    std::vector<matter::Float2> uv;
};
struct DisplayTransformGpuFixture {
    uint32_t width = 8, height = 8;
    matter::Float3 hdr{0.21404114f, 0.21404114f, 0.21404114f};
    float exposure_ev = 0.0f;
    bool srgb_output = false;
};
bool test_dispatch_environment_sampling_fixture(
    const EnvironmentSamplingGpuFixture&, std::vector<matter::Float3>&,
    std::string&);
bool test_dispatch_display_transform_fixture(
    const DisplayTransformGpuFixture&, std::vector<matter::Float3>&,
    std::string&);
VkSampler test_sky_view_sampler() const;
VkSampler test_composite_sampler() const;
```

In `vulkan_smoke_tests.cpp`, build a deterministic LUT where each texel is `rgb=(u_center, v_center, 0.25*u_center+0.75*v_center)`. Submit 432 vertical UVs away from disc/horizon and compare every channel to the CPU bilinear periodic-U/clamped-V oracle within `1e-3`; when reference luminance slope is over `1e-4`, reject identical-output plateaus longer than two samples. Submit V=0/V=1 edge probes and 256 pairs at `u=epsilon`/`u=1-epsilon`; require finite edge-pair bounds, relative seam error `<=0.005` (absolute `<=1e-3` near black), and seam finite difference `<=2*median_adjacent_u_difference`. Assert the actual sky sampler handle is non-null and differs from the representative nearest G-buffer `composite_sampler_` handle.

Run the display fixture for a 16x16 interior at encoded `0.5`: inverse the fixture's ACES input so the shader produces that code before dither, compare every pixel to `apply_display_dither_code`, require equal RGB offsets, extrema, each 8x8 mean `<=1e-8`, and byte-identical results for two static submissions. Run code rails 0 and 1 and require clamped `[0,1]` while the oracle offset remains half-LSB bounded. Run once with `srgb_output=false` and once with `true`; apply the CPU sRGB OETF to the latter's returned linear attachment values and require the recovered encoded values to match the UNORM branch within `1e-6`.

- [ ] **Step 3: Run the focused tests and verify RED**

```powershell
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-shader-source TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-vk-scene-renderer TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp WIN_CXX=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP; $env:MATTER_VK_SMOKE_MODE='atmosphere'; & MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: RED because `display_dither.h`, the dedicated sampler/test shader, periodic helper, and dithered display output do not exist; after the tests compile, the sampler handle equality, vertical plateau, seam, and dither extrema assertions fail against the current nearest/no-dither path.

- [ ] **Step 4: Implement the canonical dither and exact display-space branches**

Implement the header interfaces with FNV-1a offset `2166136261u`, byte-wise XOR, multiply `16777619u`, unsigned wrap, rank index `(pixel_y & 7u)*8u+(pixel_x & 7u)`, and:

```cpp
return ((static_cast<float>(rank) - 31.5f) / 31.5f) * (0.5f / 255.0f);
```

In GLSL, compute `code=linear_to_srgb(aces_sdr(hdr))`, add the exact scalar `d` equally to RGB, clamp once, and write `code_dithered` directly for UNORM or `srgb_to_linear(code_dithered)` for sRGB. Keep debug passthrough byte-stable and undithered by returning before the dither block. Do not move the pass after ImGui and do not alter HDR/intermediate readback.

- [ ] **Step 5: Implement the periodic sky helper and dedicated sampler transaction**

Make `atmosphere_sky_uv` compute a raw azimuth fraction and return exactly:

```glsl
float azimuth_u = (azimuth + ENV_PI) / (2.0 * ENV_PI);
float v = zenith / ENV_PI;
return vec2(fract(azimuth_u), clamp(v, 0.5 / 108.0, 107.5 / 108.0));
```

Keep the compute producer at centred `(column+0.5)/192` bins. Create `sky_view_linear_sampler_` beside `composite_sampler_` with `LINEAR/LINEAR`, nearest mip, repeat U, clamp V/W, and bind it only to environment binding 0. Bind irradiance SH and all cloud images with their existing samplers. Create/bind it as part of environment descriptor setup, retain the old valid sampler/descriptors on injected failure, destroy retired sampler state only after the protecting frame-slot fence, and report the error instead of silently using nearest.

- [ ] **Step 6: Build shaders and run all Task 1 gates GREEN**

```powershell
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp WIN_CXX=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-shader-source TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-vk-scene-renderer TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP; $env:MATTER_VK_SMOKE_MODE='atmosphere'; & MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: all focused CPU/source/GPU gates PASS, the shader header is current, the representative G-buffer sampler remains nearest, vertical plateaus/seam metrics pass without increasing LUT resolution, and Vulkan validation reports no feature-caused warning/error.

- [ ] **Step 7: Commit the independently reviewable presentation change**

```powershell
git add MatterEngine3/include/matter/display_dither.h MatterEngine3/shaders_vk/environment_sampling_test.comp MatterEngine3/shaders_vk/atmosphere_sky_view.comp MatterEngine3/shaders_vk/environment_common.glsl MatterEngine3/shaders_vk/display_transform.frag MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/Makefile MatterEngine3/tests/Makefile MatterEditor/Makefile MatterEngine3/tests/atmosphere_tests.cpp MatterEngine3/tests/shader_source_tests.cpp MatterEngine3/tests/vk_scene_renderer_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/shaders_gen/embedded_spirv.h
git commit -m "fix: smooth sky presentation and add stable display dither"
```

---

### Task 2: Resolve independent sky irradiance, display, direct-world, and disc lighting atomically

**Files:**
- Create: `MatterEngine3/include/matter/atmosphere_lighting.h`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/src/render/vk_lighting_controls.h`
- Modify: `MatterEngine3/src/render/vk_lighting_controls.cpp`
- Modify: `MatterEngine3/src/render/vk_atmosphere.h`
- Modify: `MatterEngine3/src/render/vk_atmosphere.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/vk_volumetrics.h`
- Modify: `MatterEngine3/src/render/vk_volumetrics.cpp`
- Modify: `MatterEngine3/shaders_vk/environment_common.glsl`
- Modify: `MatterEngine3/shaders_vk/composite.frag`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/shaders_vk/vol_scatter.comp`
- Modify: `MatterEditor/src/editor_props.cpp`
- Modify: `MatterEditor/src/ui.h`
- Modify: `MatterEditor/src/ui_lighting_controls.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEditor/Makefile`
- Test: `MatterEngine3/tests/atmosphere_tests.cpp`
- Test: `MatterEngine3/tests/property_editor_tests.cpp`
- Test: `MatterEngine3/tests/shader_source_tests.cpp`
- Test: `MatterEngine3/tests/vk_scene_renderer_tests.cpp`
- Test: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Regenerate: `MatterEngine3/shaders_gen/embedded_spirv.h`

**Interfaces:**
- Consumes: Task 1's sampler/environment descriptor and committed physical LUT format; authored sun/sky colours; live lighting controls; raster, native RT/GI, and volumetric consumers.
- Produces: the stable interfaces in the preceding section; four new `VulkanLightingOverrides` fields; one candidate/committed atmosphere transaction; exact `EnvironmentLightingGpu`; `VkSceneRenderer::resolved_atmosphere_status()` for Task 3; and separate diffuse-GI, reflection/miss, and volumetric history counters.

- [ ] **Step 1: Write failing curve, sanitization, independence, and backwards-default tests**

In `atmosphere_tests.cpp`, add exact anchor checks for elevations `{90,45,5,0,-5,-12}` and ratios `{1,1,0.25,0,0,0}`; sample `[-90,90]` in `0.01` degree increments to require finite `[0,1]`, monotonic direct ratio, and one-sided differences within `1e-5` at `0`, `5`, and `45`. Require ambient endpoints `sky_ambient_ratio(5,0.25,1)==0.25` and `sky_ambient_ratio(-6,0.25,1)==1`, finite continuity across every dense sample, direct zero at `-5`, and a positive 9-SH upward/fog reference at `-5` without adding a constant at `-12`.

Resolve a fixture with non-white chroma/tints and assert component-wise equations exactly. Copy the result, change only display authored chroma/multiplier/tint, and `memcmp` all nine SH inputs, irradiance modifier, direct ratio, direct-world RGB, and exposure fixture bytes. In a second copy change only authored irradiance chroma/multiplier/day/twilight and `memcmp` background/miss/reflection display RGB. Feed NaN/inf and out-of-range controls through `sanitize_vulkan_lighting_overrides` and require day/twilight/irradiance fallback `{0.25,1,1}` clamped `[0,4]`, sunset fallback `0.25` clamped `[0,1]`, and unchanged elevation fallback/clamp `[-90,90]`.

In `property_editor_tests.cpp`, load old JSON with no new keys and assert the four defaults, exact paths/labels/ranges/env names, `.read_only()==false`, and generic round-trip values. Require docs to contain all three statements: `Visible sky does not change ambient.`, `Ambient does not recolour the visible sky.`, and `Sunset direct does not affect disc presentation.`

- [ ] **Step 2: Write failing ABI/source and raster/native-RT/volumetric GPU ratio tests**

In `shader_source_tests.cpp`, reject `lighting.sky_color`, `lighting.sun_color`, `constants.sky_color`, `constants.sun_color`, `pc.sky_color`, and `pc.sun_color` in the three consumers. Require the four exact UBO names, nine-iteration SH evaluation before the irradiance multiply, display modifier only on background/miss/reflection helpers, world sun only on direct terms, disc RGB only on analytic disc terms, and no shader-side `smoothstep(-6`, `smoothstep(0,5`, elevation property, or ambient/direct ratio reconstruction.

Extend the existing physical-atmosphere GPU fixture in `vulkan_smoke_tests.cpp` to render both raster and native RT at `{90,45,5,0,-5,-12}` with a forced history reset. Before implementation, assert the GPU-published direct ratios equal `{1,1,0.25,0,0,0}` within `1e-6`, raster/native-RT resolved direct RGB channels agree within `2e-3`, raster/RT/fog direct contributions are exactly zero at and below zero elevation, and the `-5` upward receiver plus fog remain over `1e-4` linear from evaluated SH. Treat native RT unavailable as fixture failure. This is the required RED GPU-ratio gate: it must fail on the current atmosphere-transmittance-only sunlight and shared `sky_color` ABI.

- [ ] **Step 3: Write failing candidate failure/replay/history tests**

Add `test_fail_next_atmosphere_generation()`, `test_fail_next_atmosphere_descriptor_publication()`, `test_atmosphere_lut_handles()`, `test_atmosphere_history_counters()`, and `test_resolved_atmosphere_status()` accessors under the existing fault-injection define. Establish a committed baseline, then inject generation and descriptor-publication failure separately with no concurrent live edit; assert byte-identical LUT handles, direction, nine SH, atmospheric direct base, resolved direct/ambient/display values, generation serial, and all three history counters.

Repeat each failure while changing every replay category in one batch: sun multiplier/tint, display sky multiplier/tint/chroma, authored irradiance chroma, irradiance multiplier, day/twilight ambient, sunset direct, emission, exposure, sun diameter, and shadow samples. Require old LUT/direction/SH/atmospheric base/serial, but current sanitized controls in `direct_base_rgb`, direct ratio/RGB, display/irradiance modifiers, disc/exposure/emission/shadow constants. Assert the failure itself adds no reset; the combined edit adds exactly the union of prescribed narrow resets once. Finally allow a successful candidate and require exactly one serial advance, one diffuse-GI reset, and one volumetric reset.

- [ ] **Step 4: Run the focused targets and verify RED**

```powershell
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-property-editor TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-shader-source TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-vk-scene-renderer TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp WIN_CXX=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP; $env:MATTER_VK_SMOKE_MODE='atmosphere'; & MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: RED on missing fields/interfaces, shared sky/sun ABI, noon ambient remaining at the former value, 5-degree direct ratio not being exactly `0.25`, and in-place LUT mutation/failure semantics.

- [ ] **Step 5: Add the four properties and central resolver**

Append these members after `sky_tint` in `VulkanLightingOverrides` without disturbing the first four positional aggregate members:

```cpp
float day_ambient_multiplier = 0.25f;
float twilight_ambient_multiplier = 1.0f;
float sky_irradiance_multiplier = 1.0f;
float sunset_direct_ratio = 0.25f;
```

Register them after existing sky controls with exact paths, labels, ranges, environment names, and docs:

```cpp
prop(&V::day_ambient_multiplier, "day_ambient_multiplier")
 .label("Day ambient").range(0.0f,4.0f).env("MATTER_DAY_AMBIENT_MULTIPLIER")
 .doc("Physical irradiance at e >= +5 deg. Visible sky does not change ambient."),
prop(&V::twilight_ambient_multiplier, "twilight_ambient_multiplier")
 .label("Twilight ambient").range(0.0f,4.0f).env("MATTER_TWILIGHT_AMBIENT_MULTIPLIER")
 .doc("Physical irradiance at e <= -6 deg. Ambient does not recolour the visible sky."),
prop(&V::sky_irradiance_multiplier, "sky_irradiance_multiplier")
 .label("Sky irradiance").range(0.0f,4.0f).env("MATTER_SKY_IRRADIANCE_MULTIPLIER")
 .doc("Post-9SH physical ambient multiplier; independent of visible sky."),
prop(&V::sunset_direct_ratio, "sunset_direct_ratio")
 .label("Sunset direct").range(0.0f,1.0f).env("MATTER_SUNSET_DIRECT_RATIO")
 .doc("Direct-world ratio at +5 deg. Sunset direct does not affect disc presentation.")
```

Implement `atmosphere_lighting.h` exactly as specified above. Invalid derived direct RGB becomes zero; invalid irradiance uses the last-valid irradiance modifier when available, otherwise zero-safe RGB. `matter_engine.cpp` passes authored display and authored irradiance chroma as two independent copies of `manifest.lights.sky_color`, computes elevation from the requested/committed sun direction with `sun_angles_from_direction`, and never derives a value from tone-mapped pixels.

- [ ] **Step 6: Replace in-place atmosphere writes with a candidate/commit transaction**

Refactor `VkAtmosphere` so `build_candidate(const AtmosphereRequest&, Candidate&, error)` allocates a complete LUT image set, records all dirty compute passes into an immediate submission, reads/validates all nine irradiance texels, computes `atmospheric_direct_base_rgb = extraterrestrial_solar_rgb * atmospheric_transmittance_rgb * authored_sun_rgb`, and leaves the active state untouched. `commit_candidate(Candidate&&, protected_frame_slot)` increments serial exactly once and retires the prior LUT set behind the frame-slot fence. `discard_candidate` destroys only uncommitted resources. Initialization commits the existing neutral emergency set as serial zero.

In `VkSceneRenderer::prepare_frame`, compare the requested atmosphere settings/camera altitude/normalized direction/authored sun chroma with the committed state. Build a candidate only for atmosphere-linked changes. Publish candidate binding 0 with Task 1's sampler and binding 1 with its irradiance image into the selected completed-slot descriptor; after the injected/publication checks succeed, commit the candidate and publish resolved constants from the current sanitized live controls. On failure, report the precise error, preserve the complete committed state/descriptors, and call the constants-only resolver against current controls and the last-valid committed atmosphere. Requested elevation remains untouched and is never substituted into the committed state after failure.

- [ ] **Step 7: Publish one exact environment lighting ABI and narrow histories**

Append `EnvironmentLightingGpu` to the mapped UBO and update the neutral block size/flush ranges. Remove sun/sky RGB from `VkSceneLighting`, RT `GiConstants`, and volumetric `ScatterConstants`; update their `static_assert` sizes and upload all four lanes from one `ResolvedAtmosphereLighting`. Keep 9-SH texture fetch/evaluation directional and apply the modifier afterward.

Use this exact history table for both normal edits and failure replay:

| Change | Diffuse GI | Reflection/miss | Volumetric scatter | Atmosphere serial |
|---|---:|---:|---:|---:|
| full successful atmosphere commit | +1 | 0 | +1 | +1 |
| `sun_multiplier`, sun tint, `sunset_direct_ratio` | +1 | 0 | +1 | 0 |
| authored irradiance chroma, irradiance multiplier, day/twilight | +1 | 0 | +1 | 0 |
| authored display chroma, sky multiplier/tint | 0 | +1 | 0 | 0 |
| emission multiplier | existing emission-source rule | existing emission-source rule | existing emission-source rule | 0 |
| exposure or shadow samples | 0 | 0 | 0 | 0 |
| sun angular diameter | existing direct/disc/reflection rule | existing direct/disc/reflection rule | existing direct/disc/reflection rule | 0 |
| candidate failure without live edit | 0 | 0 | 0 | 0 |

Do not reset cloud density/optical-depth history for any of the four new controls. A no-pending-candidate day/twilight/sunset edit is a constants-only transaction and does not regenerate the LUT.

- [ ] **Step 8: Build and run all Task 2 gates GREEN**

```powershell
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp WIN_CXX=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-property-editor TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-shader-source TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-vk-scene-renderer TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP; $env:MATTER_VK_SMOKE_MODE='atmosphere'; & MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: all gates PASS; default noon ambient is 0.25 of the former effective linear irradiance input, `-5` has zero direct with positive SH-lit receiver/fog, `-12` has no injected floor, raster/native RT direct RGB agrees, failure retains the full prior atmosphere transaction, replay applies every current live value once, and Vulkan validation is clean.

- [ ] **Step 9: Commit the independently reviewable lighting/transaction change**

```powershell
git add MatterEngine3/include/matter/atmosphere_lighting.h MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEngine3/src/render/vk_lighting_controls.h MatterEngine3/src/render/vk_lighting_controls.cpp MatterEngine3/src/render/vk_atmosphere.h MatterEngine3/src/render/vk_atmosphere.cpp MatterEngine3/src/render/vk_scene_renderer.h MatterEngine3/src/render/vk_scene_renderer.cpp MatterEngine3/src/render/vk_volumetrics.h MatterEngine3/src/render/vk_volumetrics.cpp MatterEngine3/shaders_vk/environment_common.glsl MatterEngine3/shaders_vk/composite.frag MatterEngine3/shaders_vk/rt_lighting.rgen MatterEngine3/shaders_vk/vol_scatter.comp MatterEditor/src/editor_props.cpp MatterEditor/src/ui.h MatterEditor/src/ui_lighting_controls.cpp MatterEngine3/Makefile MatterEditor/Makefile MatterEngine3/tests/atmosphere_tests.cpp MatterEngine3/tests/property_editor_tests.cpp MatterEngine3/tests/shader_source_tests.cpp MatterEngine3/tests/vk_scene_renderer_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp MatterEngine3/shaders_gen/embedded_spirv.h
git commit -m "feat: separate atmosphere presentation from world lighting"
```

---

### Task 3: Expose resolved status and automate the exact raster/native-RT atmosphere matrix

**Files:**
- Create: `projects/world_demo/objects/AtmospherePresentationReceiver.js`
- Create: `projects/world_demo/worlds/AtmospherePresentationFixture.js`
- Create: `MatterEngine3/tools/atmosphere_presentation_metrics.py`
- Modify: `MatterEngine3/include/matter/props.h`
- Modify: `MatterEngine3/src/props/props.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEditor/src/editor_props.h`
- Modify: `MatterEditor/src/editor_props.cpp`
- Modify: `MatterEditor/src/property_editor.cpp`
- Modify: `MatterEditor/src/ui.h`
- Modify: `MatterEditor/src/main.cpp`
- Modify: `MatterEditor/src/viewer_commands.h`
- Modify: `MatterEngine3/tools/atmosphere_cloud_shots.sh`
- Modify: `MatterEngine3/docs/rendering.md`
- Modify: `MatterEngine3/tests/props_tests.cpp`
- Modify: `MatterEngine3/tests/property_editor_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: Task 2's committed `ResolvedAtmosphereLighting`, generation serial, availability/render-path state, successful-present notification, generic registry, existing `cam/set/get/stats/shot/quit`, `.done` sentinel, and one-process atmosphere harness.
- Produces: exact get-only `viewer.session.*` and `viewer.atmosphere_status.*` properties; typed `render_path`, `history_reset`, `wait_frames`, and `shot_now` commands; and a complete fixed-exposure 10-frame acceptance matrix plus metrics.

- [ ] **Step 1: Add failing UInt64/read-only/status/property tests**

Add `Type::UInt64`, `get_uint64`, `set_uint64`, type deduction, decimal parse/format, copy/equality/default support, and JSON encode/decode coverage to `props_tests.cpp`; a `uint64_t` value `4294967297` must format exactly as `4294967297`. In `main.cpp`'s FIFO setter, reject `ReadOnly` before parsing and print `set: <path> is read-only`.

Define these exact editor-owned status structs in `ui.h`:

```cpp
enum class ViewerRenderPathStatus : int32_t {
    Raster = 0, NativeRt = 1, NativeRtUnavailable = 2
};
struct ViewerSessionStatus {
    ViewerRenderPathStatus render_path = ViewerRenderPathStatus::Raster;
    uint64_t presented_frame_serial = 0;
    bool native_rt_available = false;
};
struct ViewerAtmosphereStatus {
    uint64_t generation_serial = 0;
    float resolved_elevation_deg = 0.0f;
    matter::Float3 atmospheric_direct_base_rgb{};
    float direct_world_ratio = 0.0f;
    matter::Float3 direct_base_rgb{};
    matter::Float3 direct_world_sun_rgb{};
    float sky_ambient_ratio = 0.0f;
    matter::Float3 sky_display_modifier_rgb{};
    matter::Float3 sky_irradiance_modifier_rgb{};
};
```

Bind exact `viewer.session` and `viewer.atmosphere_status` groups at `Scope::Session`; every field is `.read_only().no_serialize()`. Enum labels are exactly `raster`, `native_rt`, `native_rt_unavailable`. Add property tests for every exact path/type, `get: <path> = <value>` formatting, rejection of `set`, and absence from saved session/world JSON.

- [ ] **Step 2: Add failing typed-command parsing and successful-present sequencing tests**

Add exact typed commands to `viewer_commands.h`: `FifoRenderPath{RenderPath requested}`, `FifoHistoryReset`, `FifoWaitFrames{uint32_t count}`, and `FifoScreenshotNow{std::string path}`. Extract FIFO line parsing into the existing testable parser seam and assert:

```text
render_path raster
render_path native_rt
history_reset
wait_frames 3
shot_now C:\absolute\frame.png
```

Reject `wait_frames 0`, negative/non-integer/overflow counts, relative shot paths, and unknown render paths. Tests must prove waits count only successful presents; failed/acquire-only frames do not advance `presented_frame_serial`; `shot_now` captures the next successful present with zero compatibility-settle frames; and queued waits/captures survive intervening failed frames without printing success early.

- [ ] **Step 3: Run property/command tests and verify RED**

```powershell
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-props TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-property-editor TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor build/windows/vulkan_smoke_tests.exe TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp WIN_CXX=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: RED because UInt64, status groups, read-only enforcement, typed commands, present-based waiting, and immediate capture do not exist.

- [ ] **Step 4: Implement status publication and exact FIFO success/failure lines**

Copy Task 2's committed status into `ViewerAtmosphereStatus` once per frame; never substitute requested sun elevation. Increment `presented_frame_serial` only from the existing successful-present completion path. Implement these exact lines:

```text
render_path: raster
render_path: native_rt
render_path: native_rt unavailable
history_reset: requested
wait_frames: complete N frame_serial=M
shot_now: queued <absolute-png-path>
```

`render_path native_rt` returns command failure and the unavailable line unless `viewer.session.native_rt_available=true`. `history_reset` queues one one-shot temporal reset across diffuse GI, reflection/miss, and volumetric scatter without changing atmosphere generation. `wait_frames N` records `target_serial=current+N` and prints completion only when the successful-present serial reaches it. `shot_now` writes the next successfully presented viewport, then the existing `<png>.done`; existing `shot` retains its three-frame compatibility settle.

- [ ] **Step 5: Create the exact deterministic world fixture**

Create `AtmospherePresentationReceiver.js` as one white diffuse instrument; the thin box is an exactly 8m x 8m up-facing receiver and the second box is an exactly 0.6m x 2m x 0.6m vertical occluder centred at `(0,1,0)`:

```js
class AtmospherePresentationReceiver extends Part {
  build(p) {
    this.fill(MAT.plaster);
    this.box([0,-0.05,0], [8,0.1,8]);
    this.box([0,1,0], [0.6,2,0.6]);
  }
}
```

Create `AtmospherePresentationFixture.js` with seed `20260809`, camera position `[0,2,12]`, target `[0,1,0]`, no cloud array, fog `{density:0.002,floor:0,falloff:30,color:[0.9,0.92,0.95],wind:[0,0,0]}`, volumetrics enabled with temporal blend zero, a white authored sun/sky, and one identity-transformed instrument root:

```js
class AtmospherePresentationFixture extends World {
  static params = { worldSeed: 20260809 };
  static camera = { position: [0,2,12], target: [0,1,0] };
  static fog = { density: 0.002, floor: 0, falloff: 30,
                 color: [0.9,0.92,0.95], wind: [0,0,0] };
  static volumetrics = { enabled: true, temporalBlend: 0.0 };
  static lights = {
    sun: { dir: [0,-1,0], color: [1,1,1] },
    sky: { color: [1,1,1] },
  };
  static roots = [
    { module: "AtmospherePresentationReceiver",
      transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
  ];
}
```

- [ ] **Step 6: Extend the existing harness with the exact ordered matrix**

Add suite `atmosphere-presentation`; require `MATTER_WORLD=AtmospherePresentationFixture`, default 1280x720, `cam 0 2 12 0 1 0`, both readiness markers, one process, timeout of 240 status polls, and one cleanup-only kill trap. For each `path in raster native_rt` send `render_path <path>`, require the matching session echo, and for native RT require `get viewer.session.native_rt_available = true` before any elevation. For each `elevation in 90 5 0 -5 -12`, read `S0`, then send this exact ordered block:

```text
set render.lighting.exposure_ev -2
set render.lighting.day_ambient_multiplier 0.25
set render.lighting.twilight_ambient_multiplier 1
set render.lighting.sky_irradiance_multiplier 1
set render.lighting.sunset_direct_ratio 0.25
set render.lighting.sun_elevation_deg <elevation>
wait_frames 1
get viewer.atmosphere_status.generation_serial
get viewer.atmosphere_status.resolved_elevation_deg
```

Repeat only the final three lines until `generation_serial>S0` and `abs(resolved_elevation_deg-elevation)<=1e-4`, then send exactly:

```text
history_reset
wait_frames 3
get viewer.session.render_path
get viewer.session.presented_frame_serial
get viewer.atmosphere_status.atmospheric_direct_base_rgb
get viewer.atmosphere_status.direct_world_ratio
get viewer.atmosphere_status.direct_base_rgb
get viewer.atmosphere_status.direct_world_sun_rgb
get viewer.atmosphere_status.sky_ambient_ratio
get viewer.atmosphere_status.sky_display_modifier_rgb
get viewer.atmosphere_status.sky_irradiance_modifier_rgb
stats atmosphere-presentation-<elevation>
shot_now <absolute-png-path>
```

Wait for `.done` before proceeding. Parse requested lighting values separately from resolved status. Any `native_rt unavailable`, timeout, malformed/missing status, missing `.done`, renderer error, or process left alive is failure, never a skip. Send `quit` and wait normally.

- [ ] **Step 7: Implement strict status and image metrics**

The Python tool accepts `--log`, `--capture-dir`, and `--width 1280 --height 720`. Parse triples only in `(r,g,b)` form and exact `get:` grammar. For each path/elevation require CPU ratios within `1e-6`, raster/native-RT direct RGB channel agreement within `2e-3`, and identical exposure `-2` across all ten captures. Decode PNG sRGB to display-linear, invert the ACES rational curve using the non-negative root of `(2.43*y-2.51)x^2+(0.59*y-0.03)x+0.14*y=0`, then divide by `2^-2` to recover scene-linear ROI values.

Measure inclusive ROIs lit `[420,300]..[460,340]`, shadow `[500,300]..[540,340]`, and upward/fog `[430,260]..[470,290]`. Require at `-5`: direct ratio and direct RGB exactly zero and upward/fog mean `>1e-4`; at noon: lit mean `>=1.10*shadow mean`; at `-12`: no non-finite pixel and upward/fog mean lower than at `-5` for each path, proving no permanent ambient floor. Emit a JSON summary containing every requested/resolved value, per-ROI linear mean, raster/native-RT RGB delta, and PASS/FAIL gate.

- [ ] **Step 8: Build and run the complete sequential acceptance set**

```powershell
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-atmosphere TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-props TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-property-editor TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-shader-source TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3/tests run-vk-scene-renderer TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEngine3 vulkan-spirv TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe
C:\msys64\usr\bin\make.exe -C MatterEngine3 TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp CC=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
C:\msys64\usr\bin\make.exe -C MatterEditor windows TMP=C:/Users/webde/AppData/Local/Temp TEMP=C:/Users/webde/AppData/Local/Temp WIN_CXX=C:/msys64/ucrt64/bin/g++.exe GRAPHICS=GRAPHICS_API_OPENGL_43
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP; & MatterEditor/build/windows/vulkan_smoke_tests.exe
C:\msys64\usr\bin\bash.exe -n MatterEngine3/tools/atmosphere_cloud_shots.sh
$env:TMP='C:/Users/webde/AppData/Local/Temp'; $env:TEMP=$env:TMP; $env:MATTER_WORLD='AtmospherePresentationFixture'; C:\msys64\usr\bin\bash.exe MatterEngine3/tools/atmosphere_cloud_shots.sh atmosphere-presentation acceptance MatterEditor/build/validation/atmosphere-presentation
C:\msys64\usr\bin\python3.exe MatterEngine3/tools/atmosphere_presentation_metrics.py --log MatterEditor/build/validation/atmosphere-presentation/acceptance_viewer.log --capture-dir MatterEditor/build/validation/atmosphere-presentation --width 1280 --height 720
```

Expected: every target PASS sequentially; all ten `.done`-guarded captures exist; native RT was available and exercised; resolved ratios/status and raster/native-RT agreement pass; fixed-exposure fog remains positive at `-5`, direct is zero, noon receiver separation is at least 10%, deep night fades without a floor, and no Vulkan validation issue is introduced.

- [ ] **Step 9: Inspect and present capture evidence**

Open every PNG rather than checking existence alone. Show same-exposure raster/native-RT pairs for `90`, `5`, `0`, `-5`, and `-12`, plus close crops of the noon lit/shadow ROIs and `-5` fog ROI, using absolute paths under `MatterEditor/build/validation/atmosphere-presentation/`. Caption path, elevation, exposure, direct ratio/RGB, ambient ratio/modifier, ROI means, generation serial, and presented-frame serial. Record the metrics JSON path and any hardware/driver identity; do not claim acceptance if native RT was unavailable.

- [ ] **Step 10: Commit the independently reviewable status/harness change**

```powershell
git add projects/world_demo/objects/AtmospherePresentationReceiver.js projects/world_demo/worlds/AtmospherePresentationFixture.js MatterEngine3/tools/atmosphere_presentation_metrics.py MatterEngine3/include/matter/props.h MatterEngine3/src/props/props.cpp MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEditor/src/editor_props.h MatterEditor/src/editor_props.cpp MatterEditor/src/property_editor.cpp MatterEditor/src/ui.h MatterEditor/src/main.cpp MatterEditor/src/viewer_commands.h MatterEngine3/tools/atmosphere_cloud_shots.sh MatterEngine3/docs/rendering.md MatterEngine3/tests/props_tests.cpp MatterEngine3/tests/property_editor_tests.cpp MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "test: automate atmosphere presentation acceptance"
```

---

## Handoff to Existing Cloud Tasks 12–14

- [ ] Resume `docs/superpowers/plans/2026-08-09-physical-atmosphere-volumetric-clouds.md` at Task 12 only after all three commits above are green.
- [ ] Task 12 cloud self-shadowing reads `environment.direct_world_sun_ratio.rgb` and post-9SH `environment.sky_irradiance_ambient_ratio.rgb`; it must not restore `pc.sun_color`/`pc.sky_color`.
- [ ] Task 13 cloud/world shadow application attenuates `direct_world_sun_rgb` only. Its planned camera X/Z composite data fits the now-smaller composite push block; do not repack the four environment lighting lanes back into push constants.
- [ ] Task 14 final acceptance retains its existing matrices/timings and additionally runs the `atmosphere-presentation` suite. It references committed status for atmosphere evidence and requested properties only for authoring evidence.

## Final Acceptance Checklist

- [ ] Dedicated sky sampler is linear/repeat-U/clamp-VW; G-buffer remains nearest; `192x108` remains unchanged because all sampling gates pass.
- [ ] Dither table FNV is `0xdc0d948b`, offsets are equal RGB and exactly bounded by plus/minus `0.5/255`, complete interior tiles have zero mean, static frames match, rails clamp, and UNORM/sRGB branches agree in encoded code space.
- [ ] Visible sky, post-9SH irradiance, direct-world sun, and analytic disc use four independent RGB lanes from one committed UBO snapshot.
- [ ] Direct ratios are exactly `90=1`, `45=1`, `5=0.25`, `0=0`, `-5=0`, `-12=0`; noon ambient defaults to `0.25`; twilight remains physical/directional without an artificial deep-night floor.
- [ ] Four property paths, labels, ranges, environment variables, docs, old-state defaults, generic FIFO access, and sanitization match the approved design.
- [ ] Generation and descriptor failure retain the full last-valid atmosphere; concurrent live edits replay exhaustively against it; histories and serials advance only by the exact table.
- [ ] Read-only status reports committed state, while requested `render.lighting.*` continues to report requested sanitized state; a failed elevation request can therefore differ visibly and intentionally.
- [ ] `render_path`, `history_reset`, `wait_frames`, and `shot_now` obey exact grammar and success lines; waiting counts successful presents only.
- [ ] Same-exposure `90/5/0/-5/-12` raster and native-RT captures, fog/ROI metrics, builds, source tests, CPU tests, GPU tests, screenshots, and validation all pass, with native RT unavailable treated as failure.
- [ ] Existing physical-atmosphere plan Tasks 12–14 remain present and unmodified, and this plan's generated output stays out of git and off protected `D:\` storage.
