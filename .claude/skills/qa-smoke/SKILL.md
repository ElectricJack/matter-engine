---
name: qa-smoke
description: Run the Vulkan smoke gate (the 12-mode PowerShell gate plus the per-mode ctests in `cmake/MatterViewer.cmake` (`ctest -L vulkan`)) or a single MATTER_VK_SMOKE_MODE case against the editor's Vulkan build. Use when asked to run the smoke tests, the Vulkan gate, or verify a Vulkan/VT change didn't break validation.
---

## Build the smoke exe first

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo vulkan_smoke_tests
```

Output: `MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe`.
Never build while an existing `editor.exe`/smoke exe is running (file lock).

## Full gate: the 12-mode PowerShell gate plus the per-mode ctests

`vulkan_smoke_tests.exe` supports about 57 `MATTER_VK_SMOKE_MODE` values. Two
harnesses cover them, and neither covers all of them:

**The 12-mode PowerShell gate** (`MatterEditor/tools/smoke_vulkan_faults.ps1`):

```bash
/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File MatterEditor/tools/smoke_vulkan_faults.ps1 \
    -TestPath MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe \
    -TimeoutMilliseconds 30000
```

Each mode is its own process, run from `MatterEditor/`, with a per-mode
timeout (`rt` 90 s, `rt-transmission` 45 s). Every mode must print
`validation errors: 0` and `ALL PASS`, exit 0, or the gate fails. Pass
`-TestPath`: the script's default still points at the retired MinGW
`build/windows/` exe. Its modes: `streamline-missing-instance-proxy`,
`streamline-missing-device-proxy`, `rt`, `rt-transmission`, `rt-disabled`,
`rt-unavailable`, `animation-skin`, `vt`, `vt-surfaces`, `vt-rt`, `vt-enrich`,
`vt-enrich-nort`.

**The per-mode ctests in `cmake/MatterViewer.cmake`** (label `vulkan`):

```bash
cd MatterEditor/build/cmake/windows-msvc/relwithdebinfo
CTEST="/mnt/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe"
"$CTEST" -L vulkan --output-on-failure            # every Vulkan ctest
"$CTEST" -R '^smoke_' --output-on-failure         # just the smoke_<mode> tests
```

ctest runs tests one at a time unless you pass `-j`; don't pass it (GPU
contention, WSL2 memory). `CMAKE_CTEST_COMMAND` in that directory's
`CMakeCache.txt` names the ctest to use if the path above moves. The
smoke ctests run from the build directory, each passing on `ALL PASS` and
failing on `validation errors: [1-9]`:

- `vulkan_smoke_tests` (no mode), `vt_feedback_visibility_tests` (`vt-feedback`),
  `vt_input_snapshot_tests`, `vt_direct_source_tests`,
  `vt_surface_material_tests` (`vt-surfaces`), `sparse_voxel_gpu_tests`.
- `smoke_<mode>` for `geometry-pages`, `vt-queue`, `vt-material-domain`,
  `vt-pom-work`, `vt-receiver-material`, `vt-module-residency`, `vt-export`,
  `vt-surface-connections`, `vt-feedback-pair`, `rt-empty-tlas`,
  `water-field-nort` (dashes become underscores in the test name).

The same label also runs the standalone GPU suites `vt_compositor_tests`,
`static_surface_vt_tests` and `solid_face_projection_gpu_tests` (the last runs
from the repo root and passes on `solid_face_projection_gpu_tests: PASS`).
`vt_compositor_tests`' authored-clay blocks stay opt-in: they need
`MATTER_VT_CLAY_FIXTURE` / `MATTER_VT_PERIODIC_CLAY_FIXTURE` set to a directory
of `clay-projected-<0..7>.fst` files, which
`solid_face_projection_gpu_tests` writes when run with
`MATTER_CLAY_FACE_DUMP=<dir>`.

## Single mode (faster iteration)

```bash
cd MatterEditor/build/cmake/windows-msvc/relwithdebinfo
WSLENV=MATTER_VK_SMOKE_MODE MATTER_VK_SMOKE_MODE=vt-enrich ./vulkan_smoke_tests.exe
```

`WSLENV` forwards the variable to the Windows process; without it the exe runs
the default mode. The exe supports more modes than either harness runs
(`cull`, `tileset`, `transform`, `outlive-unproven`, `retention-fault-*`, ...).
Grep `vulkan_smoke_tests.cpp` for `std::string(smoke_mode) ==` for the full set.

See `docs/agent/qa-cookbook.md` recipe 6 for the full mode rationale and
`docs/agent/control-surface.md` for `MATTER_VK_*` env vars.
