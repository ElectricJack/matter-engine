# Geometry-derived clay bricks — source review

Follow-up checkpoints: [production finite-face preparation and cache](../2026-09-15-face-preparation/README.md)
and [walls sized from whole bricks](../2026-09-15-brick-walls/README.md).
The historical orchestration limits below describe this earlier source review.

Status: actual source geometry and finite geometry projection work. Clay
appearance now renders through direct VT on an inspection mesh. Production
textured stamps, low-poly wall composition, splats and realism acceptance remain
open under L2b/L6 of the [material plan](../../../superpowers/plans/2026-09-14-layered-surface-texturing.md).

## Geometry

Eight deterministic 245 × 84 × 112 mm solids have 2.5–3.7 mm rounded edges,
broad mould depressions, irregular deeper dents, **60 smaller pockmarks and six
short scratches per broad face**, plus chipped arrises. Scratch width is
0.8–1.9 mm and penetration 0.4–1.2 mm; individual strokes are 14–54 mm long.
Larger dents now penetrate 3.2–6.7 mm. The 161-operation source uses no physics.
The [recipe snapshot](recipes/clay-detail-v4.js) fixes this iteration.

`ClayBrickGeometryProof` renders all eight source meshes in flat colors to
expose shape. The [earlier eight-variant view](clay-shape-v2-group.png) predates
the denser pores/scratches. `ClayBrickMaterialProof` renders variant zero using
the separate shared clay material; neither scene is the final wall geometry.
The 1.5 mm inspection mesh cannot resolve every submillimetre feature. The
finite-face baker samples the continuous 3D field directly, without first
generating a mesh.

## Native geometry projection

The [v4 run](../2026-09-15-direct-source/clay-detail-v4-checks.json) passes all
eight variants × front/back at 256 × 96 texels, 1 mm pitch. Every face retains
interior recesses deeper than 3 mm and non-flat normals. Variant zero's two
complete faces are compared to the CPU reference; all existing castle-face
oracle and cancellation cases remain enabled. No Vulkan validation errors
were reported. The preceding v3 run projected all faces but failed the art
assertion because one variant lacked a >3 mm interior recess; the revised dent
depths satisfy the same assertion without changing tolerances.

- [Measured height and normals](clay-detail-v4-geometry.png)
- [All 16 raw projected faces](clay-detail-v4-faces.zip)
- [Raw-data hashes and run identity](clay-detail-v4-faces.json)
- [Plot generator](plot_projection.py)

Each raw record is little-endian height f32, UVN normal 3×f32, coverage u32.
Per-face JSON supplies dimensions and finite metric domain. These are evidence
dumps, **not** a prepared-asset cache format. Appearance is not in these files.

Test-only orchestration assembles each face from 32 eight-column regions on
one physical sampling lattice. This respects the existing field-work bound;
the global bound and 5 µm hit tolerance are unchanged. The production stamp
preparation/cache/publication path still needs to own this orchestration.

The original source revealed near-tangent misses that exhausted marching.
CPU and GPU interval proofs now use a scale-relative floating-point margin
instead of an extra absolute metre in the error scale. Analytic 12 µm exterior
and 3 µm near-surface probes verify that clear misses are rejected while the
hit-tolerance band is retained. Projection identity version is now 3. The
[projection and CPU checks](../2026-09-15-direct-source/clay-projection-v5-checks.json)
pass; the denser v4 source also passes with this change.

The v4 diagnostic reports roughly 65–91 ms summed GPU time per face on the
RTX 4090. This is a serial, bounded preparation fixture, with region dispatches
and host readbacks; it is not VT page-production timing or performance
acceptance. Cache and reuse these projections rather than repeating them for
each brick placement or material edit.

## Appearance and streaming connection

Latest native review: [close](clay-textured-v3-close.png),
[grazing](clay-textured-v3-grazing.png), [RT](clay-textured-v3-rt.png),
[albedo](clay-textured-v3-albedo.png), [normals](clay-textured-v3-normals.png)
and [wireframe](clay-textured-v3-wireframe.png).
The [source/settings manifest](clay-textured-v3-source.json) and
[capture results](clay-textured-v3-result.json) retain hashes and fixed cameras.

`shared-lib/clay_brick_material.js` records part-local firing variation,
roughness, small material pores and finer grain through the existing GPU
surface evaluator. Its footprint masks reduce unresolved fine detail. Large
damage remains in the source solid. Geometry and appearance recipes are
separate; evaluating this material on reconstructed projected hits remains
part of the prepared-stamp integration.

The direct-source world tape currently attaches to sector receivers, so the
material proof uses one isolated streamed source brick. A first flat-root
attempt produced identical images and did not exercise the material. The
initial streamed attempt then exposed a missing service binding: its temporary
ScriptHost had no GPU solid-source baker. That run was stopped after repeated
explicit bake failures; it has no accepted screenshots.

`LocalProvider::bind_solid_source_baker(host)` now shares the existing owned
request/queued GPU callback with install, cache-restore and streamed hosts.
The worker does not invoke Vulkan directly. The first successful textured
capture records the source bake, compiled local surface tape, populated VT
pages and raster/RT views. The [editor build](../2026-09-15-direct-source/clay-detail-v4-build-matter_editor.log)
passes. No source or binary changed during capture.

The first textured review is [close](clay-textured-v2-close.png),
[grazing](clay-textured-v2-grazing.png), [RT](clay-textured-v2-rt.png),
[albedo](clay-textured-v2-albedo.png) and [normals](clay-textured-v2-normals.png).
It makes the material visible, but its grain is too coarse and bubbly; the
following appearance iteration reduces relief amplitude and increases grain
frequency. Raster/RT lighting still differs and is not accepted as matched.
Capture durations include startup and fixed view waits; they are not bake
latency measurements.

The v3 capture uses [clay material v2](recipes/clay-material-v2.js): finer
650 cycles/metre grain, 320 cycles/metre material pores and at most 0.17 mm
additional recess. The smaller amplitude reduces the first pass's lumpy
appearance while retaining the actual pockmarks and scratches. Source geometry
is unchanged from the v4 projection run. This remains an art prototype, with
overly smooth large hollows and raster/RT lighting differences still visible.

Both textured runs captured an **empty initial group view** before the streamed
brick became drawable. The harness's process/image checks did not detect that
visual failure. Only the six subsequent populated views above are accepted;
the group image is not evidence of the material. A future capture harness must
wait for a drawable streamed receiver before its first shot. In populated
views the sampled queues are empty, with 50 occupied shaded pages and 80 after
wireframe. Native source meshing is about 105k triangles for this inspection
brick; this is not a proposed runtime wall budget.

The material-only iteration still ran the geometry mesher again, with the same
solid recipe digest `8b7de1b7102d2acc`. Therefore geometry-cache reuse on appearance
edits is **not proven or complete** by this scene. The prepared-stamp pipeline
must separate those dependencies and verify zero geometry work on appearance
edits. The retained native logs make this limitation explicit.

## Remaining material milestone

Prepare reusable finite geometry stamps with complete publication and separate
geometry/appearance identities; evaluate clay color/ORM/fine relief at their
3D hit points; select variants with stable brick identity; compose mortar,
paint, dampness, moss and repair splats into the same VT result. Preserve real
geometry for silhouettes and undercuts. The unrelated POM chart-continuity
acceptance remains open in the [boundary checkpoint](../2026-09-15-pom-boundaries/README.md).
