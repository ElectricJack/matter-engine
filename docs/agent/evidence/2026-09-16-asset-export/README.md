# Static asset export — 2026-09-16

User priority: add mesh/material export to a new frozen asset-authoring build,
preserving the r1 runtime and pilot source work. Broader VT work remains active.

## Implemented

- Workbench export section and `asset.export` agent command, backed by the same
  completed-bake exporter. Root module/key or decimal `part_hash` selection,
  exact root LOD, explicit error receipts and non-overwriting atomic publication.
- OBJ/MTL and self-contained GLB; UVs, normals, tangents, material assignments,
  right-handed metres/+Y and baked static placements.
- sRGB base color; conventional UV tangent normals; linear ORM and separate
  occlusion/roughness/metallic channels; optional glTF clearcoat; signed physical
  height stored in gray16 PNG with manifest decode. OBJ's flipped V has a matching
  green-inverted normal image.
- Private production VT composition across the full atlas, eight scratch pages
  per submission; finite sources, authored weathering, periodic modules and Wang
  details. Camera visibility/live page residency do not gate export coverage.
- Lossless PNG byte filtering/compression including every height bit. Windows
  publication retries transient sharing/access failures, records the native
  error code, and retains no-replace semantics.

## Validation

Final native build/check identities: `export-v8-*` in
[the native evidence directory](../2026-09-16-shared-vt-pixels/).
Four targets built successfully; writer, offline VT export, compositor, direct
source, surface parallax and receiver material tests passed with zero Vulkan
validation errors and no hardware-RT skips. Source hashes remained fixed.
The viewer CMake graph and editor registration census also passed.

The CPU fixture includes two materials, asymmetric NPOT images, clearcoat, UV
handedness and height samples 0, 1, 255, 256, 32768, 65535. PNG CRC/decompression
and exact 16-bit samples were checked independently. Official `gltf-validator`
returned zero errors/warnings. Compression preserved every decoded byte.

Real assets were exported from a disposable copy of the project and loaded in
Three.js 0.180.0 GLTFLoader and OBJLoader/MTLLoader, through Chromium software
WebGL. These are screenshots of real exported geometry, not generated imagery.

| Asset | Geometry | Material atlas | Independent checks |
| --- | --- | --- | --- |
| Marble column | 8,680 triangles, 1 × 4 × 1 m | 3456 × 4096, including clearcoat | OBJ/GLB render, zero loader errors, glTF validator 0 errors/0 warnings |
| Wrapped 2× brick wall | 12 triangles, 4.07 × .742 × .245 m | 2176 × 2688 | OBJ/GLB render, zero loader errors, glTF validator 0 errors/0 warnings |
| Workbench brick | 12 triangles | Full finite-surface material | Successful Workbench export receipt and visible preview |
| Painted two-part assembly | 24 triangles, both child placements | 1664 × 2176, one shared PBR/clearcoat material | Final-build export, external GLB view and validator 0 errors/0 warnings |

OBJ/GLB unlit column images differ by less than .00005 mean RGB byte value;
this independently checks UV orientation and base-color interpretation. See
`*-format-comparison.json`, `*-viewer.json` and `*-gltf-validation.json`.
Engine receipts also check existing-destination rejection and invalid LOD errors.

![Exported column in external viewer](column-v2-glb-pbr.png)
![Exported wrapped wall in external viewer](brick-v2-glb-pbr.png)

The real brick test exposed a shared mapping bug: packed chart origins store
only the in-plane offset and need not lie on the authored material plane. The
old validation rejected these valid charts. The fix validates actual mesh
points against the authored plane; tests now offset chart origins and still
reject a displaced material plane. This fixes offline export and live binding.

## Findings and boundaries

- Standard GLB carries PBR shading; displacement/POM height is a separate map.
  Export does not reproduce view-dependent Matter lighting, POM traversal or
  silhouettes in a standard viewer.
- The marble pilot's legacy tileset uses padded bake bounds. Its exported
  height datum is therefore offset by about .548 m; that is source metadata,
  not half a metre of marble relief. The existing BC4 tileset upload also loses
  microscopic height variation. Export preserves composed values, without
  pretending to restore that detail or applying a view-dependent depth clamp.
- `assembly-v1` deliberately exercised two finite-surface child walls. The
  current engine flattens their geometry but drops their child recipes before
  rendering; the editor and export both show plain boxes. It verifies geometry
  placement only, not preservation of those missing child materials. This
  upstream limitation is documented for authors. Export finite-surface parts
  individually or author the parent's surface until propagation is implemented.
- Static opaque mesh materials are supported. Animation, sparse voxels,
  billboard LODs, child cutovers, world-field terrain and unsupported material
  lobes return explicit errors. See [usage and limits](../../asset-export.md).
- `column-v1` is retained failed evidence from the original Windows publish
  failure. `column-v2` succeeds after PNG compression and bounded rename retries;
  the original transient failure's exact cause was not captured.
- Preview scripts are validation tooling under `/tmp/matter-export-validation`;
  Three.js, Playwright and the validator are not product/runtime dependencies.

The final `assembly-v2` fixture uses an ordinary blue PBR/clearcoat material;
its child positions, color and material survive flattening and export.

The first cold packaged column attempt correctly refused export while the
deferred marble tileset was still baking. The updated harness retries only
explicit pre-write readiness errors, with unique request IDs; it never blindly
retries a timed-out write.

Frozen handoff and final packaged validation are recorded in
`MatterEditor/build/asset-handoff/2026-09-16-r2/validation/` and `snapshot.json`.
The original r1 runtime passed `verify-runtime.ps1` after this work.

## Frozen r2 acceptance

Final executable SHA-256:
`3aac176acbea521dfc6f1c0b017c5f376a2dc5ae7e1b4c7e18bdfc5db932b615`.
All 17 pinned runtime files verified. Clean-PATH import/census checks passed.
The packaged column export succeeded by both module and exact decimal part hash;
all 13 files are byte-identical to one another and to the independently rendered
column export. Existing destination and invalid LOD checks passed, capture landed,
and the editor exited normally with zero Vulkan validation errors.

See `handoff-result.json` and `r2-column-*` for the final records. The earlier
`column-ready` harness stopped after a successful export because PowerShell
needed an explicit string-array cast for appending its command tail; its owned
editor was closed, the harness fixed, and the complete `column-confirm` run passed.
