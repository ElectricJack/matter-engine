# Vulkan fix3 review follow-up design

## Scope

Correct two review findings without modifying or staging `MatterEngine3/src/script_host.cpp`: preserve pre-existing material registry state across the diagnostic override, and make the Vulkan raster lighting assertion compare identical geometry and material emission.

## Diagnostic material override

`VulkanDiagnosticMaterialOverride` will optionally seed a non-default prior slot from `MATTER_VK_DIAGNOSTIC_GROUND_TILESET_PRIOR_SLOT` for runtime smoke coverage. Before applying slot 0, it packs the registry and reads slot 11 for the selected material. The snapshot is accepted only when finite, integral, and in the setter's supported range `[-1, 3]`; otherwise the override is rejected with a diagnostic.

On destruction, the class restores the exact snapshot through `MaterialRegistrySetGroundTilesetSlot`, packs the registry again, and logs whether the packed slot exactly matches the snapshot. The override smoke seeds slot 2 and requires the unsupported-texture warning plus seed and exact-restoration logs.

## Raster emission and lighting checks

The raster smoke retains the emission-5 dark-lighting sample, then independently renders emission 1000 and saturated `FLT_MAX`. The saturated result must be finite, strictly greater than the emission-1000 result, and within the documented HDR red-channel saturation band of 14000 to 16000.

Before testing bright sky, the smoke releases the saturated triangle, reinstalls the original emission-5 triangle, reuploads instances, and reruns culling. The dark and bright samples therefore differ only in `sky_color`; all geometry, material, transforms, and camera state are identical.

## Verification

Use test-first source/runtime assertions, then rebuild the Vulkan smoke under `-Wall -Wextra -Werror`. Run compatibility and actual raster modes with validation enabled, followed by the clean-cache viewer smoke override case and the source gate. Review the staged diff and confirm `script_host.cpp` remains unstaged.
