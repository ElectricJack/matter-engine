# Windows MSVC build migration — design

**Date:** 2026-08-22
**Status:** draft for user review
**Order:** Phase 0 and specification 1 of 3; this must reach the migration
acceptance gate before the GPU visual-meshing or PhysX fluid-bake
specifications begin implementation
**Goal:** make MSVC the single supported compiler for MatterEngine's native
Windows editor and its complete linked dependency graph, while preserving the
existing Linux GCC build, editor behavior, artifact formats, and QA control
surface.

## 1. Decision and motivation

The Windows editor moves from MSYS2/UCRT64 GCC to the supported x64 MSVC 2022
toolchain. This is a build migration, not a renderer or gameplay change.

The migration is required before the new GPU work because it:

1. puts MatterEditor, MatterEngine3, CUDA translation units, and PhysX on one
   supported Windows C/C++ ABI;
2. lets the PhysX adapter link directly into the editor instead of requiring a
   Matter-owned cross-compiler bridge DLL;
3. replaces committed MinGW archives with reproducible MSVC builds;
4. makes the editor distribution an ordinary Windows package with explicitly
   staged third-party runtimes; and
5. prevents compiler migration failures from being confused with GPU meshing
   or fluid behavior failures.

MSVC becomes the only supported Windows compiler after parity is accepted.
MinGW remains available only as a temporary comparison and rollback target
during Phase 0. Linux continues to use GCC and the existing Makefiles.

## 2. Fixed scope

Phase 0 includes:

- a Windows-native CMake/Ninja build for the shipping editor and the engine
  targets it consumes;
- MSVC builds of every C/C++ dependency linked into `editor.exe`;
- portable replacements for GCC-specific source constructs reached by the
  Windows targets;
- Windows versions of the existing CPU and Vulkan test executables;
- an MSVC-aware `dist` package containing the editor and required runtime DLLs;
- a WSL entry point that invokes the Windows build through WSL interop; and
- parity verification against the current ravine world and editor controls.

Phase 0 does not include:

- PhysX source, libraries, or fluid code;
- GPU visual-meshing implementation;
- changing the Linux compiler or replacing the Linux Make build;
- changing the world DSL, terrain geometry, materials, rendering algorithms,
  artifact formats, or gameplay behavior; or
- maintaining two permanent Windows toolchains.

## 3. Supported toolchain

The reference Windows configuration is:

- Windows 11 x64;
- Visual Studio 2022 Build Tools or Community with the **Desktop development
  with C++** workload;
- the x64 MSVC v143 compiler and linker selected through `vswhere.exe` and
  `VsDevCmd.bat`;
- Windows 10/11 SDK;
- the CMake and Ninja executables bundled with Visual Studio 2022;
- Vulkan SDK with headers, import library, validation layers, and `glslc`;
- PowerShell 5.1 or later; and
- native Windows Python 3 with `py.exe` for repository and later PhysX build
  scripts.

The build pins Visual Studio 2022 even when a newer Visual Studio is installed.
The currently selected PhysX source documents Visual Studio 2022 as a tested
Windows compiler. A future compiler upgrade requires a deliberate toolchain
lock change and the same acceptance gates; the build must never silently select
the newest installed Visual Studio.

Release and RelWithDebInfo targets use one consistent MSVC runtime choice
across Matter and source-built dependencies. The initial choice is the static
runtime (`/MT`, with the matching debug setting only in isolated Debug builds),
which matches the current self-contained MinGW distribution goal and PhysX's
documented Windows default. No build may mix `/MD`, `/MT`, debug, and release
objects in one process without an explicitly audited DLL ownership boundary.

Project C++ code uses C++17, `/permissive-`, `/Zc:__cplusplus`, `/EHsc`, and
UTF-8 source handling. Project targets start at `/W4`; warnings become errors
after the first green compile. Vendored targets receive narrow, documented
warning suppressions rather than source edits made only to silence MSVC.

## 4. Reference workstation dependency audit

The 2026-08-22 audit of the current workstation found:

| Dependency | State | Action |
|---|---|---|
| Visual Studio 2022 Community 17.14.7 | Installed | Use this supported instance |
| MSVC x64 19.44 / v143 | Installed | No user action |
| Visual Studio-bundled CMake and Ninja | Installed | Resolve through the selected VS instance |
| Windows SDK 10.0.26100.0 | Installed | Pin as the initial SDK |
| Vulkan SDK 1.4.357.0 | Installed | No user action |
| PowerShell | Installed with Windows | No user action |
| Windows Python 3.13.14 launcher `py.exe` | Installed and resolved explicitly | No user action |
| CUDA Toolkit 13.3 | Installed | Not needed for Phase 0 |
| CUDA Toolkit 12.8 | Not detected | Install side-by-side before the PhysX GPU control spike unless the pinned PhysX revision proves support for 13.3 |
| External PhysX checkout | Not part of Phase 0 | Create only when the PhysX plan starts |

All dependencies required for the compiler migration are installed. CUDA and
the external PhysX checkout remain deliberately deferred and cannot block the
compiler migration.

Before the PhysX phase, the dependency preflight must resolve the apparent
version mismatch rather than guessing. The current upstream Windows readme
names CUDA Toolkit 12.8 for GPU builds, while this workstation currently has
13.3. CUDA 12.8 may be installed side-by-side; the PhysX control spike records
the selected `nvcc`, host compiler, driver, and SDK versions.

## 5. Build-system architecture

### 5.1 Windows-native CMake and Ninja

A root CMake build defines the Windows targets. Ninja is the command-line
backend so the build is fast, deterministic, usable without the Visual Studio
IDE, and identical whether launched from PowerShell, a developer prompt, or
WSL.

The canonical entry points are:

```text
tools/build-windows.ps1 [-Config Debug|RelWithDebInfo|Release] [-Target <target>]
tools/build-windows-from-wsl.sh [Debug|RelWithDebInfo|Release] [target]
```

Both resolve the repository root, validate dependencies, select the pinned
Visual Studio 2022 instance, configure the x64 build, and invoke the same CMake
preset. No caller must manually open a Visual Studio developer prompt.

The CMake build tree lives outside the staged product directory. For example:

```text
MatterEditor/build/cmake/windows-msvc/ # CMake cache, objects, intermediate libs
MatterEditor/build/windows-msvc/       # linker/developer editor.exe + PDB
MatterEditor/build/dist/<name>/        # distributable package
```

This separation prevents CMake internals from leaking into screenshots,
packaging, or existing editor launch recipes.

### 5.2 One source manifest

The repository recently removed duplicate editor/engine source lists because
they caused silent omissions. The MSVC migration may not recreate that defect.

Small platform-neutral manifest files under `cmake/manifests/` become the sole
source lists for the
engine core, Vulkan viewer extension, Matter libraries, and editor. Each file
contains normalized repository-relative source paths. CMake reads the
manifests directly. The existing GNU Make builds read the same manifests, so a
new translation unit is added in one place.

A manifest gate fails on:

- a path that does not exist;
- a source listed twice;
- a source assigned to incompatible targets;
- a Windows-only or Linux-only source without an explicit platform tag; or
- a compilable project source below a participating directory that is absent
  without an explicit exclusion.

Generated shader headers remain owned by MatterEngine3's existing shader
source list until that list is moved through a separately verified mechanical
conversion. The migration cannot introduce a second SPIR-V inventory.

### 5.3 Target boundaries

CMake defines ordinary targets rather than one global pile of flags:

- foundation libraries under `libs/`;
- MatterSurfaceLib;
- a headless MatterEngine3 target;
- a Vulkan viewer object target containing the renderer extension;
- MatterEditor;
- source-built third-party targets; and
- individually named test executables.

The editor consumes engine object targets directly where namespace-scope
registration must be retained. This preserves the current `--whole-archive`
semantics without relying on linker extraction side effects. Static engine
archives remain available for headless consumers and tests. If a library must
be linked as an archive, MSVC's `/WHOLEARCHIVE:<library>` is used explicitly
and covered by a registration census test.

Compile definitions and include paths are target-scoped. The migration must
not retain the current duplicated editor/engine flag blocks as two independent
sources of truth.

## 6. Dependency conversion

Every binary object linked into `editor.exe` must be produced by MSVC or expose
a stable C/DLL ABI that has been explicitly accepted. Existing `.a` archives
are never passed to `link.exe`.

The initial dependency strategy is:

- **GLFW:** build its vendored C sources as an MSVC static target with the
  existing Vulkan-only context patch;
- **Box3D:** use its vendored CMake project or an equivalent narrow MSVC target
  and produce a `.lib` from the current pinned source;
- **Ozz Animation:** configure its vendored CMake project with matching CRT and
  build only the runtime/offline libraries Matter consumes;
- **autoremesher_core:** create a dedicated MSVC static target matching the
  current Makefile's exact source set and header-only TBB shim;
- **QuickJS-ng and Flecs:** compile their pinned C sources as MSVC C targets,
  retaining C linkage and version defines;
- **Dear ImGui, ImGuizmo, BC encoders, and Matter libraries:** compile from
  source as target-scoped C++ targets; and
- **Vulkan:** use the installed SDK headers and `vulkan-1.lib`; shaders remain
  generated by the installed `glslc.exe` and embedded as today.

Third-party build options, commit identities, CRT choice, and produced binary
hashes are written to `build_features.json`. No dependency is downloaded or
updated implicitly by the editor build.

## 7. Portability work

The first compiler pass is expected to expose a bounded set of portability
issues. Known examples from the current tree include:

- `__attribute__((aligned))` in SpatialQueryLib;
- GNU printf-format attributes in the engine logger;
- `__builtin_FILE`, `__builtin_LINE`, and `__builtin_return_address`;
- POSIX headers and calls that need correct Windows gates;
- GCC warning flags, dependency-file flags, and section flags; and
- GNU linker group, static/dynamic selection, garbage collection, and
  whole-archive flags.

Portable project macros isolate compiler intrinsics. Examples include alignment,
format checking, call-site capture, and return-address diagnostics. Source code
uses standard C/C++ or existing Win32 wrappers where practical. Compiler tests
cover macro layout and call-site behavior.

Windows path and process operations remain in platform-specific translation
units. The migration does not scatter `_WIN32` branches throughout engine
logic merely to make a compile succeed.

No broad warning disable, `/permissive`, forced include, or undefined-behavior
workaround is accepted as a migration solution.

## 8. WSL workflow

WSL remains a supported place from which to *launch* the Windows build. It does
not become the host toolchain.

The flow is:

```text
WSL shell
  -> tools/build-windows-from-wsl.sh
  -> wslpath converts the repository path
  -> powershell.exe launches tools/build-windows.ps1
  -> VsDevCmd.bat selects VS 2022 x64
  -> Windows cmake.exe + ninja.exe + cl.exe build Windows objects
```

The checkout and build tree must reside on a Windows filesystem such as
`C:\...`, visible from WSL as `/mnt/c/...`. This repository's current worktree
already satisfies that rule. A checkout stored under WSL's Linux filesystem
and reached by MSVC through `\\wsl$\...` is not a supported shipping-build
configuration because it introduces UNC/path compatibility and cross-filesystem
I/O costs.

The WSL wrapper must:

- detect disabled WSL interop with a precise error;
- use `wslpath` instead of hard-coding `/mnt/c`;
- invoke executables with their `.exe` suffix;
- preserve and report the Windows exit code;
- avoid passing Linux CMake, Ninja, Python, compiler, or Vulkan paths into the
  Windows configure; and
- print the final Windows and WSL artifact paths.

Therefore, `./tools/build-windows-from-wsl.sh RelWithDebInfo` remains a valid
developer command. It is functionally a remote control for the native Windows
build, not a cross-compile.

## 9. Packaging

`MatterEditor/build/windows-msvc` is the linker/developer output directory.
`matter_dist` reproducibly recreates the user-facing package at
`MatterEditor/build/dist/<project>` with no stale files. The package includes:

- `editor.exe` and PDB for non-Release developer builds;
- only runtime DLLs proven by the recursive PE import closure (Streamline when
  enabled, and later PhysX when selected);
- license and notice files for staged dependencies; and
- a machine-readable build manifest containing compiler, Windows SDK, Vulkan
  SDK, source revision, configuration, CRT, and feature identities.

After the later PhysX phase is accepted, the same staging mechanism adds every
pinned PhysX runtime DLL required by that build, including the GPU module, and
their notices. PhysX components selected as static libraries are contained in
`editor.exe`. A user receiving the editor must not install PhysX, CUDA Toolkit,
CMake, Visual Studio, or Python. Only a compatible NVIDIA display driver is an
end-user prerequisite for GPU fluid baking.

The game package remains separate. Accepted water artifacts are static, so a
game build does not load or distribute PhysX unless a later runtime feature
explicitly changes that decision.

## 10. Migration sequence

### M0 — freeze and preflight

- Record current MinGW compiler, SDK, feature, executable, and dependency
  identities.
- Run the current Windows build, CPU gates, Vulkan smoke tests, and ravine
  screenshot capture.
- Add a read-only MSVC dependency preflight with actionable error messages.
- Establish source manifests and prove the existing Make build consumes them
  without changing its source census.

### M1 — foundation and third-party libraries

- Configure the root Windows CMake build with VS 2022 and Ninja.
- Build MemoryLib, MathLib, SpatialQueryLib, ProfileLib, ParticleFlowLib, and
  the source-built C dependencies.
- Build GLFW, Box3D, Ozz, QuickJS-ng, Flecs, BC encoders, and
  autoremesher_core with the chosen CRT.
- Add smoke links for each boundary before composing the engine.

### M2 — engine

- Build the headless engine and its CPU tests.
- Build the Vulkan viewer object target and embedded shaders.
- Resolve compiler portability differences with focused tests.
- Verify public struct sizes, alignment, serialization goldens, registrars, and
  artifact digests against the MinGW baseline where byte identity is part of
  the existing contract.

### M3 — editor and Vulkan parity

- Link and launch `editor.exe` under MSVC.
- Run the existing Vulkan compatibility, smoke, fault-injection, compositor,
  and viewer gates.
- Load the current ravine and StreamMountain worlds.
- Capture matched screenshots and inspect terrain, boulders, materials,
  selection, camera controls, UI, world reload, and shutdown.
- Keep MinGW available for matched diagnosis until these gates pass.

### M4 — package and cutover

- Produce a runnable `dist` package from a clean checkout.
- Test it on a machine/process environment without developer PATH entries.
- Update canonical documentation and CI commands.
- Make MSVC the Windows default.
- Remove committed MinGW-only archives and the MinGW Windows build rules only
  after the parity package is accepted.

## 11. Error handling and diagnostics

The preflight reports missing components independently:

- Visual Studio 2022 or x64 v143 toolset;
- selected Windows SDK;
- bundled CMake or Ninja;
- Vulkan SDK headers, library, validation layer, or `glslc`;
- native Windows Python when a target requires it;
- incompatible or mixed CRT artifacts;
- stale CMake cache from a different compiler; and
- unsupported WSL/UNC checkout location.

Every configure records resolved absolute tool paths and versions. Failure
messages include the Visual Studio Installer component name or environment
variable needed to resolve the problem. The build never falls back silently to
Visual Studio 2026, MinGW, Linux CMake, or a different Vulkan SDK.

## 12. Verification and acceptance

The migration is accepted only when:

1. a clean Windows-native command builds the editor with VS 2022 MSVC;
2. the WSL wrapper produces the same configured target and artifact identity;
3. no MinGW `.a`, `libstdc++`, `libgcc`, or `libwinpthread` dependency appears
   in the staged executable or DLL import graph;
4. all required CPU tests compile and pass under MSVC;
5. Vulkan compatibility, smoke, device-fault, and compositor gates pass;
6. shader source edits rebuild SPIR-V and all consuming objects correctly;
7. editor registrars and world/DSL symbols match the baseline census;
8. the ravine and StreamMountain load without missing geometry, flipped
   normals, seams, material regressions, or camera/control failures;
9. matched screenshots show no unexplained visual difference;
10. world reload, cancellation, and editor shutdown release resources cleanly;
11. a clean `dist` directory launches without Visual Studio or MSYS2 on `PATH`;
12. the build manifest and third-party notices are present; and
13. the MinGW target can be removed without deleting the only test or packaging
    path for any Windows feature.

Warnings introduced by MSVC are triaged, not counted as success merely because
the linker produced an executable. The accepted project source build is warning
clean at its declared warning level.

## 13. Rollback and bisectability

Migration commits remain staged by M0–M4. The existing MinGW build remains
runnable in a distinct directory and is labeled rollback-only through the M4
product/docs commit. Each stage leaves at least one complete Windows editor
path green.

No stage combines compiler migration with GPU meshing, PhysX, terrain changes,
or world-authoring changes. If a parity failure occurs, matched builds can
compare the same source revision, project, camera, and replay timeline.

MinGW removal is deliberately not mixed into the M4 product/docs commit. Only
after the package is accepted may a separate final cleanup commit remove the
rollback rules and committed MinGW-only archives. Git history then becomes the
rollback mechanism; until that separate commit, the rules remain available but
non-canonical.

## 14. Expected source layout

Implementation planning may refine names, but responsibilities remain:

- `CMakeLists.txt` and `CMakePresets.json` — Windows target graph and pinned
  presets;
- `cmake/` — target helpers, dependency configuration, warning/CRT policy, and
  source-manifest reader;
- `cmake/manifests/*.sources` — compiler-neutral canonical source lists;
- `tools/build-windows.ps1` — native dependency resolution, configure, build,
  stage, and diagnostics;
- `tools/build-windows-from-wsl.sh` — WSL interop adapter only;
- `tools/check-windows-msvc-package.ps1` — imports, runtime, manifest, and
  forbidden-MinGW checks;
- `MatterEngine3/tests/compiler_portability_tests.cpp` — alignment, call-site,
  and diagnostic portability coverage; and
- existing Makefiles — Linux build plus temporary forwarding/comparison entry
  points during migration.

## 15. Downstream specification changes

After this design is accepted:

- the GPU visual-meshing specification becomes specification 2 of 3 and uses
  the MSVC/CMake Windows target graph for C++ and SPIR-V tests; and
- the PhysX specification becomes specification 3 of 3, replaces the C-ABI
  bridge DLL with a native internal adapter, links the official PhysX libraries
  through CMake, and requires the editor stage/package to contain every pinned
  redistributable runtime and notice.

The external PhysX *source checkout* remains outside this repository. That is a
source-management choice only; it does not require editor users to install
PhysX or run another process.

## 16. References

- Microsoft command-line MSVC setup and `VsDevCmd.bat`:
  <https://learn.microsoft.com/en-us/cpp/build/building-on-the-command-line?view=msvc-170>
- Microsoft WSL command and filesystem interop:
  <https://learn.microsoft.com/en-us/windows/dev-environment/wsl-interop>
- NVIDIA PhysX Windows build requirements:
  <https://github.com/NVIDIA-Omniverse/PhysX/blob/main/physx/documentation/platformreadme/windows/README_WINDOWS.md>
- GPU meshing successor:
  `docs/superpowers/specs/2026-08-22-gpu-visual-meshing-foundation-design.md`
- PhysX successor:
  `docs/superpowers/specs/2026-08-22-physx-fluid-bake-integration-design.md`
