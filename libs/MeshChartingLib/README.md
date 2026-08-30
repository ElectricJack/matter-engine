# MeshChartingLib

raylib-free, GL-free, dependency-free utilities for mesh chart segmentation and
UV atlas packing. Everything lives in namespace `mesh_charting`, declared in
`include/mesh_charting.h` and defined in `src/mesh_charting.cpp`. These were
salvaged from the chart-based imposter cage in MatterSurfaceLib and placed here
for independent reuse; WP-A of the chart-space virtual-texturing work
(2026-07-29) added the paged packer, the chart normals and the distortion
metric.

## API

Segmentation

- `build_adjacency(positions, indices, triCount)` — edge-welded triangle
  adjacency. Two overloads: `unsigned short` and `unsigned int` indices.
- `segment_charts(positions, indices, triCount, adj, coneDeg, nCharts&)` —
  region-growing normal-cone partitioning. Same two index widths.
- `chart_average_normals(positions, indices, triCount, chartOfTri, nCharts)` —
  area-weighted, outward-oriented average face normal per chart, returned flat
  as `nCharts * 3` normalized floats (degenerate chart falls back to +Y).
  **32-bit indices only** — there is no 16-bit overload.
- `plane_basis(n, T, B)` — robust right-handed orthonormal tangent/bitangent
  from a normal. Stable for a given `n`, but the rotation about `n` is
  arbitrary.

Packing

- `pack_charts(charts, atlasW, atlasH, pad, scale&, placements&)` — free-form
  shelf packing of `ChartRect`s measured in the caller's own units. `scale`
  (texels per input unit) is an OUT parameter; the search only ever scales
  DOWN, so success means everything fit, not that the atlas is tight.
- `pack_charts_paged(charts, page_texels, gutter_texels, max_atlas_dim,
  atlas_w&, atlas_h&, placements&)` — deterministic tallest-first shelf pack
  where each chart occupies a whole number of `page_texels` pages, so no
  finest-mip page ever spans two charts and any two charts' content is at
  least `2 * gutter_texels` apart. This is the packer chart-space virtual
  texturing uses.

Both packers leave `placements` empty (and `pack_charts` leaves `scale` at 0)
on every failure path — nothing partial from a rejected attempt escapes.

Measurement

- `projection_distortion(positions, indices, triCount, tri_list,
  tri_list_count, T, B)` — max over triangles of sigma_max/sigma_min of the
  per-triangle 2x2 Jacobian onto the (T,B) plane. 1.0 is isometric.

## Consumers

Compiled from source by its consumers, never copied (CLAUDE.md's rule):

- `MatterEngine3/Makefile` compiles `src/mesh_charting.cpp` (it is in
  `MSL_CPP`) into both `libmatter_engine3.a` and the editor's
  `libmatter_engine3_viewer.a`.
- `MatterEngine3/tests/Makefile` picks it up via `COMMON_MSL_BLAS_SRC`.

The live caller is `MatterEngine3/src/lod_bake.cpp`'s `build_chart_rung()`,
which runs `build_adjacency` → `segment_charts` → `chart_average_normals` →
`plane_basis` → `pack_charts_paged`. Its headless gate is
`make -C MatterEngine3/tests run-chart-atlas`.

## Build & test

```bash
make            # default goal `lib` -> build/obj/mesh_charting.o
make test       # delegates to `make -C tests run`
```
