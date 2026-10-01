# Asset mesh and material export

Status: delivered for static opaque mesh/PBR assets in frozen **2026-09-16-r2**.
The user requested a complete exporter for the asset-authoring pilot. The r1
runtime and its editable project were preserved.
[Usage and explicit limits](../../agent/asset-export.md),
[handoff](../../agent/asset-handoff.md), and
[validation](../../agent/evidence/2026-09-16-asset-export/README.md) record the result.

## Deliverable

Export a baked static asset, including its child placements, into a self-contained
directory containing OBJ + MTL, conventional PNG material maps, a GLB for portable
PBR viewing, and a manifest. Preserve part-local metres, normals, UV seams, winding,
material assignment and the requested mesh LOD. Select the asset from the workbench
or through the agent command interface. Refuse incomplete or unsupported inputs
with an explicit error; never report a partial export as complete.

Use the production VT compositor to evaluate all requested texture pages, including
pages never visible on screen. Do not export screenshots, a transient resident-page
subset, or diagnostic tileset images. Use an isolated export destination and GPU
resources, retain immutable source inputs, and leave live residency untouched.

Color is exported in sRGB; normals, roughness, metallic and occlusion remain linear.
Convert the engine's two-channel canonical-frame normals to the exported mesh's UV
tangent frame. Export signed physical height as a 16-bit PNG with its minimum and
range in metres in the manifest. Preserve chart gutters. OBJ and glTF use different
V conventions; match their normal-map handedness explicitly.

The GLB follows [glTF 2.0](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html):
embedded geometry/images, metallic-roughness materials, normals and occlusion. Height
is supplied separately for applications with displacement/POM support. Standard
glTF does not reproduce Matter's POM traversal or displacement silhouettes.

Publish the directory atomically only when every file succeeds. Do not overwrite
an existing export directory. Record source identities, triangle counts, actual
texture dimensions/density, channel conventions and limits. Bound work and memory;
exceeding a limit must not silently lower requested quality.

## Validation and handoff

1. CPU round-trip checks for OBJ indices, UVs, winding, materials, PNG channels and
   16-bit height; GLB structure/accessor bounds and image references.
2. GPU checks for exact composed texture coverage, finite-source/weathering inputs,
   periodic module mappings, normal-frame conversion, independent export resources,
   and failed/cancelled export preservation.
3. Actual exported brick and pilot-column assets opened in an independent renderer;
   compare geometry and unlit base color first, then normal/PBR lighting.
4. Editor UI and agent-command success/error receipts; repeated exports to fresh
   destinations. Export must not require camera movement to make textures resident.
5. New frozen runtime with tested launch/export examples and hashes. Preserve r1.

The broader VT goal remains active. Finish this requested handoff before resuming
the wall seam, sparse override, terrain blending and performance acceptance work.
