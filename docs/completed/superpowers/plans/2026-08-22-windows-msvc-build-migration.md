# Windows MSVC Build Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the shipping Windows MinGW build with a reproducible Visual Studio 2022 MSVC/CMake/Ninja build that can be launched autonomously from Windows or WSL and produces a verified editor distribution.

**Architecture:** A Windows-native PowerShell entry point discovers and pins Visual Studio 2022, MSVC v143, Windows SDK 10.0.26100, Vulkan SDK 1.4.357.0, CMake, Ninja, and Python, then configures one root CMake target graph. A thin WSL wrapper converts paths and invokes that same PowerShell entry point. Compiler-neutral source manifests feed both CMake and the existing Make build until MSVC parity is accepted.

**Tech Stack:** CMake 3.25+, Ninja, Visual Studio 2022 MSVC v143, PowerShell 5.1, Python 3, Vulkan SDK 1.4.357.0, C++17, C17, GNU Make/MSYS2 for temporary parity.

**Spec:** `docs/superpowers/specs/2026-08-22-windows-msvc-build-migration-design.md`

## Global Constraints

- Windows editor targets use Visual Studio 2022 Community/Build Tools in version range `[17.0,18.0)`; never select Visual Studio 18 implicitly.
- Use MSVC v143 x64, Windows SDK `10.0.26100.0`, `/MT`, C++17, `/permissive-`, `/Zc:__cplusplus`, `/EHsc`, and `/utf-8`.
- Use the CMake and Ninja executables bundled with the selected Visual Studio 2022 instance.
- Use Vulkan SDK `1.4.357.0`, including headers, `vulkan-1.lib`, validation layers, and `glslc.exe`.
- WSL launches Windows tools through explicit `.exe` paths; Linux CMake, Ninja, Python, compilers, and SDK paths never enter the Windows configure.
- Windows checkout/build paths must resolve to a local Windows drive, not `\\wsl$` or another UNC path.
- Existing MinGW builds remain available until M4 acceptance; CMake/MSVC output uses `MatterEditor/build/cmake/windows-msvc` and staging uses `MatterEditor/build/windows-msvc` during migration.
- Every binary object linked into the MSVC editor is built by MSVC or is an accepted Windows import library; no MinGW `.a` enters the link.
- Vendored dependency source is reused in place; no source copying, symlinks, implicit downloads, or dependency updates.
- GPU meshing and PhysX implementation remain outside this plan.

---

### Task 1: Windows dependency preflight and WSL entry point

**Files:**
- Create: `tools/windows/MatterWindowsToolchain.psm1`
- Create: `tools/check-windows-msvc-deps.ps1`
- Create: `tools/build-windows.ps1`
- Create: `tools/build-windows-from-wsl.sh`
- Create: `tools/tests/windows_toolchain_tests.ps1`

**Interfaces:**
- Produces: `Resolve-MatterWindowsToolchain -RepositoryRoot <string> -Json` returning `VisualStudioRoot`, `VsDevCmd`, `MsvcToolsVersion`, `WindowsSdkVersion`, `CMake`, `Ninja`, `Python`, `VulkanSdk`, and `Glslc`.
- Produces: `tools/build-windows.ps1 [-Config Debug|RelWithDebInfo|Release] [-Target <name>] [-PreflightOnly]`.
- Produces: `tools/build-windows-from-wsl.sh [Debug|RelWithDebInfo|Release] [target]`.

- [ ] **Step 1: Write failing PowerShell contract tests**

Create `tools/tests/windows_toolchain_tests.ps1` with assertions that the module exists, rejects a missing repository, resolves VS 2022 rather than VS 18, returns SDK `10.0.26100.0`, finds VS-bundled CMake/Ninja, finds Python 3, and finds Vulkan SDK `1.4.357.0`. The test exits nonzero on the first failed assertion and prints `windows_toolchain_tests: PASS` only after all real executable probes return zero.

- [ ] **Step 2: Verify the tests fail because the module is absent**

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/tests/windows_toolchain_tests.ps1
```

Expected: nonzero exit with `MatterWindowsToolchain.psm1 was not found`.

- [ ] **Step 3: Implement discovery and preflight**

Implement VS discovery with:

```powershell
& $vswhere -version '[17.0,18.0)' -products '*' `
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
  -property installationPath
```

Validate exact files for `VsDevCmd.bat`, v143 `cl.exe`, SDK headers/import libraries, CMake, Ninja, `py.exe`, Vulkan headers/import library/validation JSON/`glslc.exe`, and reject UNC repository roots. Return a structured object and serialize it only when `-Json` is requested.

- [ ] **Step 4: Implement native and WSL wrappers**

`build-windows.ps1` calls the resolver, invokes `VsDevCmd.bat -arch=x64 -host_arch=x64 -winsdk=10.0.26100.0`, configures the selected preset, builds the requested target, and preserves the child exit code. The WSL shell script verifies `/proc/sys/fs/binfmt_misc/WSLInterop`, converts its own repository root with `wslpath -w`, calls `/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe`, and prints both Windows and WSL artifact paths.

- [ ] **Step 5: Verify real Windows and WSL preflight paths**

Run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/tests/windows_toolchain_tests.ps1
wsl.exe -- bash -lc 'cd /mnt/c/Users/webde/.codex/worktrees/af80/matter-engine-cpp && ./tools/build-windows-from-wsl.sh RelWithDebInfo preflight'
```

Expected: both exit zero and select VS `17.14.7`, MSVC `14.44.35207`, SDK `10.0.26100.0`, CMake `3.31.6-msvc6`, Ninja `1.12.1`, Python `3.13.14`, and Vulkan `1.4.357.0`.

- [ ] **Step 6: Commit**

```bash
git add tools/windows/MatterWindowsToolchain.psm1 tools/check-windows-msvc-deps.ps1 tools/build-windows.ps1 tools/build-windows-from-wsl.sh tools/tests/windows_toolchain_tests.ps1
git commit -m "build: add pinned Windows MSVC preflight"
```

### Task 2: Baseline recorder and canonical source manifests

**Files:**
- Create: `cmake/manifests/engine-core.sources`
- Create: `cmake/manifests/engine-viewer.sources`
- Create: `cmake/manifests/matter-surface.sources`
- Create: `cmake/manifests/editor.sources`
- Create: `cmake/manifests/vendor.sources`
- Create: `cmake/ManifestSources.cmake`
- Create: `tools/check-source-manifests.py`
- Create: `tools/record-windows-baseline.ps1`
- Create: `tools/tests/test_source_manifests.py`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEditor/Makefile`

**Interfaces:**
- Manifest grammar: one repository-relative path per line; blank lines and lines beginning with `#` are ignored; platform suffixes are `|windows`, `|linux`, or `|all`.
- Produces: `matter_read_manifest(<file> <out-var> PLATFORM <windows|linux>)`.
- Produces: `tools/check-source-manifests.py --root <path>` with zero exit only for existing, unique, classified sources.
- Produces: `MatterEditor/build/baselines/mingw/build_features.json` and SHA-256 records for engine/editor binaries.

- [ ] **Step 1: Write failing manifest behavior tests**

Use Python `unittest` temporary directories to prove missing paths, duplicate paths, invalid platform tags, and unclassified compilable sources fail while a valid two-platform fixture passes.

- [ ] **Step 2: Verify RED**

Run `py -3 -m unittest tools.tests.test_source_manifests -v`.

Expected: import failure for `tools/check-source-manifests.py`.

- [ ] **Step 3: Implement parser, real manifests, and Make consumption**

Move the existing `ME3_CPP`, `MSL_CPP`, `MSL_C`, `VIEWER_ONLY_CPP`, `APP_SRC`, ImGui, QuickJS, Flecs, BC encoder, and GLFW Windows source inventories into the five manifests. Make reads them with `$(shell sed ...)`/a narrow helper so its compiled source census remains byte-for-byte equivalent. CMake reads the same paths through `matter_read_manifest`.

- [ ] **Step 4: Record MinGW baseline**

The recorder runs the documented MSYS2 Bash builds, hashes `MatterEngine3/build/libmatter_engine3.a` and `MatterEditor/build/windows/editor.exe`, captures compiler/Vulkan versions, runs `objdump -p` imports, and writes JSON without modifying product binaries.

- [ ] **Step 5: Verify manifests and unchanged MinGW build**

Run:

```powershell
py -3 -m unittest tools.tests.test_source_manifests -v
py -3 tools/check-source-manifests.py --root .
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/record-windows-baseline.ps1
```

Expected: all tests pass; both MinGW artifacts rebuild; baseline JSON records zero missing/duplicate sources.

- [ ] **Step 6: Commit**

```bash
git add cmake/manifests cmake/ManifestSources.cmake tools/check-source-manifests.py tools/record-windows-baseline.ps1 tools/tests/test_source_manifests.py MatterEngine3/Makefile MatterEditor/Makefile
git commit -m "build: establish canonical source manifests"
```

### Task 3: Root CMake policy and foundation libraries

**Files:**
- Create: `CMakeLists.txt`
- Create: `CMakePresets.json`
- Create: `cmake/MatterCompiler.cmake`
- Create: `cmake/MatterTargets.cmake`
- Create: `cmake/tests/compiler_policy_tests.cmake`
- Modify: `tools/build-windows.ps1`

**Interfaces:**
- Presets: `windows-msvc-debug`, `windows-msvc-relwithdebinfo`, `windows-msvc-release` using Ninja and `MatterEditor/build/cmake/windows-msvc/<config>`.
- Targets: `matter_memory`, `matter_math`, `matter_spatial`, `matter_profile`, `matter_particle_flow`, `matter_mesh_charting`, `matter_asset_store`.
- Helper: `matter_apply_project_defaults(target)` applies C++17/C17, `/MT`, `/permissive-`, `/Zc:__cplusplus`, `/EHsc`, `/utf-8`, and `/W4` to Matter-owned code.

- [ ] **Step 1: Write failing CMake compiler-policy test**

The script configures a tiny C/C++ target using `MatterCompiler.cmake`, inspects its target properties, and fails until the exact MSVC runtime, language standards, and compile options are applied.

- [ ] **Step 2: Verify RED**

Run VS-bundled `cmake.exe -P cmake/tests/compiler_policy_tests.cmake` and expect failure because the module does not exist.

- [ ] **Step 3: Implement root project, presets, and foundation targets**

Build each foundation library from its source-of-truth directory with public include directories and narrow target-to-target dependencies. Do not flatten all flags globally. Enable `CTest` and register existing library executables where their Makefiles already define tests.

- [ ] **Step 4: Configure and build foundation milestone**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_foundation
```

Expected: Ninja builds only MSVC `.obj`/`.lib` outputs and CTest foundation tests pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt CMakePresets.json cmake/MatterCompiler.cmake cmake/MatterTargets.cmake cmake/tests/compiler_policy_tests.cmake tools/build-windows.ps1
git commit -m "build: add MSVC CMake foundation targets"
```

### Task 4: Source-built third-party dependencies

**Files:**
- Create: `cmake/MatterThirdParty.cmake`
- Create: `cmake/tests/third_party_smoke/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `third_party/autoremesher_core/include/autoremesher/portable_compiler.h`

**Interfaces:**
- Targets: `matter_quickjs`, `matter_flecs`, `matter_box3d`, `matter_glfw`, `matter_ozz_base`, `matter_ozz_animation`, `matter_ozz_offline`, `matter_autoremesher`, `matter_imgui`, `matter_imguizmo`, `matter_bc7enc`.
- Aggregate target: `matter_third_party`.

- [ ] **Step 1: Write failing smoke-link project**

Create a tiny executable that includes and calls one stable public symbol from QuickJS, Flecs, Box3D, GLFW, Ozz runtime/offline, autoremesher, ImGui, ImGuizmo, and BC encoders. It must link every boundary without the editor.

- [ ] **Step 2: Verify RED**

Configure/build `matter_third_party_smoke`; expect unknown target failures.

- [ ] **Step 3: Implement source-built targets**

Compile vendored C as C17 and vendored C++ with dependency-local warning suppression. Reuse Box3D/Ozz source lists but force `/MT`; compile GLFW's Win32/Vulkan source set and `MatterEditor/src/glfw_vulkan_only_context.c`; compile autoremesher's exact Makefile source set with its TBB shim and portable compiler macros. No `.a` files are linked.

- [ ] **Step 4: Verify all boundary links and imports**

Build and run `matter_third_party_smoke`; inspect it with `dumpbin /dependents` and fail if `libstdc++`, `libgcc`, or `libwinpthread` appears.

- [ ] **Step 5: Commit**

```bash
git add cmake/MatterThirdParty.cmake cmake/tests/third_party_smoke CMakeLists.txt third_party/autoremesher_core/include/autoremesher/portable_compiler.h
git commit -m "build: compile Windows dependencies with MSVC"
```

### Task 5: Headless engine and MSVC portability

**Files:**
- Create: `MatterEngine3/include/matter/compiler.h`
- Create: `MatterEngine3/tests/compiler_portability_tests.cpp`
- Create: `cmake/MatterEngine.cmake`
- Modify: portability sites identified by MSVC under `libs/SpatialQueryLib`, `libs/MemoryLib`, `libs/ProfileLib`, and `MatterEngine3`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Macros: `MATTER_ALIGN(N)`, `MATTER_PRINTF_FORMAT(fmt_index, first_arg)`, `MATTER_CALLSITE_FILE`, `MATTER_CALLSITE_LINE`, and `matter::diagnostics::return_address()`.
- Targets: `matter_engine_core`, `matter_engine_headless`, and `matter_engine_cpu_tests`.

- [ ] **Step 1: Write failing portability tests**

Assert alignment of the affected public structs, call-site file/line capture, standard-layout/size invariants for serialized geometry, and a callable return-address diagnostic that may return null only on unsupported platforms.

- [ ] **Step 2: Verify RED under MSVC**

Build `compiler_portability_tests`; expect failures on missing macros and current GCC-only constructs.

- [ ] **Step 3: Implement portable compiler boundary and engine targets**

Replace reached GCC attributes/builtins with compiler-specific definitions in `matter/compiler.h`; gate POSIX-only source correctly; compile engine core plus source-built QuickJS/Flecs/Matter libraries. Use object libraries where namespace-scope registrars must be retained.

- [ ] **Step 4: Port and run required CPU tests**

Add CMake executables for the Windows-runnable set documented in `docs/agent/qa-cookbook.md`: world definition, script/evalworld, LOD distance, five event tests, sector stream/coord, terrain field/mesh, seam/contour tests, viewer logic, and props.

- [ ] **Step 5: Verify headless milestone**

Run `tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_engine_cpu_tests` followed by `ctest --preset windows-msvc-relwithdebinfo --output-on-failure -L cpu`.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/include/matter/compiler.h MatterEngine3/tests/compiler_portability_tests.cpp cmake/MatterEngine.cmake CMakeLists.txt libs MatterEngine3
git commit -m "build: compile the headless engine with MSVC"
```

### Task 6: Vulkan shaders and viewer object graph

**Files:**
- Create: `cmake/MatterShaders.cmake`
- Create: `cmake/MatterViewer.cmake`
- Create: `cmake/tests/shader_rebuild_test.ps1`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Targets: `matter_vulkan_spirv`, `matter_embedded_spirv`, `matter_engine_viewer_objects`, `vulkan_compat_tests`, `vulkan_smoke_tests`, `vt_compositor_tests`.
- Shader outputs: `MatterEngine3/shaders_vk/*.spv` and generated `MatterEngine3/shaders_gen/embedded_spirv.h`, matching the existing logical inventory.

- [ ] **Step 1: Write failing shader dependency test**

Copy one shader to a temporary fixture, configure the shader target, build twice, change the source, rebuild, and assert both the `.spv` and embedded header timestamps/hashes change while unrelated objects do not.

- [ ] **Step 2: Verify RED**

Run `powershell -File cmake/tests/shader_rebuild_test.ps1`; expect missing target/module failure.

- [ ] **Step 3: Implement shader and viewer targets**

Use Vulkan SDK `glslc.exe --target-env=vulkan1.3 -O`, preserve every include/define edge from the Makefile, run the existing Python embedding scripts with Windows `py -3`, and compile viewer-only `vk_*`, `vt_*`, scene registry, dynamic bridge, and Streamline-disabled sources as an object target.

- [ ] **Step 4: Verify viewer tests**

Build and run compatibility, smoke, and compositor targets. Require `validation errors: 0`, `ALL PASS`, and zero OpenGL/CUDA imports.

- [ ] **Step 5: Commit**

```bash
git add cmake/MatterShaders.cmake cmake/MatterViewer.cmake cmake/tests/shader_rebuild_test.ps1 CMakeLists.txt
git commit -m "build: add MSVC Vulkan viewer targets"
```

### Task 7: MSVC editor executable and WSL parity

**Files:**
- Create: `cmake/MatterEditor.cmake`
- Create: `cmake/tests/editor_registration_census.ps1`
- Modify: `CMakeLists.txt`
- Modify: `tools/build-windows.ps1`
- Modify: `tools/build-windows-from-wsl.sh`

**Interfaces:**
- Target: `matter_editor` with output `MatterEditor/build/windows-msvc/editor.exe`.
- Alias target: `editor`.
- Build scripts print `MATTER_WINDOWS_ARTIFACT=<absolute-path>` and `MATTER_WSL_ARTIFACT=<absolute-path>`.

- [ ] **Step 1: Write failing editor registration census**

Run the MinGW editor's list-worlds/registration diagnostic into a baseline file, then require the MSVC editor to expose the same world, DSL, property, and editor registration census.

- [ ] **Step 2: Verify RED**

Run the census script and expect `MatterEditor/build/windows-msvc/editor.exe was not found`.

- [ ] **Step 3: Implement editor target**

Compile editor, ImGui/ImGuizmo, and Vulkan GLFW sources; consume the engine viewer object target directly; link Box3D, Ozz offline/runtime, autoremesher, Vulkan, `gdi32`, `winmm`, `user32`, `shell32`, `ws2_32`, and `dbghelp`; set `WIN32_EXECUTABLE`; stage output to the required directory.

- [ ] **Step 4: Fix MSVC diagnostics one focused test at a time**

For each compiler/link failure, add or extend the smallest portability/contract test, observe it fail, apply one portable source fix, and rerun the focused target before continuing.

- [ ] **Step 5: Verify native and WSL builds are identical**

Build RelWithDebInfo once through PowerShell and once through WSL, hash `editor.exe`, and require identical source/toolchain/build-feature identity. Run registration census and a `StreamMountain` one-shot launch from the staged directory.

- [ ] **Step 6: Commit**

```bash
git add cmake/MatterEditor.cmake cmake/tests/editor_registration_census.ps1 CMakeLists.txt tools/build-windows.ps1 tools/build-windows-from-wsl.sh MatterEditor MatterEngine3 libs
git commit -m "build: link the Windows editor with MSVC"
```

### Task 8: Package, parity gates, and cutover

**Files:**
- Create: `cmake/MatterPackaging.cmake`
- Create: `tools/check-windows-msvc-package.ps1`
- Create: `tools/tests/windows_package_tests.ps1`
- Modify: `CMakeLists.txt`
- Modify: `CLAUDE.md`
- Modify: `README.md`
- Modify: `docs/agent/qa-cookbook.md`
- Modify: `docs/superpowers/specs/2026-08-22-windows-msvc-build-migration-design.md`

**Interfaces:**
- Target: `matter_dist` producing `MatterEditor/build/dist/<project>/editor.exe`, required runtime DLLs, PDB for RelWithDebInfo, notices, and `build_features.json`.
- Checker: `tools/check-windows-msvc-package.ps1 -DistPath <path>`.

- [ ] **Step 1: Write failing package tests**

Fixture packages must fail independently for a missing executable, forbidden MinGW import, missing runtime DLL, missing notice, missing build manifest, compiler mismatch, and a developer-PATH-only launch.

- [ ] **Step 2: Verify RED**

Run the package tests and expect failure because checker/target do not exist.

- [ ] **Step 3: Implement staging and machine-readable manifest**

Stage the editor and only proven runtime DLLs; emit compiler, SDK, Vulkan, source revision, configuration, CRT, dependency identities, hashes, and feature flags. Inspect imports with `dumpbin /dependents` and fail on `libstdc++`, `libgcc`, `libwinpthread`, `opengl32`, or unresolved DLLs.

- [ ] **Step 4: Run parity acceptance gates**

Run all CPU CTest labels, Vulkan compatibility/smoke/fault/compositor gates, ravine and StreamMountain captures, world reload/cancellation/shutdown, registration census, matched screenshot diffs, and clean-PATH package launch. Record results under `MatterEditor/build/baselines/msvc/`.

- [ ] **Step 5: Cut Windows documentation/default over to MSVC**

Document `tools/build-windows.ps1` and `tools/build-windows-from-wsl.sh` as canonical. Keep MinGW rules labeled rollback-only until every acceptance gate passes; remove them only in a separate final commit after the clean package is accepted.

- [ ] **Step 6: Verify complete migration**

Run:

```powershell
tools/build-windows.ps1 -Config RelWithDebInfo -Target matter_dist
tools/check-windows-msvc-package.ps1 -DistPath MatterEditor/build/dist/world_demo
wsl.exe -- bash -lc 'cd /mnt/c/Users/webde/.codex/worktrees/af80/matter-engine-cpp && ./tools/build-windows-from-wsl.sh RelWithDebInfo matter_dist'
```

Expected: all commands exit zero; package launch succeeds with developer paths removed; acceptance record contains no unexplained failures or visual differences.

- [ ] **Step 7: Commit**

```bash
git add cmake/MatterPackaging.cmake tools/check-windows-msvc-package.ps1 tools/tests/windows_package_tests.ps1 CMakeLists.txt CLAUDE.md README.md docs/agent/qa-cookbook.md docs/superpowers/specs/2026-08-22-windows-msvc-build-migration-design.md
git commit -m "build: make MSVC the canonical Windows toolchain"
```

