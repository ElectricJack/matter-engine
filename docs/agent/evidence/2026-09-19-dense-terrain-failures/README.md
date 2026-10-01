# Dense terrain bake failures

The quarter-metre stress session recorded 631 child-variant lookup failures and
72 geometry hierarchy failures (`group exceeded compiler staging triangle limit`).
See errors.txt for the individual messages and counts.

## Fixes

- Small rock sizes use binary-exact quarter-metre increments. The previous
  decimal expressions differed after the native float parameter round trip.
  A JS regression asserts exact float32 round-trip values, catalog completeness,
  deterministic placement and unique sector ownership.
- Hierarchy merging now considers the combined triangle count before choosing a
  neighbor. If no neighbor can fit within the staging bound, the current node
  becomes an independent root. This preserves its triangles and locked borders;
  it does not lower the source detail or raise memory limits. Compatible neighbors
  already consumed in the current pass are deferred normally.
- Native geometry_hierarchy_tests passes, including a deliberately tight group
  limit checking complete coverage and unchanged outer boundaries.

The source terrain remains at 0.25 m finest spacing, with the same relief the
user approved. Trees are re-enabled by terrainOnly=false. The review launcher
also enables RT, GI, volumetric clouds/fog, and cloud shadows; POM remains off.

## Remaining validation

Native editor build passed. Integrated seam diagnostics are in progress. The
quarter-metre mesher boundary format carries its actual rung and cell count;
no evidence yet establishes an additional density-dependent seam-welder defect.
Failed sectors can leave missing coverage, but that alone does not establish the
cause of every visible seam. Do not treat the seam issue as resolved until the
repaired scene is inspected.

Forest re-enablement exposed a dormant definition-loader error: defineMaterial
calls inside forestMaterials() were not registered during module discovery.
Forest materials now have module-scope declarations after the geometry-rock
material, preserving that handle order. The subsequent world reload passed
registration and entered asset preparation. mountain_forest_tests.mjs and
alpine_ecology_tests.mjs also passed. Clouds/fog and cloud-shadow flags are
accepted by the live editor command stream.
