# CastleMaterials fixture

Close-range fixture for the reusable grid-castle primitive and material APIs.
It shows all 12 deterministic voxel-stone variants, an oak beam with mortise,
pegs and iron straps, a scarfed plank, reflective gold, and clear/colored
transmitting glass. Dimensions are metres. Run it as world `CastleMaterials`.

The authoritative API and axis/material-handle contracts are documented at the
top of `shared-lib/castle_primitives.js` and `shared-lib/castle_materials.js`.

## Integration contract

`castle_primitives.js` exports `emitStone`, `emitBeam`, and `emitPlank` for the
three thin `Castle*` Part wrappers. `stoneParams`, `beamParams`, and
`plankParams` canonicalize every part input to flat finite scalars. Downstream
expanded assemblies should use functional `static requires(p)` together with
`stoneChildVariants`, `beamChildVariants`, or `plankChildVariants`, then use the
matching `placeStone`, `placeBeam`, or `placePlank` helper. Both paths call the
same canonicalizer, so declared and placed child parameters are identical.
Use a small catalogue of canonical dimensions for those baked child variants,
then fit clipped runs with the placement affine transform where practical.
Passing every computed floating-point span as a child dimension defeats cache
reuse by creating a separately baked shape for each nearly-identical length.

Stone dimensions are `length` (+X), `height` (+Y), and `depth` (+Z), with the
bottom bed at y=0. Beam dimensions are `length` (+X), `height` (+Y), and
`width` (+Z); plank substitutes `thickness` for height. Timber is centered at
the local origin. Dimensions describe the structural core; shallow face relief
and straps stand slightly proud. Plank thickness has a 0.10 m native minimum.

`defineCastleMaterials(prefix)` must run during world-module evaluation before
`World.roots` is read. It returns integer handles for `limestone[4]`,
`foundation`, `mortar`, `oak`, `oakEnd`, `iron`, `gold`, `agedGold`,
`clearGlass`, `coloredGlass`, `slate`, `terracotta`, and `plaster`. Handles are
world-local scalar values and must be passed through Part params; tint is not a
substitute. The glass materials are transmitting volume boundaries and the
gold materials have `metallic: 1`.

The fixture-only rounded samples put polished and aged gold beside closed clear
and colored glass volumes. Pale overhead and side cards provide a neutral form
for the metals to reflect; limestone targets behind the glass make transmission
and refraction readable in a still frame. These presentation objects are not
part of the reusable primitive API.

The voxel emitters intentionally use a coarse core session followed by a finer
ordered-detail session. This makes the native script mesher select a detail
rung fine enough for tool marks, grain, and chips while retaining planar stone
beds.
