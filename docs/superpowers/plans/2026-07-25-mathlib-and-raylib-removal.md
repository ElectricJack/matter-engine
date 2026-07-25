# MathLib and raylib Removal Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the duplicated matrix/vector code catalogued in `tech-debt.md` §1–§3, and drop raylib from the engine. These are the same job.

**The insight that shapes this plan:** the Windows/Vulkan editor **already does not link raylib**. `MatterEditor/Makefile:601` links `$(WIN_LIBS) $(GLFW_WIN_LIB) $(WIN_SYSTEM_LIBS) -lvulkan-1`, where `WIN_LIBS = box3d + winpthread`; the shipped `editor.exe` imports no `opengl32.dll`. What survives is a **header** dependency on raylib's POD types (`Vector3`, `Matrix`, `Mesh`, `Color`) plus 157 lines of `MatterEngine3/src/render/vulkan_only_compat.cpp` re-implementing the few raylib C symbols the bake path still calls — its own header says it is *"a CPU-only compatibility surface for bake code that still uses raylib's POD math and allocation API"* and it `abort()`s on any GPU call.

So the remaining raylib dependency **is** a math-types dependency. A MathLib is not tidiness; it is the mechanism that lets raylib go. Doing them as one programme means each type migration pays down debt twice.

**Tech Stack:** C++17, GNU Make, MSYS2 UCRT64, Vulkan.

## Global Constraints

- This repository does not discover sources by glob. Every new `.cpp` must be added to the explicit lists in `MatterEngine3/Makefile`, `MatterEngine3/tests/Makefile`, `MatterEditor/Makefile` and the relevant `libs/*/Makefile`.
- **The verification gate for every phase includes `make -C MatterEditor windows`** — `c3f0577a` passed the kernel build and every headless suite while leaving that target broken.
- `libs/MatterSurfaceLib` cannot be built here (missing `x86_64-w64-mingw32-g++-posix`, `tech-debt.md` §7). Compile-check its sources with `g++ -fsyntax-only` instead, and never `make clean` it — that used to delete the committed `raytrace_tlas_blas_processed.fs`.
- Do not run `git add -A` blind: the shader junctions have bitten twice (`tech-debt.md` §9). Verify `git ls-files -s MatterEngine3/shaders MatterEditor/shaders` still shows mode `120000` before every commit.
- Use `./build-dlss.sh` for a Streamline/DLSS build; it pins `STREAMLINE_DLL_DIR` to `bin/x64/development`.

## Standard Verification Gate

```bash
./build-dlss.sh clean
for t in run-script run-evalworld run-world-definition run-iso; do
    make -C MatterEngine3/tests $t TMP="$T" TEMP="$T" GRAPHICS=GRAPHICS_API_OPENGL_43
done
cd MatterEditor/build/windows && ./editor.exe   # expect: worlds available (14), DLSS ready
```

---

## Phase 1 — `libs/MathLib`: one Vec3/Mat4/Quat, one inverse

Resolves `tech-debt.md` §2 and §3.

The hazard being fixed is **not** duplication, it is that six inverses disagree on singular input: identity, zero matrix, `false`, and unguarded NaN. A degenerate transform behaves differently depending on which path reaches it.

- [ ] Create `libs/MathLib` with `include/matter_math.h` (+ `src/` as needed): `Vec2/Vec3/Vec4`, `Mat4` (row-major `float[16]`, matching `mat4` and `Matrix4x4`), `Quat`. Header-only where it can be; no raylib, no GL, no Vulkan.
- [ ] **Decide and document the singular-matrix policy once.** Recommended: return `bool` (as `matrix_math.cpp:120` already does) so the caller chooses, plus a `Mat4 inverse_or_identity()` convenience for call sites that genuinely want the current lenient behaviour. Record the decision in the header — the next person will re-litigate it otherwise.
- [ ] One `inverse`, one `multiply`, one `transform_point`/`transform_vector`, `translation`/`scale`/`rotation_{x,y,z,axis}`, TRS-from-quaternion.
- [ ] Tests covering: round-trip `M * M⁻¹ == I`, singular input hitting the documented policy, TRS composition against the existing `transform_math.h` output, and row-major layout (a transposed implementation passes naive tests).
- [ ] Delete the dead `invert4x4` at `MatterEngine3/src/matter_engine.cpp:197` — the compiler already reports it unused.
- [ ] Gate.

**Do not** migrate any call sites in this phase. Land the library and its tests first so later phases have something proven to move onto.

---

## Phase 2 — collapse `Matrix4x4` onto MathLib

Resolves `tech-debt.md` §1.

`libs/MatterSurfaceLib/src/tlas_manager.cpp` carries nine matrix functions on its own `Matrix4x4` — a C++ port of the API from the C BVH deleted in `c3f0577a` — plus `convert_matrix`/`convert_matrix_back` shims to `mat4`, which is the same row-major `float[16]` layout.

- [ ] Replace `Matrix4x4` with MathLib's `Mat4`; delete the nine functions and both shims.
- [ ] Update the consumers: `libs/MatterSurfaceLib/include/tlas_manager.hpp:21` (14 uses), `MatterEngine3/src/tileset_torus_bvh.cpp:162,169,299`, `MatterEngine3/src/render/tileset_bake_vk.cpp:420` (row-major → `VkTransformMatrixKHR` — verify the layout assumption still holds), `MatterEngine3/tests/part_asset_v2_tests.cpp:96-99`.
- [ ] Replace `world_tracer.cpp:34`'s `invert4x4` and `mat_math.h:27`'s `mul16` with MathLib calls.
- [ ] Gate.

---

## Phase 3 — replace raylib POD types in the engine

This is the phase that actually removes `#include "raylib.h"` from engine code. Counts in `MatterEngine3/src`: `Vector3` 126, `Matrix` 63, `Shader` 11, `Material` 11, `Mesh` 10, `Camera3D` 7, `Texture2D` 6.

- [ ] Migrate `Vector3` → `Vec3` and `Matrix` → `Mat4` across `MatterEngine3/src`. Mechanical but wide; do it in reviewable chunks (render/, provider/, bake/) rather than one commit.
- [ ] `csg_lowering.cpp` uses raylib `Matrix` and its own `mat_invert`. **Keep the zero-determinant guard** — raymath's `MatrixInvert` has none, and that guard is the entire reason the local copy exists (`tech-debt.md` §2). Port it onto MathLib's policy rather than dropping it.
- [ ] `dsl_state.cpp:31-40,76` builds the DSL transform stack from raymath `MatrixMultiply`/`MatrixTranslate`/`MatrixRotate*`/`MatrixScale`. Move to MathLib. **This changes bake output if any operation differs in convention** — verify `run-iso`, `run-evalworld` and a world bake byte-compare before and after.
- [ ] Gate, plus a bake determinism check: bake `world_demo/Demo` before and after and compare the emitted `.part` hashes.

---

## Phase 4 — replace raylib `Mesh`, delete `vulkan_only_compat.cpp`

- [ ] The engine already has `MeshIndexed` (`libs/MatterSurfaceLib/include/mesh_indexed.hpp`). Decide whether it becomes the single mesh type or whether MathLib/a new `GeometryLib` owns a plain mesh POD.
- [ ] Migrate the ~10 raylib `Mesh` uses plus the `UploadMesh` call sites.
- [ ] Once nothing calls raylib's allocator contract, delete `MatterEngine3/src/render/vulkan_only_compat.cpp` (157 lines) and drop `-I$(RAYLIB_PATH)` from the Windows include paths.
- [ ] Confirm `grep -rn 'raylib.h' MatterEngine3/src MatterEditor` is empty for the Vulkan build.
- [ ] Gate.

**At the end of this phase the Windows/Vulkan build has no raylib dependency of any kind** — not link, not header.

---

## Phase 5 — decide the fate of the Linux/GL path

After Phase 4, raylib survives in exactly two places: the Linux app build (`MatterEditor/Makefile:72`, `LDLIBS = .../libraylib.a`) and the GL upload machinery in `tech-debt.md` §6.

- [ ] **Decision required.** Either (a) retire the Linux/GL path — `main_linux.cpp`, `ui_linux.cpp`, the GL BLAS/TLAS uploaders, `bvh_visualizer` — and remove raylib from `third_party/` entirely; or (b) keep it as a fallback, in which case raylib stays vendored but is confined to that one target.
- [ ] If retiring: delete the §6 machinery (`textures_dirty_`, `shader_values_dirty_`, per-entry `gpu_dirty`, both `Texture2D` members, `ensure_gpu_textures_ready()`, `bind_to_shader()`) rather than refactoring it. `content_revision()` (landed `b11d36fc`) is already the backend-agnostic replacement.
- [ ] **Carry these two facts forward before deleting the code that documents them** (they exist only as comments in the doomed files):
  1. `bind_to_shader` stages textures every frame regardless of dirty state — raylib's batch resets `activeTextureId` after each draw.
  2. `TLASManager::mark_dirty` resets `cached_shader_id_ = 0` — GL reuses program ids after a shader is deleted, so a cached uniform location can silently belong to a stale program.
- [ ] `libs/MatterSurfaceLib/main.cpp:1150,1151,1496` also calls `bind_to_shader`; that standalone app may retire with the GL path.
- [ ] Gate.

---

## Phase 6 — cleanup sweep

Independent of the above; can be done at any time by anyone.

- [ ] `tech-debt.md` §8: six test files reference `projects/world_demo/schemas/`, gone since `83f171c9` (`WorldSector.js` is in `objects/`). Repoint them; `sector_bake_tests` currently reports 11 failures.
- [ ] §8: `realpath` undeclared under UCRT64 in four `abspath()` helpers — these fail to compile on Windows.
- [ ] §8: `lighting_garden_tests.cpp:272` undeclared `schemas`; `TILESETMEADOWMANIFEST_CPP` omits `world_definition_loader.cpp`; `material_registry_tests.cpp` wrong-depth include.
- [ ] §9: reconcile `STREAMLINE_DLL_DIR` — `MatterEditor/Makefile:78` says `bin/x64` (16 release DLLs), `tools/check_vulkan_toolchain.sh:10` and every historical build use `bin/x64/development` (18).
- [ ] §9: record `STREAMLINE` in `build_features.txt` so a binary states whether DLSS is compiled in.
- [ ] §9: stale comment at `MatterEditor/Makefile:390` still says Win32 import libs "MUST come AFTER libraylib.a" — there is no libraylib.a on that link line any more.
- [ ] §7: `libs/MatterSurfaceLib/Makefile` hardcodes `x86_64-w64-mingw32-g++-posix`; plain `x86_64-w64-mingw32-g++` exists. One-word fix that makes the project buildable here again.

---

## What this plan deliberately does NOT do

- **It does not unify the three type families for their own sake** (`tech-debt.md` §4). MathLib replaces the *raylib* family because that removes a dependency. Whether `matter::Float3` and `float3` (precomp.h) then merge is a separate call — there is still only one cross-family converter in the whole engine (`row_major_to_matrix`), so the conversion tax that would justify it does not exist yet.
- **It does not touch the §5 "verified not debt" list** — the double-precision `V3` in `mesh_simplifier` (Garland-Heckbert needs doubles), `pf::V3` (deterministic sim), `vecmath.js` (QuickJS runtime), `GpuMat4` (packing layout), `NormalMat` (3×3 inverse-transpose).
- **It does not rename `SpatialQueryLib`** (§7), though MathLib taking `precomp.h`'s role would make that rename more obviously worth doing.

## Sequencing

| Phase | Resolves | Risk |
|---|---|---|
| 1 — MathLib | §2, §3 | low; additive, nothing migrates |
| 2 — `Matrix4x4` collapse | §1 | low; one file, mechanical |
| 3 — raylib POD types | — | **highest**; wide, and touches bake determinism |
| 4 — `Mesh` + compat shim | — | medium; ends the raylib header dependency |
| 5 — GL path decision | §6 | gated on a product decision, not effort |
| 6 — cleanup sweep | §8, §9, §7 | low; independent |

Phases 1 and 2 are worth doing regardless of whether raylib ever goes. Phase 3 is the commitment point.
