# VT/POM mechanical move evidence — 2026-10-11

Task `clear-crest-48.4.6`, source reference
`6490dbab806876631465259cdf5b397f258c5837`. Supervisor guidance
`msg-744b1f95855e40e5b9e3529b4c70f625` confirms the move-only scope and deferred
VT qualification. See [the retained host boundary](../README.md#vt-residency-and-pom-relocation).

37 files / 10,763 original source lines moved with `git mv`: 25 C++ files and
twelve GLSL files. All reconstruct the reference byte-for-byte after restoring
include paths and the two compute entrypoints' version directives. New runtime
text is 101 lines of legacy forwarding files, with no new algorithm. Two Make
dependency additions preserve incremental rebuilds. New larger implementation
files are entirely relocated code; larger text evidence files are captured test
output. No test assertion or production budget changed.

Native MSVC v143, Vulkan SDK 1.4.357.0, NVIDIA GeForce RTX 4090, driver 610.74,
Vulkan API 1.4.341. Khronos validation enabled. Existing missing Epic overlay
manifest and unused shader-interface warnings remain; every smoke mode reports
zero Vulkan validation errors.

| Check / fixture IDs | Result |
| --- | --- |
| Native builds: `vulkan_smoke_tests`, `vt_residency_tests`, `vt_encoded_pages_tests`, `matter_editor` | All pass |
| Native CPU CTest: `vt_residency_tests`, `vt_encoded_pages_tests` | 2/2 pass |
| Standalone MatterGraphics lifetime, scene, composition, Vulkan, independence CTests | 5/5 pass; audits existing facade/composition, not the moved native VT runtime |
| Smoke modes `raster`, `raster-fault`, `rt-disabled`, `animation-skin` | All pass |
| Deferred `vt`, `vt-surfaces`, `vt-rt` | 7 failures, pass, 12 failures respectively; failing assertion lines match prior graphics-producer-move evidence |
| Optimized SPIR-V: `vt_feedback.comp`, `vt_bc_encode.comp`, `gbuffer.frag`, `rt_surface_test.rgen` | Exact byte equality; hashes recorded |
| Rollback Make dry runs with moved shader/header as `-W` input | Rebuild the G-buffer shader and residency CPU test |

[Captured results, source manifest, hashes and exact scripts](../../../tests/evidence/vt-relocation/)
retain the evidence. Full build logs are `/tmp/clear-crest-vt-move/build-*.log`.
Run from repository root (the native scripts require WSL interop):

```bash
./tools/build-windows-from-wsl.sh RelWithDebInfo <target-from-table>
```

Native PowerShell: `libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo -Test`.
CPU command: `ctest --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo --output-on-failure --no-tests=error -j 1 -R '^(vt_residency_tests|vt_encoded_pages_tests)$'`.
`run_smoke.py.txt` records the exact native environment/working directory and
serial modes. To repeat source/SPIR-V comparison, create `/tmp/clear-crest-vt-move`,
copy the recorded `moves.json` there, and run `python3 libs/MatterGraphics/tests/evidence/vt-relocation/verify.py.txt`.
VT runtime restoration remains `nimble-rapids-30`; P3 conversion/live adapter
acceptance remains ordered `.9.4`, as documented in the boundary above.
