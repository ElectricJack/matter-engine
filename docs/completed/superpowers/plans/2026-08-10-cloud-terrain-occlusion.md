# Cloud Terrain Occlusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the existing volumetric TLAS shadow ray to the 3 km froxel range only when an enhanced froxel contains cloud extinction, allowing mountains to shadow clouds without adding a ray or pass.

**Architecture:** Hoist enhanced cloud-density sampling ahead of the existing ray query, sanitize it once, and select either the existing 300 m fog reach or `VOL_FROXEL_FAR`. Preserve the single shared visibility term and all existing lighting/resource behavior. Prove the policy first with source tests and then with a deterministic RTX fixture whose occluder lies beyond 300 m.

**Tech Stack:** C++20 tests, GLSL 460 with `GL_EXT_ray_query`, Vulkan/SPIR-V, GNU Make, MSYS2 UCRT64, NVIDIA RTX validation fixture.

## Global Constraints

- Launch at most one TLAS shadow ray per actively updated froxel.
- Use a 3 km maximum only when enhanced sanitized cloud extinction exceeds `1e-6 m^-1`.
- Fog-only and Current-cost froxels retain the 300 m maximum.
- Sky irradiance ambient remains independent of geometry visibility.
- Do not add a render pass, persistent GPU resource, renderer API, or history invalidation.
- Keep unrelated main-worktree edits untouched.

---

### Task 1: Lock the dynamic ray-reach contract with failing tests

**Files:**
- Modify: `MatterEngine3/tests/shader_source_tests.cpp`
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: existing `vol_scatter.comp` source and `run_rt_froxel_resize_smoke`.
- Produces: source assertions for the exact long/short policy and a real-Vulkan regression that cannot pass with the old 300 m constant.

- [ ] **Step 1: Read the test-quality rules**

Run:

```powershell
Get-Content -Raw C:\Users\webde\.codex\plugins\cache\openai-curated-remote\superpowers\6.2.0\skills\test-driven-development\writing-good-tests.md
```

- [ ] **Step 2: Add the failing source contract**

Extend the existing Task 12 shader assertions to require:

```cpp
assert(volume_common.find(
           "const float VOL_CLOUD_TERRAIN_SHADOW_FAR = VOL_FROXEL_FAR") !=
       std::string::npos);
assert(volume.find("cloud_extinction > 1e-6") != std::string::npos);
assert(volume.find("VOL_CLOUD_TERRAIN_SHADOW_FAR") != std::string::npos);
```

Use the file's existing counting idiom to assert exactly one `rayQueryInitializeEXT`. Assert that cloud extinction is sampled before that call and reused later for `cloud_scattering`.

- [ ] **Step 3: Run the source target and capture RED**

```bash
make -C MatterEngine3/tests run-shader-source CC=/ucrt64/bin/g++
```

Expected: failure on the missing cloud-terrain constant/order contract.

- [ ] **Step 4: Add the deterministic real-Vulkan failing fixture**

In `run_rt_froxel_resize_smoke`, after TLAS warm-up:

1. Choose center froxel `(80,45,92)` using `task9_froxel_world_position`.
2. Configure a fully covered enhanced cloud layer spanning that point; set temporal blend 0, local march steps 0, orders 1, strength 0, powder 0, and cloud shadows disabled. Set fixture sky irradiance to zero so readback isolates direct cloud sunlight.
3. Choose a normalized near-horizontal `to_sun`. Create real opaque TLAS geometry as a two-triangle quad centered at `froxel_world + to_sun * 600 m`, perpendicular to `to_sun`, with 100 m half-extent. Its whole plane is beyond 300 m and inside 3 km.
4. Capture the same scatter voxel after four production frames with the quad omitted, then included through `update_instances`.
5. Assert clear direct luminance is finite and positive, occluded direct luminance is below 25% of clear, and alpha/extinction agrees within half-float tolerance. Restore original instances.

Do not add a shader override or mock ray result.

- [ ] **Step 5: Build and run the GPU target to capture RED**

From `MatterEditor` under UCRT64:

```bash
make build/windows/vulkan_smoke_tests.exe WIN_CXX=/ucrt64/bin/g++.exe
MATTER_VK_SMOKE_MODE=froxel-resize ./build/windows/vulkan_smoke_tests.exe
```

Expected: the distant-occluder assertion fails while Vulkan validation remains zero.

---

### Task 2: Implement the single-ray dynamic reach

**Files:**
- Modify: `MatterEngine3/shaders_vk/vol_common.glsl`
- Modify: `MatterEngine3/shaders_vk/vol_scatter.comp`
- Regenerate: `MatterEngine3/src/render/vk_embedded_spirv.h`

**Interfaces:**
- Consumes: `VOL_FROXEL_FAR`, `VOL_SHADOW_FAR`, `ENHANCED_CLOUD_LIGHTING`, and `vol_cloud_density`.
- Produces: `VOL_CLOUD_TERRAIN_SHADOW_FAR` and one ray query whose `tmax` is selected from sanitized cloud extinction.

- [ ] **Step 1: Define the explicit cloud-terrain reach**

Add to `vol_common.glsl`:

```glsl
const float VOL_CLOUD_TERRAIN_SHADOW_FAR = VOL_FROXEL_FAR;
```

Keep the short constant and its fog/empty-media performance comment.

- [ ] **Step 2: Hoist and sanitize cloud extinction**

Before `rayQueryInitializeEXT` in `vol_scatter.comp`, add:

```glsl
float cloud_extinction = 0.0;
if (ENHANCED_CLOUD_LIGHTING) {
    cloud_extinction = texture(vol_cloud_density, uvw).r;
    if (isnan(cloud_extinction) || isinf(cloud_extinction))
        cloud_extinction = 0.0;
    cloud_extinction = clamp(cloud_extinction, 0.0, extinction);
}
float shadow_far = cloud_extinction > 1e-6
    ? VOL_CLOUD_TERRAIN_SHADOW_FAR
    : VOL_SHADOW_FAR;
```

Use `shadow_far` as the existing ray query's `tmax`. Remove the later duplicate density sample and reuse `cloud_extinction` for `cloud_scattering`. Do not add a ray query.

- [ ] **Step 3: Run source GREEN**

```bash
make -C MatterEngine3/tests run-shader-source CC=/ucrt64/bin/g++
```

Expected: all assertions pass.

- [ ] **Step 4: Regenerate SPIR-V**

```bash
make -C MatterEngine3 vulkan-spirv CC=/ucrt64/bin/g++
```

Expected: `vol_scatter.comp` compiles and the embedded module is refreshed.

- [ ] **Step 5: Run real-Vulkan GREEN**

```bash
make -C MatterEditor build/windows/vulkan_smoke_tests.exe WIN_CXX=/ucrt64/bin/g++.exe
cd MatterEditor
MATTER_VK_SMOKE_MODE=froxel-resize ./build/windows/vulkan_smoke_tests.exe
```

Expected: distant-quad luma is below 25% of clear, extinction is stable, all existing cases pass, and validation errors are zero.

- [ ] **Step 6: Commit the implementation**

Stage only the two shader sources, generated embedded header, and two focused tests. Run `git diff --cached --check`, then commit:

```bash
git commit -m "feat: let terrain shadow volumetric clouds"
```

---

### Task 3: Verify and integrate

**Files:**
- Verify only.

**Interfaces:**
- Consumes: Task 2's committed shader and regression coverage.
- Produces: a verified feature commit integrated into local `main` without touching unrelated dirty files.

- [ ] **Step 1: Run final gates sequentially**

```bash
make -C MatterEngine3/tests run-shader-source CC=/ucrt64/bin/g++
make -C MatterEngine3 vulkan-spirv CC=/ucrt64/bin/g++
make -C MatterEditor build/windows/vulkan_smoke_tests.exe WIN_CXX=/ucrt64/bin/g++.exe
cd MatterEditor
MATTER_VK_SMOKE_MODE=froxel-resize ./build/windows/vulkan_smoke_tests.exe
```

Require fresh exit zero, `ALL PASS`, and zero validation errors.

- [ ] **Step 2: Audit scope**

```bash
git diff HEAD^ --check
git show --stat --oneline HEAD
git status --short
```

Verify exactly one `rayQueryInitializeEXT`, no new renderer resource/API, and only approved files changed.

- [ ] **Step 3: Fast-forward local main**

Confirm the primary checkout still contains only its known unrelated user edits, then fast-forward `main` to `codex/cloud-terrain-occlusion`. Do not stage, restore, or rewrite user files.

- [ ] **Step 4: Verify integrated identity**

Run the focused source test from local main. Confirm `git branch --show-current` prints `main`, the implementation commit is at the tip, and the pre-existing dirty files remain unstaged.
