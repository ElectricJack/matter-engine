# Retain Volumetric History Across Atmosphere Updates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop atmosphere LUT and lighting commits from clearing volumetric temporal history so moving clouds converge smoothly instead of flashing a Bayer dot pattern.

**Architecture:** Narrow the pure atmosphere history-decision policy while leaving froxel allocation, cloud-shape, and scatter-setting invalidation untouched. Pin the behavior in the existing atmosphere CPU tests, then verify the unchanged renderer and real Vulkan paths.

**Tech Stack:** C++17, Vulkan, existing MatterEngine3 test executables and shader/SPIR-V gates.

## Global Constraints

- Do not change froxel resource or authored cloud-shape invalidation.
- Do not change diffuse-GI or reflection-history behavior.
- Do not change temporal blend defaults or Bayer scheduling.
- Production change is limited to the atmosphere history-decision helper.

---

### Task 1: Retain volumetric history across atmosphere commits

**Files:**
- Modify: `MatterEngine3/tests/atmosphere_tests.cpp`
- Modify: `MatterEngine3/include/matter/atmosphere_lighting.h`

**Interfaces:**
- Consumes: `matter::atmosphere_history_decision(uint32_t, bool)`
- Produces: the same API with `reset_volumetric == false` for atmosphere-only decisions

- [ ] **Step 1: Write the failing policy regression**

Extend `test_atmosphere_history_decisions_are_narrow()` so full commits,
direct/irradiance changes, and emission/disc changes require the existing GI
and reflection resets but assert `!reset_volumetric`.

- [ ] **Step 2: Run the focused test and verify RED**

Run: `make -C MatterEngine3/tests run-atmosphere CC=/ucrt64/bin/g++`

Expected: the new retained-volumetric-history assertions fail because the
helper currently sets `reset_volumetric`.

- [ ] **Step 3: Implement the minimal policy change**

Remove only the `result.reset_volumetric = true` assignments from
`atmosphere_history_decision`. Preserve every diffuse-GI and reflection-miss
assignment.

- [ ] **Step 4: Run focused and integration verification**

Run:

```text
make -C MatterEngine3/tests run-atmosphere CC=/ucrt64/bin/g++
make -C MatterEngine3/tests run-shader-source CC=/ucrt64/bin/g++
make -C MatterEngine3/tests run-vk-scene-renderer CC=/ucrt64/bin/g++
make -C MatterEngine3 vulkan-spirv
MATTER_VK_SMOKE_MODE=atmosphere MatterEditor/build/windows/vulkan_smoke_tests.exe
```

Expected: all focused tests pass; Vulkan smoke reports `ALL PASS` and
`validation errors: 0`.

- [ ] **Step 5: Build the editor and commit**

Run the full MatterEditor Windows build, stage only the two source/test files
plus these two docs, and commit with `fix: retain volumetric history across atmosphere updates`.

