# Standalone native graphics sample

`matter_graphics_sample` uploads the checked-in `fixture.repeated-assembly`
asset and the existing hand-built `gpu.synthetic` triangle through the same
facade. It renders their shared placements with the production mesh pass,
then feeds the resulting G-buffers to the production lighting/display pass.
The camera is a reversed-Z perspective view at (1, 2.5, 8), facing -Z,
with near/far 1/100 metres and tan(half-FOV)=1. The directional light arrives
along -Z with white intensity 2. Uniform opaque PBR is the supported baseline;
unsupported required capabilities and appearances fail explicitly.

From native PowerShell at the repository root:

```powershell
libs/MatterGraphics/build-windows.ps1 -Config RelWithDebInfo -Test `
  -BuildDirectory "$PWD/libs/MatterGraphics/build/sample-clean" `
  -SampleOutputDirectory "$PWD/libs/MatterGraphics/build/sample-clean/captures"
```

Choose a previously nonexistent build directory for a clean build. The wrapper
resolves the supported MSVC, Vulkan SDK, CMake, Ninja and Python toolchain and
requires Vulkan validation. Only the graphics component and contracts are
registered: procedural, terrain and script-host targets are absent. The
`matter_graphics_independence` test audits the actual source, linker, compiler
header and shader closure, including the sample executable. Python compiles
shaders and runs the audit; the executable does not embed a language host.

The same executable runs directly with an optional output directory:

```powershell
libs/MatterGraphics/build/sample-clean/matter_graphics_sample.exe captures
```

Four 256x256 views show both assets, removal of one shared instance, an updated
remaining placement, and the imported asset alone. Each writes `*-actual.ppm`,
`*-repeat.ppm` and `*-albedo.ppm`. Actual/repeat images must match exactly.
Per-placement coverage, known imported normal/material/depth pixels, and the
existing composition oracle's white-dielectric special case (HDR 1.3, absolute
tolerance 0.0003) check rendering independently of the repeat comparison.
Caller release, instance removal, frame retention and final resource retirement
are exercised separately; Vulkan validation also covers teardown. A repeat
comparison establishes reproducibility, not end-to-end parity with the editor.

The existing native device, imported acceptance caller, synthetic triangle and
PPM writer were relocated here and remain the single implementations used by
their tests. The sample adds application boundary wiring, not rendering or
geometry algorithms. It performs offscreen capture and needs no window.
