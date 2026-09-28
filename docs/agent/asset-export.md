# Static mesh and material export

Available in the frozen **2026-09-16-r2** asset-authoring build.
The previous `2026-09-16-r1` remains unchanged and does not include this feature.
See [the build handoff](asset-handoff.md) for launch instructions.

## Editor

Open an asset in **Bake Lab → Workbench**, finish its bake, then use **Export
asset** (below Params and the LOD inspector; scroll the panel if necessary).
Enter a new output folder and a mesh LOD, and select **Export OBJ + GLB +
textures**. Export runs between frames. The status line reports completion or an
error. Existing destinations are refused.

## Agent command

`asset.export` exports a published root in the current world, or the open workbench
asset. Export uses all material pages, independently of camera visibility and live
VT residency. It can briefly pause the editor while GPU baking and file writing
complete. A cold load may report geometry `bake.finished` while a deferred detail
tileset is still baking. If export reports a completed-bake/idle-queue or
tileset-not-loaded error, wait for that bake to finish and retry with a new request
ID. Those readiness failures occur before writing the destination.

```json
{
  "directory": "D:/asset-exports/villa-column-01",
  "source": "world",
  "module": "VillaDoricColumnPilot",
  "lod": 0
}
```

If a module has several root variants, supply `part_hash` as a decimal uint64
string from the scene inventory. It can replace `module`, or constrain it. The
receipt also returns `source_hash` as a decimal string to avoid JSON precision
loss. Manifest hashes use hexadecimal strings.

For the open workbench, use `"source":"workbench"` and omit `module`. Call it with
`tools/matter_agent.py asset.export --args '{"source":"workbench","directory":"D:/asset-exports/asset-01"}'` and the usual
`--cmd-file` / `--result-file` or `--session` arguments. The receipt lists the
absolute output directory, source hash, requested LOD, vertices, triangles,
materials and files. Use `--timeout 30` for material exports. A client timeout
is not proof that export stopped; inspect the terminal result and destination
before trying again.

## Files and conventions

| File | Contents |
| --- | --- |
| `asset.glb` | Embedded geometry, UVs, tangent normals and metallic-roughness PBR images |
| `asset.obj`, `asset.mtl` | Geometry with positions, normals, UVs, material assignments and external image references |
| `material_*-basecolor.png` | sRGB RGB base color |
| `material_*-normal.png` | Linear RGB normal, glTF UV tangent frame |
| `material_*-normal-obj.png` | Matching normal for OBJ's flipped V coordinates |
| `material_*-orm.png` | Linear occlusion / roughness / metallic |
| `material_*-occlusion.png`, `-roughness.png`, `-metallic.png` | Individual linear channels |
| `material_*-height.png` | Unsigned 16-bit storage of signed physical height |
| `material_*-clearcoat.png` | Optional linear clearcoat factor (R) and roughness (G); GLB uses KHR_materials_clearcoat |
| `manifest.json` | Source identities, mesh/image counts, conventions and height decode |

Geometry uses metres, +Y up, and a right-handed frame. Child placements are baked
into asset-local geometry. Normals and winding account for mirrored and nonuniform
transforms. Repeated translated placements share their exported material.

Decode height as `min_m + png_sample / 65535 * range_m`. Finished
surface tilesets use their stored bake `height_max` as the zero datum, matching
the engine sampler; finite surfaces and wrapped modules preserve their signed
datum. Legacy tileset bounds include padding and can therefore add a large
constant height offset (the marble pilot is one example). Inspect the decoded
range and set the receiving shader's datum/relief deliberately; export preserves
the source values without the editor's view-dependent POM depth cap. Upstream BC4
height compression can already have lost fine detail; PNG export preserves the
composed values at 16-bit precision but cannot recover that lost information.
Height is a separate image: standard GLB viewers do not reproduce Matter's POM ray traversal or displaced silhouettes.
OBJ MTL PBR extensions vary between importers; GLB is the preferred portable PBR
preview.

## Current scope and explicit limits

Static opaque mesh assets with base color, normal, roughness, metallic, clearcoat
and source occlusion are supported. This includes geometry-baked finite surfaces, weathering,
periodic material modules and legacy Wang details. Export also applies baked vertex
tint and vertex occlusion. View-dependent lighting and streamed hemisphere
illumination are not material maps.

Animation, sparse voxel assemblies, billboard impostors, LOD-dependent child
cutovers, world-field terrain, and opacity/emission/subsurface/transmission materials
currently return explicit errors. They are not silently flattened or dropped.

Export captures the completed engine bake. The current engine does not propagate
child finite-surface recipes through a flattened parent assembly; such an assembly
is already missing those details in the editor. Export those finite-surface parts
individually (or author their material on the parent) until that bake limitation
is resolved. Ordinary mesh materials and baked child transforms are preserved.

The requested root LOD must exist. Children use that ordinal, capped at their final
available mesh LOD. Each exported mesh must have complete baked surface charts.
Limits: 4 million triangle corners, 16 million unique material texels, texture axes
at most 8192, 4096 hierarchy nodes and eight child levels. Exceeding a limit fails;
export does not reduce density automatically. GPU scratch is bounded to eight pages
per submission. The final folder appears only after every file is written.
