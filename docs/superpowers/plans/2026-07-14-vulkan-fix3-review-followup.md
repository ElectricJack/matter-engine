# Vulkan fix3 Review Follow-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve exact pre-existing packed material slot state across Vulkan diagnostics and make emission/sky-light GPU tests isolate one variable at a time.

**Architecture:** Keep the diagnostic state guard local to `matter_engine.cpp`, using the registry's public pack and setter APIs for snapshot and restoration. Extend the existing actual-GPU raster smoke so each lighting comparison reinstalls identical emission-5 scene data and the saturated emission case has a strict numeric contract.

**Tech Stack:** C++17, C material registry API, Vulkan, GLSL, PowerShell runtime smoke, GNU Make/MSYS2.

## Global Constraints

- Do not modify, stage, or commit `MatterEngine3/src/script_host.cpp`.
- Prior packed slot 11 must be finite, integral, and in `[-1, 3]`.
- The runtime smoke seeds prior slot 2 and asserts the exact packed post-state.
- Saturated HDR red must be finite, strictly above emission 1000, and between 14000 and 16000.
- Dark and bright sky samples use identical emission-5 geometry and material.

---

### Task 1: Exact diagnostic registry restoration

**Files:**
- Modify: `MatterViewer/tools/check_vulkan_viewer.ps1`
- Modify: `MatterViewer/tools/smoke_vulkan_viewer.ps1`
- Modify: `MatterEngine3/src/matter_engine.cpp`

**Interfaces:**
- Consumes: `MaterialRegistryPackForGPU(float*)` and `MaterialRegistrySetGroundTilesetSlot(int, int)`.
- Produces: `MATTER_VK_DIAGNOSTIC_GROUND_TILESET_PRIOR_SLOT` diagnostic input and an exact packed-restoration log.

- [ ] **Step 1: Write failing source/runtime assertions**

Require the prior-slot environment variable, seed log, and exact packed restoration log in the source gate and override smoke.

- [ ] **Step 2: Run the source gate to verify RED**

Run: `MatterViewer/tools/check_vulkan_viewer.ps1`

Expected: FAIL because the prior-slot contract and restoration log do not exist.

- [ ] **Step 3: Implement minimal snapshot and restoration**

Pack before applying slot 0, validate slot 11, retain its integer value, restore it in the destructor, repack, and log the exact observed post-state. When the prior-slot diagnostic is set, validate and seed it before snapshotting.

- [ ] **Step 4: Run the source gate to verify GREEN**

Run: `MatterViewer/tools/check_vulkan_viewer.ps1`

Expected: `vulkan-viewer gate: PASS`.

### Task 2: Isolated sky-light and strict saturation GPU regression

**Files:**
- Modify: `MatterEngine3/tests/vulkan_smoke_tests.cpp`

**Interfaces:**
- Consumes: `known_raster_triangle(hash, emission)`, renderer part lifecycle, and `kVkMaxEncodedEmission`.
- Produces: strict saturated HDR and identical-scene sky-light assertions.

- [ ] **Step 1: Write failing raster assertions**

Require saturated red HDR to satisfy `max > thousand`, `max > 14000`, and `max < 16000`. Before bright sky, require successful release/reinstall of `known_raster_triangle(901, 5.0f)`, instance upload, and culling.

- [ ] **Step 2: Run the current raster binary to capture the pre-fix evidence**

Run with `MATTER_VK_SMOKE_MODE=raster`; record that the existing binary compares bright sky against the still-installed saturated triangle and lacks the strict source contract.

- [ ] **Step 3: Implement the minimal scene reset**

Restore emission-5 part 901 immediately before bright lighting, then render and compare the bright result with the earlier emission-5 dark result.

- [ ] **Step 4: Rebuild and run strict GPU verification**

Run: `make -C MatterViewer vulkan-smoke HAVE_CUDA=1` using the configured MSYS2/CUDA/OptiX environment, then execute default and raster modes.

Expected: `ALL PASS`, validation errors 0, and finite monotonic emission output within the saturation band.

### Task 3: Runtime integration, review, and commit

**Files:**
- Modify: `docs/superpowers/specs/2026-07-14-vulkan-fix3-review-followup-design.md`
- Modify: `docs/superpowers/plans/2026-07-14-vulkan-fix3-review-followup.md`
- Update unstaged report: `.superpowers/sdd/vulkan-task-9-fix3-report.md`

- [ ] **Step 1: Run full runtime smoke**

Run the clean-cache Vulkan viewer smoke and require the texture warning, prior-slot-2 seed log, and exact packed slot-2 restoration log.

- [ ] **Step 2: Review staged scope**

Run `git diff --check`, inspect staged names and hunks, and verify `MatterEngine3/src/script_host.cpp` remains unstaged and unchanged by this follow-up.

- [ ] **Step 3: Commit scoped follow-up**

Stage only the files listed in this plan and commit with `fix(vulkan): preserve diagnostic render state`.

- [ ] **Step 4: Update the unstaged report**

Record the follow-up commit SHA, commands, validation count, emission values, exact restoration evidence, and remaining ScriptHost contingency.
