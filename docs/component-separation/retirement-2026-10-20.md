# Legacy forwarder retirement (2026-10-20, clear-crest-48.9.7)

Earlier moves (48.9.1–48.9.6) relocated the graphics implementation into
`libs/MatterGraphics/src/native/` with `git mv` and left `.cpp` forwarders at
the old MatterEngine3 locations, each one an `#include` of the moved
translation unit. Those forwarders were build links, not APIs: no code
`#include`s a `.cpp`, so their only consumers were the CMake manifests, the
Make source lists and a few source-text checks. This change deletes them and
points every consumer at the canonical file. No implementation code changed.

## Removed paths

| Removed path (lines) | Replacement |
| --- | --- |
| `MatterEngine3/src/render/raster_mesh.cpp` (2) | `libs/MatterGraphics/src/native/raster_mesh.cpp` |
| `MatterEngine3/src/render/streamline_bridge.cpp` (2) | `libs/MatterGraphics/src/native/streamline_bridge.cpp` |
| `MatterEngine3/src/render/vk_atmosphere.cpp` (2) | `libs/MatterGraphics/src/native/vk_atmosphere.cpp` |
| `MatterEngine3/src/render/vk_cloud_shadows.cpp` (2) | `libs/MatterGraphics/src/native/vk_cloud_shadows.cpp` |
| `MatterEngine3/src/render/vk_gi_math.cpp` (2) | `libs/MatterGraphics/src/native/vk_gi_math.cpp` |
| `MatterEngine3/src/render/vk_lighting_controls.cpp` (2) | `libs/MatterGraphics/src/native/vk_lighting_controls.cpp` |
| `MatterEngine3/src/render/vk_resources.cpp` (3) | `libs/MatterGraphics/src/native/vk_resources.cpp` + `vk_device_internal.cpp` (the forwarder compiled both as one TU; they are now two manifest entries, as `libs/MatterGraphics/native_environment.cmake` already built them) |
| `MatterEngine3/src/render/vk_temporal.cpp` (2) | `libs/MatterGraphics/src/native/vk_temporal.cpp` |
| `MatterEngine3/src/render/vt_residency.cpp` (2) | `libs/MatterGraphics/src/native/vt/vt_residency.cpp` |
| `MatterEngine3/src/render/vt_stub_filler.cpp` (2) | `libs/MatterGraphics/src/native/vt/vt_stub_filler.cpp` |
| `MatterEngine3/src/render/water_animation_gpu.cpp` (2) | `libs/MatterGraphics/src/native/water_animation_gpu.cpp` |
| `MatterEngine3/src/render/water_field_vk.cpp` (2) | `libs/MatterGraphics/src/native/water_field_vk.cpp` |
| `MatterEngine3/src/render/water_field_vk_resources.cpp` (2) | `libs/MatterGraphics/src/native/water_field_vk_resources.cpp` |
| `MatterEngine3/src/render/water_forward_reference.cpp` (2) | `libs/MatterGraphics/src/native/water_forward_reference.cpp` |
| `MatterEngine3/src/render/water_mesh_animation_playback.cpp` (2) | `libs/MatterGraphics/src/native/water_mesh_animation_playback.cpp` |
| `MatterEngine3/src/render/water_surface_reference.cpp` (2) | `libs/MatterGraphics/src/native/water_surface_reference.cpp` |
| `MatterEngine3/src/world_lights.cpp` (2) | `libs/MatterGraphics/src/native/world_lights.cpp` |
| `MatterEngine3/tests/local_light_index_tests.cpp` (2) | `libs/MatterGraphics/tests/native/local_light_index_tests.cpp` |
| `MatterEngine3/tests/primary_light_culling_tests.cpp` (2) | `libs/MatterGraphics/tests/native/primary_light_culling_tests.cpp` |
| `libs/MatterGraphics/tests/raster_native.h` (2) | `libs/MatterGraphics/examples/imported_acceptance.h` (zero includers) |
| `libs/MatterGraphics/tests/vulkan_device_fixture.h` (2) | `libs/MatterGraphics/examples/native_device.h` (zero includers) |

43 forwarder lines deleted; 0 implementation lines moved or written.

## Consumers repointed

- CMake: `cmake/manifests/engine-core.sources`, `engine-viewer.sources`,
  `cmake/MatterViewer.cmake` (direct test source lists; manifest census
  41 → 42 because `vk_device_internal.cpp` is now its own entry),
  `cmake/MatterEngine.cmake` (the two moved CPU tests, which gain
  `MatterEngine3/tests` on their include path for the shared `check.h`),
  `cmake/tests/viewer_graph_tests.cmake` (viewer count 233/232 → 234/233,
  same reason), `cmake/tests/vt_link_inventory_test.ps1` (adds
  `vk_device_internal.cpp.obj`).
- Make: `MatterEngine3/Makefile` vpath gains `native` and `native/vt` so
  manifest-derived objects keep their basenames; `MatterEngine3/tests/Makefile`
  and the `MatterEditor/Makefile` rollback test rules name the canonical files.
- Source-text checks that read files by path:
  `MatterEngine3/tests/property_editor_tests.cpp`,
  `MatterEngine3/tools/qualify_shader_sources.cpp`,
  `MatterEditor/tools/check_vulkan_viewer.ps1`.

Incremental Make trees built before this change hold `.d` files whose first
prerequisite is a deleted forwarder (`No rule to make target
'src/render/water_field_vk.cpp'`). Delete the stale `build/obj*/<name>.d` and
`.o`, or `make clean`; fresh and CMake/Ninja builds are unaffected.

## Adapters kept

Each forwards to the single implementation and is kept because it is still
included or compiled by supported code:

- `MatterEngine3/src/render/vk_volumetrics.cpp`: a real host adapter. It
  defines `VkVolumetrics::set_lighting(const VkSceneLighting&)` against the
  engine's scene-renderer type, so it cannot move into MatterGraphics.
- The header forwarders in `MatterEngine3/src/render/*.h` and
  `MatterEngine3/src/{polygon_triangulate.hpp,world_lights.h,terrain_field.h,terrain_mesher.h,seam_boundary.h,seam_weld.h,tileset_settle.h}`:
  each is `#include`d by engine, editor or test code under its legacy name.
- `MatterEngine3/include/matter/{atmosphere,cloud_layers,cloud_shadow_settings,volumetric_quality}.h`:
  part of the public `matter/` include surface.
- The 24 GLSL forwarders in `MatterEngine3/shaders_vk/`: still `#include`d by
  engine shaders. `water_forward.frag`, `vt_bc_encode.comp` and
  `vt_feedback.comp` are also compiled as shader entry points.

## Duplicate audit

`tools/component-separation/check_dependencies.py --checks duplicates`
reports 0 violations before and after. Three candidate pairs were inspected
and are not duplicates:

- `MatterEngine3/src/csg_lowering.cpp` lowers the legacy DSL `BuildBuffer`
  into MatterSurfaceLib field primitives, while
  `libs/ProceduralGeometry/src/csg.cpp` folds data-recipe CSG nodes. They have
  different inputs and different callers.
- `libs/TerrainGeneration/src/field.cpp` already adapts the single
  `terrain_field::FieldProgram`/`FieldRuntime` in
  `libs/ProceduralGeometry/src/terrain_field.cpp`.
- `libs/AssetPipeline/src/gltf.cpp` is the glTF import/prepare path, while
  `MatterEngine3/src/export/` holds the OBJ/texture export path. Neither
  reimplements the other.

None of them was retired: retiring either side would mean rewriting callers,
which this change does not do.
