# Receiver-to-periodic-material mapping

Status: native planar mapping checkpoint verified. This checkpoint does not establish
that the JS wall scenes use independent module pages, nor visual approval.

## Verified implementation

- `VtResidency::bind_receiver_materials` stages one immutable table per canonical
  receiver parameterization. Entries bind retained module leases to validated
  planar chart frames, physical repeat dimensions, phase and signed height datum.
- Publication waits for every module tail. Existing receiver pages keep their
  previous complete mapping while a replacement is pending. Tables and their
  leases survive page release through the existing eight-frame reader horizon.
- A new geometry/surface snapshot requires a new binding. Old pages retain their
  original binding; unmatched newly generated pages use their finite fallback.
  Cross-page POM rejects mismatched publication tables.
- Shared raster/RT sampling keeps receiver AUX/geometry private and reads the
  independent module for albedo, normal, ORM and height. Normal XY rotates into
  the receiver's canonical frame; height decode remains distinct from the common
  displacement envelope.
- Desired LOD preserves sub-texel receiver footprints until module conversion.
  POM resolves material detail using the module's physical texel size, and limits
  steps against both coverage and material texels. Raster emits both requests.
- A table costs 80 bytes per chart plus Vulkan allocation overhead; page metadata
  stays 64 bytes, using its previously reserved 12 bytes for the table reference.

## Validation

All five MSVC build targets and thirteen focused native checks passed against
924 frozen source inputs. Logs and binaries were rehashed in
[the final audit](final-audit.json); there were zero Vulkan validation errors and
no RT skips. The source and executable manifests are linked there.

- New `vt-receiver-material` GPU fixture: three differently sized/oriented finite
  receivers share one actual compressed periodic producer. Six physical-coordinate
  comparisons have maximum channel error **0**. It checks rotated normals, signed
  height offsets, production chart POM, fine material demand over coarse coverage,
  pending-tail fallback, invalid-map rejection, removal and deferred final release.
- The independent sampler's 741 probes still pass (maximum channel error
  0.000023872), including the 12 NPOT failures reproduced by its old-lookup control.
- Existing residency, paired and raster-visible feedback, input-snapshot, composed
  POM/seam, direct-source and surface-parallax checks pass. Their raster/RT seam
  fixtures use finite materials; they do not prove mapped-module corner handling.

The first build (`receiver-map-v1`) emitted the shader optimizer's ID-overflow
error in RT lighting. The second compiler attempt exited without a diagnostic.
Separating periodic parent traversal from finite receiver lookup, reducing sample
state, and separating AUX/height reads from normal conversion fixed compilation
with the existing `glslc -O` flags. No toolchain optimization setting was weakened.

## Reproduce

Build with `tools/build-windows.ps1 -Config RelWithDebInfo -Target vulkan_smoke_tests`.
Then, from the repository root in native PowerShell:

```powershell
$env:MATTER_VK_VALIDATION='1'
$env:MATTER_VK_SMOKE_MODE='vt-receiver-material'
$env:TMP="$env:LOCALAPPDATA\Temp"; $env:TEMP=$env:TMP
& MatterEditor/build/cmake/windows-msvc/relwithdebinfo/vulkan_smoke_tests.exe
```

Require exit 0, `validation errors: 0` and `ALL PASS`. The exact full validation
commands and logs are in the linked test manifest.

## Still required

- Wire generic module/mapping authoring into the renderer's part-registration path
  and JS wall recipes, with source-edit/rebind integration.
- Prove mapped connected POM at real corners/caps, and suitable curved modules.
- Avoid base shading/encoding for receiver pages once only coverage is needed.
- Add sparse per-instance weathering; complete the terrain/contact workflow.
- Render wall captures, moving-camera raster/RT comparisons and performance gates.

The frozen `MatterEditor/build/asset-handoff/2026-09-16-r1` runtime is untouched.
Its editor was running during development; native correctness checks here must not
be represented as isolated performance measurements.
