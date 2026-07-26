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

## Phase 5 — port Linux to Vulkan, then retire the GL path

After Phase 4, raylib survives in exactly two places: the Linux app build (`MatterEditor/Makefile:72`, `LDLIBS = .../libraylib.a`) and the GL upload machinery in `tech-debt.md` §6.

**The renderer is already portable — this is a missing build target, not missing code.** Platform gating across the ten core Vulkan sources:

| Source | `_WIN32`/Win32 lines |
|---|---|
| `vk_pipeline`, `vk_resources`, `vk_temporal`, `vk_instance_cache`, `vk_gi_math`, `vk_lighting_controls`, `vk_emitter_gather` | **0** |
| `vk_scene_renderer`, `vk_volumetrics` | 2 each |
| `vk_context` | 20 (surface + instance extensions) |

And `create_surface()` (`vk_context.cpp:586`) already carries both branches:

```cpp
#ifdef _WIN32
    ... streamline.create_win32_surface(...)   // proxied so DLSS-G can hook the swapchain
#else
    return vk_ok(glfwCreateWindowSurface(instance, window, nullptr, &surface), ...);
#endif
```

Windows needs the special path only because Streamline must intercept surface creation for Frame Generation. Everything else in the stack — Vulkan, GLFW, ImGui, `glslc`, QuickJS, flecs, box3d — is cross-platform. `MATTER_VULKAN_ONLY` simply appears in one place (`Makefile:248`, the Windows flag set), and the Linux target builds `LINUX_APP_SRC` (`main_linux.cpp`, `ui_linux.cpp`) instead of the Vulkan `APP_SRC`.

So the choice is not "which platform do we sacrifice". It is **one renderer, two platforms, DLSS only where the SDK exists.**

- [ ] Add a Linux Vulkan target that compiles `APP_SRC` (the Vulkan `main.cpp`, already free of raylib behaviour calls) with `-DMATTER_VULKAN_ONLY -DMATTER_VULKAN_VIEWER`, rather than `LINUX_APP_SRC`.
- [ ] Build GLFW for Linux with Vulkan support — the Windows target already does a Vulkan-only GLFW via `MatterEditor/src/glfw_vulkan_only_context.c`; reuse that shape.
- [ ] Link `-lvulkan` plus X11/Wayland instead of `-lGL`. `HAVE_STREAMLINE=0` on Linux: Streamline is a Windows-only SDK, so **no DLSS on Linux** — the one genuine capability loss, and it is contained behind an existing flag.
- [ ] Expect real gaps in `vk_context`'s 20 gated lines: instance-extension selection (`VK_KHR_win32_surface` vs `VK_KHR_xlib_surface`/`wayland`), and validation-layer availability. The SPIR-V embedding step (`glslc` + `embed_spirv.py`) is already platform-neutral.
- [ ] Once Linux runs on Vulkan, retire the GL path outright — `main_linux.cpp`, `ui_linux.cpp`, the GL BLAS/TLAS uploaders, `bvh_visualizer` — and remove raylib from `third_party/` entirely. raylib then leaves the repository rather than surviving as a one-target dependency.
- [ ] Delete the §6 machinery (`textures_dirty_`, `shader_values_dirty_`, per-entry `gpu_dirty`, both `Texture2D` members, `ensure_gpu_textures_ready()`, `bind_to_shader()`) rather than refactoring it. `content_revision()` (landed `b11d36fc`) is already the backend-agnostic replacement.

**Fallback if the Linux port stalls:** keep the GL path as-is. raylib stays vendored but confined to that single target, and §6 stays open. This is strictly worse but it is not blocking — Phases 1–4 stand on their own.
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

**REVISED 2026-07-25, mid-execution — Phase 5 is split and 5a moves ahead of 4.**

Phases 1, 2, 3 and 6 landed as written. Scoping Phase 4 then found its stated
scope ("~10 `Mesh` uses, delete a 157-line file, drop `-I$(RAYLIB_PATH)`") wrong
by 3-4x: **48 of the 141 translation units in the Windows/Vulkan `editor.exe`
link set transitively include `raylib.h`** (2 MatterEditor, 28 MatterEngine3/src,
18 libs/MatterSurfaceLib), and the concentration is in `libs/MatterSurfaceLib` —
26 direct includes, 15 of them in public headers — which Phase 4 never mentions.
Cross-validated two ways: include-graph walk, and the compiler's own `-MMD`
output.

The fix is ordering, not more work. **Phase 5's GL-path deletion *removes* much
of that surface rather than migrating it**, so it must come first:

| Phase | Resolves | Risk |
|---|---|---|
| 1 — MathLib | §2, §3 | done (`d5ca3f95`, `6a8ae975`) |
| 2 — `Matrix4x4` collapse | §1 (partly — reopened) | done (`a48a71f0`, `a3a0e08f`) |
| 3 — raylib POD types in the DSL/CSG path | — | done (`a958dffc`, `bedfbb54`, `d351e59a`, `f241f2f4`) |
| 6 — cleanup sweep | §8, §9, §7 | done (`f674f773`, `ad93950f`) |
| **5a — delete the GL path + §6 machinery** | §6 | medium; **deletion only, and Windows-verifiable throughout** |
| **4 — raylib POD types in MatterSurfaceLib** | — | medium; against the *smaller* post-5a surface |
| **5b — Linux Vulkan build target** | — | written but **unverified**; isolated behind its own target |

Why 5a before 4: deleting `bind_to_shader()` / `ensure_gpu_textures_ready()` and
the `Texture2D`/`Shader` members takes `raylib.h` out of `blas_manager.hpp` and
`tlas_manager.hpp` outright, and `raster_composer`, `gpu_culler`, `renderer`,
`raster_mesh` and `main_linux.cpp` go with the path. Phase 4 then faces only the
POD boundary (`particle.h`, `fat_primitive.h`, `cell.h`, `cluster.h`,
`surface.h`, `csg_stages.h`).

**The cost of this ordering, stated plainly:** 5a removes the only Linux build
target, and 5b's replacement cannot be verified on this machine. Linux therefore
goes from an *unverified GL build* to an *unverified Vulkan build*. No verified
capability is lost — Windows/Vulkan is the target that gets built and run here,
and it stays green at every step — but nobody should read "Linux works" into
5b landing.

**One design question Phase 4 must answer, which the plan never surfaced:**
`particle.h` and `fat_primitive.h` are shared with **real C** (`surface.c`);
`fat_primitive.h:12-13` says so. `mm::Vec3` uses C++ default member initializers
and lives in a namespace — neither is valid C — so the raylib types there cannot
simply be swapped for `mm::` ones. A C-compatible POD layer in MathLib is needed
first.

**Not a risk, verified:** no on-disk format stores raylib structs. `.part` v1/v2
use flat `float[N]` arrays with padding guards (`static_assert(sizeof(ChildInstance)
== 72)`, `static_assert(sizeof(VolumeEmitter) == 60)`). Type swaps upstream of
that are compile-time only, not wire-format changes.

Phases 1 and 2 were worth doing regardless of whether raylib ever goes. Phase 3
was the commitment point, and it is past.
