# Lighting Sculpture Garden Design

**Date:** 2026-07-15  
**Status:** Approved design, pending written-spec review  
**Target:** MatterViewer Vulkan hybrid renderer with Native and Streamline DLSS
Super Resolution output

## 1. Goal

Add a deterministic, walkable dusk sculpture garden that exposes interactions
between materially different neighbors. The garden is both a visual demo and a
visual red/green fixture for the remaining hybrid-ray-tracing work: implemented
diffuse GI, emission, GGX reflection, and clearcoat should already read well,
while authored glass, water, and scattering sculptures remain in place and
visibly improve when Tasks 9 and 10 are completed.

The demo is not a material editor, a text-heavy reference chart, or an
exhaustive Cartesian product of every property. It is a curated 5 by 5 matrix
of distinct sculptures selected to reveal direct light, emissive bounce,
roughness, metallic response, Fresnel reflection, clearcoat, colored
visibility, and later refraction and subsurface scattering.

## 2. Scope

### In scope

- One `LightingGarden` world discoverable by the existing MatterViewer world
  picker.
- A single open 5 by 5 matrix containing 25 sculptures.
- Mesh cubes, UV spheres, and organic marching-cubes isosurfaces distributed
  throughout the matrix.
- Deliberate adjacency between emissive, diffuse, reflective, clearcoat,
  transmission, water, and scattering materials.
- A dusk sun and sky, dark neutral ground, pale plinths, sparse backdrop slabs,
  open walkways, and a shallow water feature.
- Reuse of current production materials plus twelve reusable material presets
  added to the central material registry.
- A readable data table in the world schema mapping every cell to geometry,
  material, tint, transform, and parameter intent.
- Safe material-schema/cache invalidation and a fresh bake of the new world.
- A CUDA- and Streamline-enabled Windows build and one validation-clean Viewer
  launch for visual inspection.

### Out of scope

- Implementing hybrid-GI Tasks 9 or 10.
- Demo-only material overrides or shader branches.
- Runtime material editing, parameter sliders, selection UI, legends, or
  in-world text labels.
- Persisting Viewer lighting overrides into the world.
- Pixel-perfect screenshot gates, exhaustive screenshot suites, or performance
  qualification.
- Giving every material every property or repeating every material on all
  three geometry types.

## 3. Chosen Architecture

Use the existing authoritative `MaterialRegistry` and the normal JS Part DSL,
asset bake, Vulkan material upload, raster G-buffer, and RT surface-query path.
There is no parallel demo renderer.

The work has three bounded units:

1. **Reusable material presets.** Extend the registry from 18 to 30 entries,
   expose stable `MAT.*` names in `part_base.js.h`, bump the serialized material
   schema, and verify the Vulkan RT packing fields.
2. **Data-driven garden world.** Add one `LightingGarden.js` root and one
   `WorldData/LightingGarden/world.manifest`. The schema owns the cell table,
   environment geometry, mesh primitives, and a single separated
   marching-cubes field for the isosurface cells.
3. **Demo gate.** Bake from a fresh cache, confirm all 25 cells and the three
   geometry paths reach the Vulkan scene, build with CUDA and Streamline 2.12,
   and launch the world once with validation enabled.

This design intentionally adds reusable named materials rather than
`garden_*` materials. The values are useful to any authored world and remain
owned by the same registry as every existing material.

## 4. New Material Presets

IDs 0 through 17 retain their current meaning and values. Add the following
stable entries at IDs 18 through 29. Unless a field is listed otherwise:
`opacity=1`, `emission=0`, `transmission=0`, `ior=1`,
`absorptionColor=(0,0,0)`, `absorptionDistance=0`, `thickness=0`,
`subsurface=0`, `scatteringColor=(0,0,0)`, `scatteringDistance=0`,
`anisotropy=0`, `clearcoat=0`, `clearcoatRoughness=0`,
`specularStrength=1`, `specularTint=(1,1,1)`, `alphaCutoff=0`,
`shadowOpacity=1`, `surfaceFlags=NONE`, `meshingAlgorithm=0`, and no ground
tileset.

| ID | `MAT` name | Base RGB | Rough | Metal | Special fields |
|---:|---|---|---:|---:|---|
| 18 | `plaster` | `(0.82,0.78,0.72)` | 0.92 | 0 | flat shading, unique merge group |
| 19 | `charcoal` | `(0.035,0.04,0.05)` | 0.75 | 0 | flat shading, unique merge group |
| 20 | `chrome` | `(0.62,0.65,0.70)` | 0.03 | 1 | smooth shading, unique merge group |
| 21 | `goldRough` | `(0.83,0.57,0.17)` | 0.36 | 1 | smooth shading, unique merge group |
| 22 | `copper` | `(0.90,0.32,0.12)` | 0.18 | 1 | smooth shading, unique merge group |
| 23 | `ceramic` | `(0.82,0.86,0.90)` | 0.22 | 0 | clearcoat 1.0, coat roughness 0.04 |
| 24 | `lacquerRed` | `(0.45,0.015,0.02)` | 0.38 | 0 | clearcoat 0.85, coat roughness 0.08 |
| 25 | `lightCool` | `(0.35,0.65,1.0)` | 1.0 | 0 | emission RGB `(0.25,0.55,1.0)`, strength 6.0 |
| 26 | `lightWarmLow` | `(1.0,0.50,0.15)` | 1.0 | 0 | emission RGB `(1.0,0.35,0.08)`, strength 1.5 |
| 27 | `glassSmoke` | `(0.16,0.18,0.22)` | 0.04 | 0 | transmission 0.85, IOR 1.48, absorption RGB `(0.12,0.16,0.20)`, distance 1.5, thickness 1.0, `VOLUME_BOUNDARY` |
| 28 | `wax` | `(0.85,0.42,0.24)` | 0.62 | 0 | subsurface 0.80, scattering RGB `(1.0,0.35,0.18)`, distance 0.35, anisotropy 0.10 |
| 29 | `foliageThin` | `(0.12,0.32,0.08)` | 0.75 | 0 | subsurface 0.85, scattering RGB `(0.25,0.65,0.12)`, distance 0.18, anisotropy 0.35, `THIN_WALLED | DOUBLE_SIDED` |

Assign stable merge-group values 14 through 25 in the same order. Bump
`MATERIAL_SCHEMA_VERSION` from 2 to 3. The material count and schema mismatch
must make prior baked artifacts rebake through the existing compatibility path;
old cache files must not be destructively overwritten before a replacement is
complete.

Add the exact names above to `globalThis.MAT`. World schemas use names rather
than numeric literals.

## 5. Matrix Layout

Center the matrix at the origin. Cell centers use 5.0 world-unit spacing and
rows run from north to south. Each sculpture sits on a low pale plinth. The
schema table contains these exact assignments:

| Row | Column 1 | Column 2 | Column 3 | Column 4 | Column 5 |
|---:|---|---|---|---|---|
| 1 | plaster cube | polished-gold sphere (`metal`) | warm-emissive isosurface (`light`) | green-glass sphere | charcoal cube |
| 2 | copper isosurface | cool-emissive cube | snow sphere | water sculpture | red-lacquer isosurface |
| 3 | bark cube | clearcoat-ceramic sphere | warm-emissive cube (`light`) | chrome sphere | leaf isosurface |
| 4 | smoke-glass sphere | rough-gold isosurface | red-clay cube (`stone` plus red tint) | cool-emissive sphere | wax isosurface |
| 5 | dark-stone isosurface (`stoneDark`) | green diffuse sphere (`stone` plus green tint) | low warm-emissive cube | thin-foliage isosurface | white-stone sphere (`stone` plus pale tint) |

The matrix is intentionally interleaved rather than ordered as a smooth
property gradient. In particular:

- Row 1 places polished gold between plaster and warm emission, with glass on
  the emitter's opposite side.
- The center warm emitter is adjacent to ceramic, chrome, snow, and red clay.
- Smoke glass is adjacent to rough gold.
- Wax is adjacent to a cool emitter.
- Low warm emission is adjacent to green diffuse and thin foliage.

### Geometry contract

- **Cube:** direct mesh `box` with half extents `(1.15,1.15,1.15)`, rotated
  22.5 degrees around Y so at least three faces are visible from the entry
  path.
- **Sphere:** direct mesh UV `sphere` with radius 1.35.
- **Isosurface:** one organic CSG sculpture contained within its cell. Its base
  is two unioned spheres of radii 1.15 and 0.90 at local offsets
  `(-0.45,0,0)` and `(0.45,0.25,0)`, smoothed with 0.35. Alternating
  isosurface cells subtract a radius-0.55 sphere at `(0,0.35,0.80)` to expose
  an interior silhouette. Use 0.10 voxel spacing and keep every component at
  least 1.5 units inside its cell boundary.
- **Water sculpture:** a direct horizontal capsule from local
  `(-0.9,0,0)` to `(0.9,0,0)` with radius 0.75 using `MAT.water`. The
  environment also includes a 0.12-unit-deep, 1.5-unit-wide water strip along
  the northern perimeter, outside the walking lanes.

The garden schema remains deterministic: it uses no random values, frame time,
or external state.

## 6. Environment and Lighting

The root world provides:

- A dark neutral floor large enough to extend at least 4 units beyond the
  matrix on every side.
- Pale plaster box plinths with half extents `(1.60,0.175,1.60)`, centered at
  Y=0.175 so their top is exactly Y=0.35.
- Sparse charcoal backdrop slabs behind selected edge cells, never walls that
  isolate neighboring sculptures.
- A perimeter path and 2-unit-or-wider clear walking lanes between rows and
  columns.
- A shallow water strip at the edge of the garden.

The manifest uses a weak warm low-angle sun and cool dusk sky:

```text
LightingGarden
light sun  -0.55 -0.35 -0.75  0.45 0.24 0.12
light sky   0.055 0.075 0.16
```

The Viewer retains its session-default exposure of `-2 EV`. Authored emissive
sculptures are the dominant local lights; the sun and sky keep non-emissive
areas navigable and provide a shared reflection baseline.

## 7. Current and Future Rendering Behavior

The demo must remain truthful to the production renderer:

- Diffuse, emissive, GGX metal/dielectric reflection, roughness, and clearcoat
  use their current production paths.
- Glass, green glass, smoke glass, and water retain their final transmission,
  IOR, absorption, and boundary fields. Before Task 9, their visible base uses
  the current diffuse/GGX path plus any transmission-aware colored visibility
  already implemented. No fake refraction is added.
- Leaf, wax, and thin foliage retain their final scattering fields. Before
  Task 10, their visible base uses the current diffuse/GGX path. No fake
  subsurface lighting is added.
- Completing Tasks 9 and 10 must not require editing sculpture placement or
  material values. The same garden should visibly transition from fallback to
  final transport behavior.

## 8. Failure and Cache Behavior

- Invalid material IDs remain impossible in the authored table and are caught
  by registry/DSL tests.
- A material schema or count mismatch rejects old artifacts with the existing
  rebake diagnostic and regenerates them atomically.
- A garden bake failure reports the failing root and does not publish a partial
  artifact.
- Missing Vulkan RT or DLSS uses the renderer's truthful Native/raster fallback;
  the world itself contains no backend-specific branches.
- Unimplemented transmission/scattering is expected fallback behavior, not a
  load error and not a reason to substitute different material values.

## 9. Verification and Demo Acceptance

Keep verification proportional to a visual demo:

### Contract checks

- Registry count is 30 and schema version is 3.
- Every new named material maps to its exact stable ID and packed RT fields.
- Existing IDs 0 through 17 and their legacy 12-float OpenGL packing remain
  unchanged.
- The schema table has exactly 25 unique cells, includes every declared
  material assignment, and contains at least one cube, sphere, and isosurface.

### Runtime checks

- Bake `LightingGarden` from a fresh cache with zero part errors.
- Load all 25 cells into the Vulkan scene and report nonzero mesh and instance
  counts.
- Build MatterViewer on Windows with `HAVE_CUDA=1`, `HAVE_STREAMLINE=1`,
  `STREAMLINE_PATH=/d/SDKs/streamline-sdk-v2.12.0`, and
  `STREAMLINE_DLL_DIR=/d/SDKs/streamline-sdk-v2.12.0/bin/x64/development`.
- Launch once with the Vulkan validation layer configured and confirm zero
  validation errors through world load and initial presentation.
- Leave the Viewer running in `LightingGarden` for user inspection.

No automated pixel thresholds, exhaustive screenshot matrix, or performance
gate is required for this milestone.

## 10. Files and Ownership

- `MatterSurfaceLib/include/material_registry.h`: schema version.
- `MatterSurfaceLib/src/material_registry.c`: reusable material definitions and
  stable merge groups.
- `MatterEngine3/src/part_base.js.h`: stable `MAT.*` authoring names.
- `MatterSurfaceLib/tests/material_registry_tests.cpp`: identity, compatibility,
  and RT packing checks.
- `MatterEngine3/examples/world_demo/schemas/LightingGarden.js`: deterministic
  matrix and environment geometry.
- `MatterEngine3/examples/world_demo/WorldData/LightingGarden/world.manifest`:
  root selection and dusk lights.
- `MatterEngine3/tests/lighting_garden_tests.cpp` and its Makefile target:
  exact 25-cell mapping, deterministic coordinates, and the three-geometry-
  path contract.

## 11. Alternatives Rejected

**Separate family courts.** They reduce cross-material bounce and reflection,
which are central to this demo.

**A strict property-gradient chart.** It is easy to measure but reads as a test
card rather than a walkable sculpture garden and repeats too many similar
materials.

**Bespoke geometry for every preset.** It is visually rich but makes material
differences harder to diagnose because geometry changes dominate comparison.

**Runtime or world-local material overrides.** The engine already has one
authoritative registry. Adding a second material-definition path solely for a
demo would create serialization, packing, and backend-parity risk.

**Waiting for Tasks 9 and 10.** The garden is more valuable now as a stable
visual red/green fixture. Future transport work should improve an unchanged
scene rather than introducing its reference scene afterward.
