# Shared castle structure mesh recipes

`castle_structure_catalog.js` removes duplicate `CastleWingStructure` layer-1
recipes from `CastleWingStructureAssembly`. The three default site programs
contain 15 wing manifests and 110 mesh recipes; these resolve to **40 shared
recipes**. Independently instanced bricks and timber remain unchanged.

The catalogue replays the real `emitStructure` function into a geometry
recorder. It applies the complete matrix stack, including the record's anchor
subtraction, and compares the resulting local operation streams. Box dimensions
and orientation, cylinder endpoints and orientation, triangle winding, operation
order and material calls all participate. Geometry numbers are normalized to
1e-9 metres to remove floating-point translation noise; material parameters,
detail and stair style are separate exact configuration keys. Full serialized
streams are compared, rather than collision-prone hashes.

The first equal recipe in deterministic site/wing/record order becomes the
representative. Each placement retains its original transform and substitutes
only the representative's scalar lookup parameters. `CastleWingStructure`
continues to resolve and emit its existing manifest recipe without modification.
Physical collision geometry and worker-owned structure generation are untouched.

Default catalogues are cached once per material/detail/style configuration.
A nondefault site seed extends that cached catalogue with that site's actual
seeded wing manifests; it does not recompile or replay the default catalogue.
Custom-seed representatives cannot depend on which other custom site happened
to be requested first. `castleStructureCatalogue` exposes `records`, `unique`
and a recipe resolver; `castleWingStructurePlacements` performs the assembly
substitution and preserves optional outer transforms and offsets.

Validation:

```sh
node projects/world_demo/tests/castle_structure_catalog_tests.mjs
```

The test replays original and substituted Parts independently using chained
point transforms, comparing actual world-space box corners, cylinder endpoints,
triangles, winding and materials. It covers all 15 default wings, custom seeds,
changed materials/detail/stair style, outer placement transforms, unchanged
primitive children, and the actual assembly wrapper's declarations and build.
The catalogue test is CPU authoring evidence; it does not run a native bake.
