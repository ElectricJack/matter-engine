# Villa column: asset pilot 01

A single Doric-inspired column for jackkern.com, now wearing white marble.
The original limestone recipe is retained as a comparison. Authored and
inspected with the frozen MatterEditor **2026-09-16-r1** runtime. This scene
contains one column and a neutral ground plane for inspection.

From that handoff directory, run `launch-villa-column.cmd`, or:

```powershell
.\launch.ps1 -World VillaDoricColumnStudy -TextureDumps
```

F11 toggles the editor panels. The column is `VillaDoricColumnPilot` in this
project `objects/architecture/villa/` directory, since the shared library
names it. Geometry lives in
`../../../../shared-lib/villa_doric_pilot.js`.

## Design and dimensions

- Metres, +Y up, bottom-centred origin; bounds `[-.5,0,-.5]` to `[.5,4,.5]`.
- Twenty rounded flutes with closed ends, tapered shaft with a subtle convex
  profile, neck rings, curved capital, bevelled square abacus and plinth.
- All silhouette details are real triangles with authored smooth normals and
  metric source UVs. Hard beds retain crisp edges. Components are closed
  and overlap at their concealed joints; this is not a single boolean-unioned
  solid or a fabrication model.
- `quality: 0 / 1 / 2` produces **8,680 / 5,440 / 3,440 triangles**.
  These are explicit recipe variants, not automatic runtime LOD switching.
- The current finish is `VillaMarblePilotDetail`, backed by
  `shared-lib/villa_marble_pilot.js`. It uses a 1.28 m periodic white/grey
  marble field, 512 texels per tile edge, and a polished ceramic substrate.
  The coloured `VillaMarblePilotSource` follows the `conifer_bark.js` baking
  pattern and is never instanced in the scene. Veins affect colour; microscopic
  relief stays below eight micrometres. The geometry/triangle counts stay fixed.
- The original `VillaLimestonePilotDetail` describes a half-metre periodic tile at 512
  texels/metre, using sub-millimetre pores and tooling. It uses the built-in
  stone palette, as the frozen tileset evaluator does not expose
  `defineMaterial`. The receiver material is declared by the world.

## What the example review informed

The handoff's 270 project JavaScript files (32,812 lines before this pilot)
were inventoried: castle masonry/structure/furniture, brick source/receiver
recipes, terrain, vegetation, lighting, physics, water and demonstration
scenes. The full inventory is in `asset-example-inventory.json` at the handoff
root. Representative implementations were read in depth.

The closest precedents were `castle_surface_shells.js` and
`castle_ring_surface.js` for direct surface vertices, `castle_materials.js`
for scene material handles, and `castle_finish_detail.js` for small periodic
height details. `Rock.js` and the stone/brick sources demonstrate voxel CSG;
the tree and alpine recipes demonstrate seeded procedural placement. For
this regular architectural silhouette, direct triangles preserve small
bevels and avoid a dense voxel mesh.

## Validation and website delivery

Run with Node 24 from this directory:

```sh
node verify.mjs
```

Checks cover deterministic output, exact bounds, finite UVs, unit normals,
nondegenerate outward faces, closed geometric seams at every quality, decreasing
triangle counts, and continuity of the periodic height tile. Engine screenshots,
logs and numerical evidence are under `pilot-output/villa-column-01/` in the
handoff, outside the authored scene.

Marble screenshots and material checks are in
`pilot-output/villa-column-marble-01/`. The native marble bake succeeded;
full-height, capital and base screenshots were inspected at 1728 x 1065.
Both colour and height repeat at the tile boundaries (maximum numerical
discontinuity 1.1e-14). The marble recipe is 3,114 bytes, 1,536 bytes gzipped,
excluding the engine-generated texture atlas. Frozen runtime hashes still pass.

The native geometry bake completed with zero errors, and the corrected tileset
bake completed successfully. The first tileset attempt failed because
`defineMaterial` is unavailable there; the final recipe follows the existing
`MAT.stone` pattern. Runtime hashes were reverified after authoring. Full-height,
capital and base captures were inspected at 1728 x 1084. Close views retain a
strong cool shadow cast and some repeating grain; material/lighting polish is
still appropriate before approving a production batch.

The portable geometry/height recipe is roughly 6 KB of source, about 2.6 KB
gzipped. That is **source size**, not a measurement of a finished GLB or its
textures. Reusing one generated mesh and one material for the site's 92 columns
is the intended next test. The website has not yet been changed to load it.
The native sixteen-layer `.gtex` cache is 2,362,656 bytes; it is an engine cache,
not a website delivery format. This overhead is another reason to test a
compact shared browser material before generating the rest of the kit.

This frozen editor has no general mesh/UV/material export command. Its debug
tile PNGs are diagnostic outputs, not a production PBR package. A website
handoff should either consume this same deterministic geometry recipe or use
a future validated exporter. Do not claim that this pilot establishes the
finished GLB/KTX2 pipeline or mobile performance.
