# Water-field interpolation at mesh edges

Date: 2026-08-30. Implementation: `5ea698fd` and `4ac0fb5b`.

## What changed

Missing presentation-field samples no longer make a valid water-mesh fragment
drop out of the water shading path. The water mesh remains the authority that
the fragment is water; the field supplies its flow and appearance inputs.

The shared CPU reference and GLSL sampling path now:

- renormalize bilinear interpolation over wet contributors only;
- if that neighborhood has no wet contributors, search at most two cell rings
  and blend the first supported ring with inverse-distance weights;
- interpolate flow, depth, normals, and local appearance data, while retaining
  the existing nearest wet-cell classification within the river interior;
- use deterministic categorical selection for borrowed fringe samples; and
- retain conservative water defaults when a valid water draw has no nearby
  field support, instead of falling back to the unrelated raw material color.

Slot, generation, material, and field-record validation remain required. A
material-only search does **not** receive mesh-authoritative defaults: it must
find bounded wet support, so an earlier dry rectangle cannot hide another
same-material river field. This is presentation interpolation, not new fluid,
terrain collision, or buoyancy coverage.

Implementation: `MatterEngine3/shaders_vk/water_surface.glsl`,
`water_forward.frag`, and `src/render/water_surface_reference.{h,cpp}`.

## Verification and limits

Fresh native CPU verification on 2026-08-30:

```powershell
& 'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe' --test-dir MatterEditor/build/cmake/windows-msvc/relwithdebinfo -C RelWithDebInfo -R '^(water_surface_reference_tests|shader_source_tests)$' --output-on-failure --no-tests=error
```

Result: **2/2 pass**, 0.09 seconds. The first attempt exposed an old
`shader_source_tests.exe` still asserting a removed waterfall experiment's
`BoundaryNormalBuffer`. Rebuilding that target with MSVC, PhysX enabled, and
`-PhysxRoot 'D:/PhysX-5.6.1'` resolved it without source changes; the result above
is the subsequent fresh run.

The reference tests cover wet-only weights, one/two-cell support, rejection
beyond the halo, stale bindings, unsupported water defaults, and overlapping
same-material fields. Production Vulkan regression coverage also exists in
`MatterEngine3/tests/vulkan_smoke_tests.cpp`; it was not rerun during this CPU
checkpoint while the character acceptance editor owned the GPU.

This closes the requested missing-field behavior, not overall raster-water
visual acceptance. Waterfall/plunge appearance, downstream foam proof,
reflection fallback, and final memory/performance gates remain on the
[roadmap](../../ROADMAP.md). Section continuity has its own
[acceptance record](animated-water-section-continuity-acceptance-2026-08-29.md).
