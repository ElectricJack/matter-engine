# Task 8 Report: build-all.sh + doc pass

## Summary

Task 8 is complete. Integrated new CPU tests (run-tilesetgtex, run-tilesettorusbvh) into the MatterEngine3 test sweep and added a guarded GPU test block to build-all.sh. All existing tests pass, GPU tests skip gracefully when GL 4.6 is unavailable.

## Changes

1. **build-all.sh**
   - Added `run-tilesetgtex` and `run-tilesettorusbvh` to the MatterEngine3 CPU test list
   - Added a guarded GPU test block that:
     - Checks for GL 4.6 availability via GALLIUM_DRIVER=d3d12 env var OR glxinfo detection
     - Runs `run-tilesetgpu` and `run-tilesetseam` if GL 4.6 is available
     - Prints SKIP message otherwise

2. **MatterEngine3/tests/Makefile**
   - Added `tileset_bake_gpu_stub.cpp` to TILESETBAKE_CPP to fix linker error
   - This stub satisfies the undefined reference to bake_tileset_gpu for headless builds

3. **MatterEngine3/tests/tileset_bake_gpu_stub.cpp** (new file)
   - Minimal stub implementation that returns an error (tests don't use GPU path)
   - Allows headless test suite to link despite tileset_phase.cpp having GPU overload

## Verification

All tileset test suites pass individually:
- run-tilesetdsl ✓
- run-tilesetplacement ✓
- run-tilesetbake ✓
- run-tilesetphysics ✓
- run-tilesetcore ✓
- run-tilesetgtex ✓ (new)
- run-tilesettorusbvh ✓ (new)

GPU test detection logic verified:
- With GALLIUM_DRIVER=d3d12: GPU tests enabled
- Without: GPU tests skipped with informative message

## Commit

SHA: d92c1c3
Message: "phase 3: tileset GPU bake + .gtex complete"

Files changed:
- build-all.sh (2 insertions: CPU test list, GPU test block)
- MatterEngine3/tests/Makefile (1 insertion: stub linking)
- MatterEngine3/tests/tileset_bake_gpu_stub.cpp (new, 22 lines)

## Notes

The fix in MatterEngine3/tests/tileset_bake_gpu_stub.cpp was necessary because Task 6 added a GPU-based overload to run_tileset_phase() that references bake_tileset_gpu, but the headless test Makefile didn't link the GPU code (tileset_bake_gpu.cpp requires GL headers). This stub allows the test to link without actually using the GPU path (which tests don't call).

## Phase 3 final-review fixes

### I1 — Material table divergence
**Files**: `MatterEngine3/src/tileset_bake_primary.cpp` (lines 68-95), `MatterEngine3/src/tileset_bake_ao.cpp` (lines 63-76, 181-197)

Unified both bake drivers to the canonical `MaterialRegistryPackForGPU` slot layout:
`[0..2]=albedo, [3]=roughness, [4]=metallic, [5]=emission, [6]=pad, [7]=translucency, [8]=ior, [9]=flatShading, [10]=mergeGroup, [11]=pad`.

Primary bake (`tileset_bake_primary.cpp`) now also uploads the `materialTable` uniform
so `getMaterialProperties` in `materials.glsl` reads real values instead of zero-initialized
defaults.

AO bake (`tileset_bake_ao.cpp`) slot order was fixed but `materialTable` upload intentionally
omitted: the AO pass uses normals only; uploading the real table changes per-tile AO output in
a way that breaks Wang-tile seam invariance for the AO channel (each tile's unique geometry
produces different boundary-strip occlusion). The no-materialTable path retains the pre-existing
behaviour where `getMaterialProperties(id)` with `id<materialCount` reads the correct SSBO and
the BVH shader's zero-normal fallback handles the base mesh safely.

Added CPU-side pack assertion in `MatterEngine3/viewer/tileset_gpu_tests.cpp`
`test_material_pack_layout()`: fixture with flatShading=1, mergeGroup=42, translucency=0.5,
ior=1.7 verifies r[7]=0.5, r[8]=1.7, r[9]=1.0, r[10]=42.0 exactly.

### I2 — GPU stub split
**Files**: `MatterEngine3/src/tileset_phase.cpp`, `MatterEngine3/src/tileset_phase_gpu.cpp` (new),
`MatterEngine3/tests/Makefile`, `MatterEngine3/tests/tileset_bake_gpu_stub.cpp` (deleted)

Moved the `run_tileset_phase` opts overload (which calls `bake_tileset_gpu`) to new file
`tileset_phase_gpu.cpp`, not included in `libmatter_engine3.a`. The headless lib no longer
references `bake_tileset_gpu` through `tileset_phase.cpp`, so the stub is deleted.
`tileset_bake_gpu_stub.cpp` removed from `TILESETBAKE_CPP` in `tests/Makefile`.

### M1 — Document TILESET_GTEX_USE_RAYLIB_STB contract
**File**: `MatterEngine3/include/tileset_gtex.h` (header comment, +6 lines)

Added 5-line block comment explaining the `TILESET_GTEX_USE_RAYLIB_STB` macro contract.

### M2 — Replace typedef GLuint with uint32_t
**Files**: `MatterEngine3/include/tileset_bake_primary.h`, `MatterEngine3/include/tileset_bake_ao.h`,
`MatterEngine3/src/tileset_bake_primary.cpp`, `MatterEngine3/src/tileset_bake_ao.cpp`

Replaced `typedef unsigned int GLuint` in both public headers with `uint32_t` (from `<cstdint>`)
and a comment explaining the aliasing. Added `static_assert(sizeof(GLuint)==sizeof(uint32_t))`
in both `.cpp` files to catch platform mismatches at compile time.

### M3 — Derive ray_y from instance envelope
**File**: `MatterEngine3/src/tileset_bake_gpu.cpp` (lines 36-61, 153-163)

`compute_height_range` now also tracks `max_instance_top = pose_y + 2*scale` per instance.
`ray_y = max(hmax + 2.0, max_instance_top + 0.5)` so tall scaled instances don't poke above
the ortho ray origin.

### Build/test results
```
Step 1: cd MatterEngine3 && make clean && make  → OK (no errors, pre-existing warnings only)
Step 2: CPU suites (tilesetdsl, tilesetplacement, tilesetbake, tilesetphysics,
         tilesetcore, tilesetgtex, tilesettorusbvh) → ALL PASS
Step 3: cd viewer && make tileset-gpu-tests tileset-seam-tests → builds OK
Step 4: GALLIUM_DRIVER=d3d12 ./tileset_gpu_tests → 51/51 PASS
        GALLIUM_DRIVER=d3d12 ./tileset_seam_tests → 32/32 PASS
```

### Commit SHA
`f445a77`
