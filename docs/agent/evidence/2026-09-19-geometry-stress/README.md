# Streaming Mountains geometry stress profile

- Source voxel spacing is 0.25 m in the finest 64 m sector: 256 cells per
  axis instead of 32. Coarser sector levels retain the same relative density.
- World-space relief includes 9 m, 4 m, 1.5 m and 0.75 m noise wavelengths,
  with decreasing amplitudes. This changes the density field and cache identity.
- Inspection site near (425, 1450): 1,283 rock placements, 19 reusable source
  meshes at resolution 128 (196,608 triangles each). Approximately 252 million
  placed source triangles; only 3.74 million unique source triangles. This is
  an instancing/residency stress test, not a unique-geometry capacity claim.
- Terrain-only mode now includes this rock site while still excluding forest.
- Rock bottoms are buried relative to size. Half-open XYZ sector ownership
  preserves one placement per rock across nested sector levels.

The engine's existing QEM edge-collapse simplifier remains unchanged. Finest
hierarchy leaves preserve the input mesh; coarser parents lock shared boundaries
and carry measured geometric error. Improving shading normals, attribute-aware
collapse costs, and LOD selection should be evaluated independently from source
sampling. More source samples do not guarantee every sample is drawn at distance.

Validation: mountain_detail_rocks_tests.mjs and mountain_geometry_site_tests.mjs
pass. The latter checks catalog completeness, deterministic placement, and unique
XZ ownership at four nested levels. Cold native preparation is pending; this
profile deliberately invalidates previous prepared terrain data.
