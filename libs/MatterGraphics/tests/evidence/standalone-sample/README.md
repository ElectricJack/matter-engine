# Standalone graphics sample acceptance

Task: `clear-crest-48.4.4`, epoch 8. Base: `1f4ff089a9a81f8f54528df6a131a70757648853`,
including producer/audit prerequisite delivery `1499febcf` and native viewer
manifest repair `596c04c3f`. Implementation commit:
`6239c3f1b7c256f3eb0268ab1021223b9bdaf458`. Native environment: NVIDIA GeForce RTX 4090,
MSVC 14.44.35207, Vulkan SDK 1.4.357.0, validation enabled; the exact resolved
tool paths/versions are in `toolchain.json`.

From native PowerShell at the repository root, with a previously nonexistent
build directory:

```powershell
libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo -Test `
  -BuildDirectory "$PWD/libs/MatterGraphics/build/sample-clean-epoch8" `
  -SampleOutputDirectory "$PWD/libs/MatterGraphics/build/sample-clean-epoch8/captures"
```

Result: exit 0, all 72 native build steps and 12/12 CTests passed. The wrapper
also ran the sample separately and all assertions passed. `clean-native.log`
and `ctest.log` preserve full focused output. The earlier illustrative layout
occluded one imported mesh and failed coverage; the corrected placements below
the imported rows retain the same coverage assertion.

Fixture IDs: imported `fixture.repeated-assembly` / `GFX-IMPORTED-RASTER`,
synthetic `gpu.synthetic`, and `GFX-SAMPLE`. Imported pixel checks cover exact
material values, depth within 0.0001, normals within 0.001, inverse-transpose
mirrored/nonuniform placement, identity, clear state and atomic failures.
The combined sample checks visible coverage, material/directional-light HDR
1.3 within absolute 0.0003, shared-prototype identity, caller/instance/frame
retirement and validation-clean teardown.

| View | Visible draws | Pixel counts per expanded draw |
| --- | --- | --- |
| imported-synthetic | 6 | 120, 120, 1027, 972, 435, 450 |
| one-shared-instance-removed | 5 | 120, 120, 1027, 972, 450 |
| placement-updated | 5 | 120, 120, 1027, 972, 435 |
| imported-only | 4 | 120, 120, 1027, 972 |

Each view has a lossless `*-actual.png`, `*-repeat.png` and `*-albedo.png`
encoded from the native PPM readback. Actual and repeat linear HDR/display
arrays match exactly in the executable; the saved PPM hashes also match.
`capture-hashes.json` records both PPM and PNG SHA-256 values. Repeat agreement
checks reproducibility, not editor end-to-end parity. Existing imported and
composition pixel oracles provide the independent appearance checks.

`linkage.json` records the Windows CMake File API's actual sources, dependency
edges and linker fragments for the sample and its three library targets. The
full independence test audits all compiled targets, source/include paths,
Ninja compiler dependencies and shader depfiles; procedural, terrain,
QuickJS/script host and live-world targets are absent. Native MSVC
`dumpbin /dependents matter_graphics_sample.exe` output is preserved in
`dumpbin-dependents.txt`: only Vulkan and Windows/C++ runtime DLLs occur.

`relocation.json` records 246 original lines relocated, 152 adapter/build lines
added after subtracting those moves, and 5 other code lines removed.
`native_device.h` (123 lines) and `imported_acceptance.h` (91) are byte-identical
moves. The 130-line sample extends the moved 13-line main solely with public
facade/pass/camera/light/capture/lifetime calls. The synthetic asset and PPM
writer likewise have one shared implementation; legacy test paths forward.
No renderer, shader or generation algorithm was rewritten. Evidence JSON/log
files over 50 lines are captured observations rather than implementations.

The sample renders uniform opaque PBR. Textures, VT, skin/water/impostor products
and unsupported required capabilities retain the producer's explicit failure
behavior. No broader graphics capability or performance claim follows.
