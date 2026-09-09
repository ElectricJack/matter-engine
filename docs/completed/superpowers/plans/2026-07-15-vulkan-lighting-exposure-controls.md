# Vulkan Lighting and Exposure Controls Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add temporary sun, sky, emission, and exposure controls to MatterViewer and apply a finite ACES-style display transform after Native or DLSS rendering so shadows and indirect bounces remain readable.

**Architecture:** Keep all raster, ray-traced, temporal, denoised, and DLSS resources in linear HDR. Apply source-energy multipliers before lighting integration, invalidate lighting history exactly once when those sources change, then run one display-only exposure/tone-map pass on the selected Native/DLSS image immediately before the UI overlay.

**Tech Stack:** C++17, Vulkan 1.3 dynamic rendering, GLSL 4.60/SPIR-V, ImGui, NVIDIA Streamline 2.12, CUDA 13.3, UCRT64/MinGW, existing Vulkan smoke harness.

## Global Constraints

- Overrides are temporary per-session diagnostic state and never modify manifests or material definitions.
- `sun_multiplier`, `sky_multiplier`, and `emission_multiplier` default to `1.0` and clamp to `[0.0, 4.0]`.
- `exposure_ev` defaults to `-2.0` and clamps to `[-6.0, 6.0]`.
- NaN and infinity sanitize to the corresponding default before reaching shaders or temporal state.
- Source multipliers reset on world switch, world reload, and **Reset to World**; camera and DLSS mode changes preserve them.
- Source-energy changes invalidate lighting history exactly once; exposure-only changes do not invalidate lighting, denoiser, or DLSS history.
- DLSS consumes pre-tone-mapped linear HDR. Tone mapping runs on the selected Native/DLSS output and before the UI overlay.
- Native, DLSS fallback, RT-disabled, and RT-unavailable paths use identical controls and never composite stale RT history.
- Production rendering adds no immediate submission, queue wait, device wait, or synchronous readback.
- Enabled Windows verification uses `HAVE_CUDA=1`, `HAVE_STREAMLINE=1`, `STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0`, and `STREAMLINE_DLL_DIR=/d/SDKs/streamline-sdk-v2.12.0/bin/x64/development`.

---

### Task 1: Define and Sanitize the Lighting Override Contract

**Files:**
- Modify: `MatterEngine3/include/matter/world_session.h`
- Create: `MatterEngine3/src/render/vk_lighting_controls.h`
- Create: `MatterEngine3/src/render/vk_lighting_controls.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`

**Interfaces:**
- Consumes: no new interface; uses `matter::RenderOptions`.
- Produces: `matter::VulkanLightingOverrides`, `viewer::sanitize_vulkan_lighting_overrides`, `viewer::vulkan_exposure_scale`, and `viewer::vulkan_source_lighting_changed` for Tasks 2 and 3.

- [ ] **Step 1: Write failing default, clamp, finite-input, and change-classification tests**

Add the include and assertions to `MatterEngine3/tests/vulkan_smoke_tests.cpp`:

```cpp
#include "render/vk_lighting_controls.h"

static void test_vulkan_lighting_override_contract() {
    matter::VulkanLightingOverrides defaults{};
    CHECK(defaults.sun_multiplier == 1.0f, "sun override defaults to one");
    CHECK(defaults.sky_multiplier == 1.0f, "sky override defaults to one");
    CHECK(defaults.emission_multiplier == 1.0f,
          "emission override defaults to one");
    CHECK(defaults.exposure_ev == -2.0f, "display exposure defaults to -2 EV");

    matter::VulkanLightingOverrides bad{};
    bad.sun_multiplier = -4.0f;
    bad.sky_multiplier = 9.0f;
    bad.emission_multiplier = std::numeric_limits<float>::infinity();
    bad.exposure_ev = std::numeric_limits<float>::quiet_NaN();
    const auto clean = viewer::sanitize_vulkan_lighting_overrides(bad);
    CHECK(clean.sun_multiplier == 0.0f, "sun override clamps low");
    CHECK(clean.sky_multiplier == 4.0f, "sky override clamps high");
    CHECK(clean.emission_multiplier == 1.0f,
          "invalid emission override uses default");
    CHECK(clean.exposure_ev == -2.0f, "invalid exposure uses default");
    CHECK(std::fabs(viewer::vulkan_exposure_scale(-2.0f) - 0.25f) < 1e-6f,
          "-2 EV maps to quarter exposure");

    auto exposure_only = defaults;
    exposure_only.exposure_ev = 1.0f;
    CHECK(!viewer::vulkan_source_lighting_changed(defaults, exposure_only),
          "exposure-only change preserves lighting history");
    auto source_change = defaults;
    source_change.emission_multiplier = 0.5f;
    CHECK(viewer::vulkan_source_lighting_changed(defaults, source_change),
          "emission change invalidates lighting history");
}
```

Call `test_vulkan_lighting_override_contract()` from the smoke test's CPU-test section.

- [ ] **Step 2: Run the strict smoke compile and verify RED**

Run:

```bash
cd MatterViewer
make build/windows/vulkan_smoke_tests.exe HAVE_CUDA=1 HAVE_STREAMLINE=1 \
  STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0 \
  STREAMLINE_DLL_DIR=/d/SDKs/streamline-sdk-v2.12.0/bin/x64/development -j1
```

Expected: compilation fails because `matter::VulkanLightingOverrides` and the three `viewer::` helpers do not exist.

- [ ] **Step 3: Add the public value type and focused sanitizer implementation**

Add to `MatterEngine3/include/matter/world_session.h` before `RenderOptions`:

```cpp
struct VulkanLightingOverrides {
    float sun_multiplier = 1.0f;
    float sky_multiplier = 1.0f;
    float emission_multiplier = 1.0f;
    float exposure_ev = -2.0f;
};
```

Add `VulkanLightingOverrides vulkan_lighting{};` after `vulkan_gi` in `RenderOptions`.

Create `vk_lighting_controls.h`:

```cpp
#pragma once
#include "matter/world_session.h"

namespace viewer {
matter::VulkanLightingOverrides sanitize_vulkan_lighting_overrides(
    const matter::VulkanLightingOverrides& value) noexcept;
float vulkan_exposure_scale(float exposure_ev) noexcept;
bool vulkan_source_lighting_changed(
    const matter::VulkanLightingOverrides& a,
    const matter::VulkanLightingOverrides& b) noexcept;
}
```

Create `vk_lighting_controls.cpp` with finite-before-clamp behavior:

```cpp
#include "vk_lighting_controls.h"
#include <algorithm>
#include <cmath>

namespace viewer {
namespace {
float finite_or(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}
}

matter::VulkanLightingOverrides sanitize_vulkan_lighting_overrides(
    const matter::VulkanLightingOverrides& value) noexcept {
    matter::VulkanLightingOverrides out{};
    out.sun_multiplier = std::clamp(finite_or(value.sun_multiplier, 1.0f), 0.0f, 4.0f);
    out.sky_multiplier = std::clamp(finite_or(value.sky_multiplier, 1.0f), 0.0f, 4.0f);
    out.emission_multiplier =
        std::clamp(finite_or(value.emission_multiplier, 1.0f), 0.0f, 4.0f);
    out.exposure_ev = std::clamp(finite_or(value.exposure_ev, -2.0f), -6.0f, 6.0f);
    return out;
}

float vulkan_exposure_scale(float exposure_ev) noexcept {
    const auto clean = sanitize_vulkan_lighting_overrides(
        {1.0f, 1.0f, 1.0f, exposure_ev});
    return std::exp2(clean.exposure_ev);
}

bool vulkan_source_lighting_changed(
    const matter::VulkanLightingOverrides& a,
    const matter::VulkanLightingOverrides& b) noexcept {
    const auto x = sanitize_vulkan_lighting_overrides(a);
    const auto y = sanitize_vulkan_lighting_overrides(b);
    return x.sun_multiplier != y.sun_multiplier ||
           x.sky_multiplier != y.sky_multiplier ||
           x.emission_multiplier != y.emission_multiplier;
}
}
```

Add `src/render/vk_lighting_controls.cpp` to the MatterEngine3 and MatterViewer Windows source lists.

- [ ] **Step 4: Rebuild and run the CPU contract GREEN**

Run the Step 2 command, then:

```powershell
$env:MATTER_VK_SMOKE_MODE='raster'
MatterViewer\build\windows\vulkan_smoke_tests.exe
```

Expected: `ALL PASS`, `validation errors: 0`, including the four lighting-contract groups.

- [ ] **Step 5: Commit Task 1**

```bash
git add MatterEngine3/include/matter/world_session.h \
  MatterEngine3/src/render/vk_lighting_controls.h \
  MatterEngine3/src/render/vk_lighting_controls.cpp \
  MatterEngine3/tests/vulkan_smoke_tests.cpp \
  MatterEngine3/Makefile MatterViewer/Makefile
git commit -m "feat(vulkan): define lighting override contract"
```

---

### Task 2: Apply Source Multipliers and Reset Lighting History Exactly Once

**Files:**
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/shaders_vk/composite.frag`
- Modify: `MatterEngine3/shaders_vk/rt_lighting.rgen`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: `matter::RenderOptions::vulkan_lighting` and Task 1 sanitizer/change classifier.
- Produces: sanitized `VkSceneLighting::emission_multiplier`, scaled sun/sky colors, and `VkSceneRenderer::set_lighting` exact-once history invalidation for Task 3.

- [ ] **Step 1: Write failing isolated primary/secondary energy and reset-sequence tests**

Extend the existing Vulkan material/GI GPU fixture with controlled captures:

```cpp
const auto base = renderer.test_read_composite_pixel(receiver_x, receiver_y, error);

matter::VulkanLightingOverrides isolated{};
isolated.sun_multiplier = 0.0f;
isolated.sky_multiplier = 0.0f;
isolated.emission_multiplier = 0.0f;
renderer.test_set_lighting_overrides(isolated);
const auto dark = renderer.test_read_composite_pixel(receiver_x, receiver_y, error);
CHECK(luminance(dark.hdr) < luminance(base.hdr),
      "zero source multipliers remove authored lighting energy");

isolated.emission_multiplier = 0.5f;
renderer.test_set_lighting_overrides(isolated);
const auto half_emission = renderer.test_read_composite_pixel(emitter_x, emitter_y, error);
isolated.emission_multiplier = 1.0f;
renderer.test_set_lighting_overrides(isolated);
const auto full_emission = renderer.test_read_composite_pixel(emitter_x, emitter_y, error);
CHECK(relative_error(luminance(full_emission.hdr),
                     2.0f * luminance(half_emission.hdr)) < 0.05f,
      "primary emission follows authored multiplier");
CHECK(relative_error(luminance(full_emission.raw_diffuse),
                     2.0f * luminance(half_emission.raw_diffuse)) < 0.10f,
      "secondary emissive bounce follows authored multiplier");
```

Add a presented-frame sequence that records `test_gi_history_reset_count()`:

```cpp
const uint64_t before = renderer.test_gi_history_reset_count();
renderer.set_lighting(changed_source);
render_and_present(renderer, frame_a);
CHECK(renderer.test_gi_history_reset_count() == before + 1,
      "source change resets lighting history once");
render_and_present(renderer, frame_b);
CHECK(renderer.test_gi_history_reset_count() == before + 1,
      "stable source does not repeat reset");
renderer.set_display_exposure_for_test(1.0f);
render_and_present(renderer, frame_c);
CHECK(renderer.test_gi_history_reset_count() == before + 1,
      "exposure-only change preserves lighting history");
```

- [ ] **Step 2: Run the RT smoke and verify RED**

Run:

```powershell
$env:MATTER_VK_SMOKE_MODE='rt'
MatterViewer\build\windows\vulkan_smoke_tests.exe
```

Expected: compile/link failure for the new test seam or assertion failure because primary and secondary emission ignore the override and source changes do not request a GI reset.

- [ ] **Step 3: Extend the lighting push-constant ABI without reusing debug fields**

Replace the padding names in `VkSceneLighting` with an explicit 64-byte layout:

```cpp
struct VkSceneLighting {
    matter::Float3 sun_direction{-0.45f, -0.80f, -0.35f};
    float sun_intensity = 1.0f;
    matter::Float3 sun_color{2.2f, 2.05f, 1.8f};
    float diffuse_rt_multiplier = 0.0f;
    matter::Float3 sky_color{0.38f, 0.43f, 0.52f};
    float emission_multiplier = 1.0f;
    float debug_view = 0.0f;
    float pad0 = 0.0f;
    float pad1 = 0.0f;
    float pad2 = 0.0f;
};
static_assert(sizeof(VkSceneLighting) == 64);
```

Mirror that order in `composite.frag`. Multiply decoded primary emission by
`lighting.emission_multiplier`. Add `emission_multiplier` to the RT lighting
push constants and multiply the emission term in `hit_radiance` before it can
contribute at secondary hits.

- [ ] **Step 4: Sanitize options, apply source values, and request one history reset**

In the Vulkan render path of `matter_engine.cpp`:

```cpp
const auto controls =
    viewer::sanitize_vulkan_lighting_overrides(opts.vulkan_lighting);
lighting.sun_color = {
    manifest.lights.sun_color[0] * controls.sun_multiplier,
    manifest.lights.sun_color[1] * controls.sun_multiplier,
    manifest.lights.sun_color[2] * controls.sun_multiplier};
lighting.sky_color = {
    manifest.lights.sky_color[0] * controls.sky_multiplier,
    manifest.lights.sky_color[1] * controls.sky_multiplier,
    manifest.lights.sky_color[2] * controls.sky_multiplier};
lighting.emission_multiplier = controls.emission_multiplier;
vk_scene->set_lighting(lighting);
vk_scene->set_display_exposure(controls.exposure_ev);
```

Move `set_lighting` out of the header. Compare sanitized source fields against
the previous value; set `gi_history_reset_pending_ = true` only when they differ.
The existing successful-present candidate logic consumes and counts that reset.
The first lighting assignment after renderer initialization establishes a
baseline rather than manufacturing an extra reset.

- [ ] **Step 5: Verify GPU energy, exact-once reset, and all fallback modes**

Run:

```powershell
foreach ($mode in @('rt','rt-unavailable','rt-disabled','raster','default')) {
  $env:MATTER_VK_SMOKE_MODE=$mode
  & MatterViewer\build\windows\vulkan_smoke_tests.exe
  if ($LASTEXITCODE -ne 0) { throw "mode failed: $mode" }
}
```

Expected for every mode: `ALL PASS`, `validation errors: 0`. The RT mode prints
the primary/secondary emission ratio and one-reset/stable-frame evidence.

- [ ] **Step 6: Commit Task 2**

```bash
git add MatterEngine3/src/matter_engine.cpp \
  MatterEngine3/src/render/vk_scene_renderer.h \
  MatterEngine3/src/render/vk_scene_renderer.cpp \
  MatterEngine3/shaders_vk/composite.frag \
  MatterEngine3/shaders_vk/rt_lighting.rgen \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(vulkan): apply runtime lighting multipliers"
```

---

### Task 3: Tone Map the Selected Native/DLSS Output and Expose Viewer Controls

**Files:**
- Create: `MatterEngine3/shaders_vk/display_transform.frag`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Modify: `MatterViewer/ui.h`
- Modify: `MatterViewer/ui.cpp`
- Modify: `MatterViewer/main.cpp`
- Modify: `MatterViewer/tools/smoke_vulkan_viewer.ps1`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: Task 1 `exposure_ev`, Task 2 linear-HDR `hdr_`, and per-frame `dlss_output`.
- Produces: `VkSceneRenderer::set_display_exposure(float)`, a per-frame display descriptor, final post-DLSS SDR swapchain color, and live Viewer lighting controls.

- [ ] **Step 1: Write failing display-order, curve, UI-reset, and fallback tests**

Add CPU reference helpers to the smoke test:

```cpp
static matter::Float3 aces_reference(matter::Float3 hdr, float exposure_ev) {
    const float scale = std::exp2(exposure_ev);
    const auto map = [scale](float value) {
        const float x = std::max(value * scale, 0.0f);
        return std::clamp((x * (2.51f * x + 0.03f)) /
                          (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
    };
    return {map(hdr.x), map(hdr.y), map(hdr.z)};
}
```

Use the real GPU display path with constant HDR values `0`, `1`, `5`, and
`65504`; read back the swapchain and compare to the reference within one UNORM
code value plus conversion tolerance. Assert finite `[0,1]` output and monotonic
response at `-2`, `0`, and `+2 EV`.

Instrument the fake Streamline evaluator to write a distinct constant HDR color.
Assert the displayed color matches tone mapping of that DLSS output, while the
captured DLSS input remains the original pre-tone-mapped `hdr_` value.

Add a viewer control-state unit/static fixture:

```cpp
viewer::ViewerStats stats{};
stats.lighting.sun_multiplier = 0.25f;
stats.lighting.exposure_ev = 3.0f;
viewer::reset_lighting_controls(stats);
CHECK(stats.lighting.sun_multiplier == 1.0f,
      "world reset restores authored sun multiplier");
CHECK(stats.lighting.exposure_ev == -2.0f,
      "world reset restores default display exposure");
```

Exercise that same helper through the reload and successful-world-switch test
seams, with non-default values installed before each transition. Assert all
four fields return to defaults, a failed world switch preserves the current
values, and neither successful transition produces a second source-history
reset on the first presented frame.

- [ ] **Step 2: Verify RED**

Run the strict build from Task 1 and the `default` smoke mode.

Expected: missing `display_transform.frag`, display pipeline APIs, per-frame
descriptor, and `ViewerStats::lighting`/`reset_lighting_controls`.

- [ ] **Step 3: Add the post-DLSS display shader and SPIR-V build inputs**

Create `display_transform.frag`:

```glsl
#version 460
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D linear_hdr_texture;
layout(push_constant) uniform DisplaySettings { float exposure_ev; } display;

vec3 aces_sdr(vec3 value) {
    vec3 x = max(value * exp2(clamp(display.exposure_ev, -6.0, 6.0)), vec3(0.0));
    vec3 numerator = x * (2.51 * x + 0.03);
    vec3 denominator = x * (2.43 * x + 0.59) + 0.14;
    return clamp(numerator / max(denominator, vec3(1e-6)), 0.0, 1.0);
}

void main() {
    vec3 hdr = texture(linear_hdr_texture, in_uv).rgb;
    hdr = vec3(isnan(hdr.x) || isinf(hdr.x) ? 0.0 : hdr.x,
               isnan(hdr.y) || isinf(hdr.y) ? 0.0 : hdr.y,
               isnan(hdr.z) || isinf(hdr.z) ? 0.0 : hdr.z);
    out_color = vec4(aces_sdr(hdr), 1.0);
}
```

Add `display_transform.frag.spv` to both Vulkan `VK_SPV` lists so the generated
embedded SPIR-V header contains it.

- [ ] **Step 4: Replace the transfer blit with a frame-safe display graphics pass**

Create a descriptor-set layout with one combined image sampler, a pipeline
layout with a fragment push constant containing `exposure_ev`, and a dynamic-
rendering graphics pipeline using `composite.vert` plus
`display_transform.frag`. Add one `display_descriptor_set` to each
`FrameResources` slot.

In `record_composite_to_swapchain`, keep the Native/DLSS source selection
unchanged, then:

```cpp
record_image_transition(command_buffer, *composite_source,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        composite_source_stage, composite_source_access,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
update_display_descriptor(frame_slot.display_descriptor_set,
                          composite_source->view);
begin_swapchain_dynamic_rendering(VK_ATTACHMENT_LOAD_OP_CLEAR);
vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                  display_pipeline_);
vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        display_pipeline_layout_, 0, 1,
                        &frame_slot.display_descriptor_set, 0, nullptr);
vkCmdPushConstants(command_buffer, display_pipeline_layout_,
                   VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float),
                   &display_exposure_ev_);
vkCmdDraw(command_buffer, 3, 1, 0, 0);
vkCmdEndRendering(command_buffer);
```

Do not update another frame slot's descriptor. Retain the chosen source
lifetime on the acquired frame. Leave the swapchain image in
`VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` for the existing ImGui pass.
Remove the direct HDR-to-swapchain `vkCmdBlitImage` path.

- [ ] **Step 5: Wire the Viewer UI and world-reset behavior**

In `ViewerStats`, replace the unused sun override fields with:

```cpp
matter::VulkanLightingOverrides lighting{};
```

Expose a free helper in `ui.h`/`ui.cpp`:

```cpp
void reset_lighting_controls(ViewerStats& stats) {
    stats.lighting = matter::VulkanLightingOverrides{};
}
```

Add the Lighting section to `draw_debug_panel`:

```cpp
ImGui::SeparatorText("Lighting");
ImGui::SliderFloat("Exposure (EV)", &s.lighting.exposure_ev, -6.0f, 6.0f, "%.2f");
ImGui::SliderFloat("Sun", &s.lighting.sun_multiplier, 0.0f, 4.0f, "%.2f");
ImGui::SliderFloat("Sky", &s.lighting.sky_multiplier, 0.0f, 4.0f, "%.2f");
ImGui::SliderFloat("Emission", &s.lighting.emission_multiplier, 0.0f, 4.0f, "%.2f");
if (ImGui::Button("Reset to World")) reset_lighting_controls(s);
```

Copy `stats.lighting` into `options.vulkan_lighting` before `session->render`.
Call `reset_lighting_controls(stats)` immediately before `session->reload()` and
immediately after a successful world switch. Do not reset on camera or DLSS
mode changes.

- [ ] **Step 6: Run full enabled validation and visual acceptance**

Run:

```bash
cd MatterViewer
make windows HAVE_CUDA=1 HAVE_STREAMLINE=1 \
  STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0 \
  STREAMLINE_DLL_DIR=/d/SDKs/streamline-sdk-v2.12.0/bin/x64/development -j2
make vulkan-smoke HAVE_CUDA=1 HAVE_STREAMLINE=1 \
  STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0 \
  STREAMLINE_DLL_DIR=/d/SDKs/streamline-sdk-v2.12.0/bin/x64/development -j1
```

Then run `MatterViewer/tools/smoke_vulkan_viewer.ps1` with its configured
validation-layer path. Expected: Native, resize, RT-disabled, and fake/real-DLSS
cases pass with `validation errors 0`; screenshots contain visible CornellBox
shadow separation and a non-clipped ceiling-emitter shape at `-2 EV`.

Manually launch CornellBox and confirm Sun `0` removes sun only, Emission `0`
removes the panel and emissive bounce, Sky `0` removes environment fill, and
Exposure changes display brightness without temporal convergence or reset.

- [ ] **Step 7: Commit Task 3**

```bash
git add MatterEngine3/shaders_vk/display_transform.frag \
  MatterEngine3/src/render/vk_scene_renderer.h \
  MatterEngine3/src/render/vk_scene_renderer.cpp \
  MatterEngine3/Makefile MatterViewer/Makefile \
  MatterViewer/ui.h MatterViewer/ui.cpp MatterViewer/main.cpp \
  MatterViewer/tools/smoke_vulkan_viewer.ps1 \
  MatterEngine3/tests/vulkan_smoke_tests.cpp
git commit -m "feat(viewer): add post-DLSS exposure controls"
```

---

## Final Acceptance Gate

- [ ] Generate a review package from the pre-plan base through Task 3 and request a fresh whole-plan review.
- [ ] Fix every Critical and Important finding, then re-review until approved.
- [ ] Run strict SPIR-V, all Vulkan smoke modes, full `HAVE_CUDA=1 HAVE_STREAMLINE=1` Windows build, and the bounded viewer screenshot suite.
- [ ] Launch the approved viewer with `VK_LAYER_PATH` configured and CornellBox selected for user sign-off.
