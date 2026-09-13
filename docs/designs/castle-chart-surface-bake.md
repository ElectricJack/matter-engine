# Castle geometry-derived chart surfaces

Status: normal-frame raster/RT and compositor native gates pass. Transient source projection and periodic brick atlas integration are implemented pending the end-to-end wall gate. Render validation uses a visible editor/window (`MATTER_HIDE_WINDOW=0`).

## First textured wall

Keep the direct wall shell, including closed portal reveals and its matching collision volumes. Bake eight physical brick source recipes into front/back face images, then stamp those images into the existing chart-VT compositor. Source bricks are bake inputs, not thousands of runtime children. Placements remain proper rigid transforms.

The first planar wall can evaluate a running-bond recipe from `VtSurfacePoint.pos` (part-local metres) in `vt_composite.comp`. Persist physical brick dimensions, seed/row phase, local wall frame and source-atlas digest in a versioned surface-recipe binding. `vt_chart_resolve.glsl` already supplies the owning triangle, barycentrics, position and interpolated normal. Chart UV remains solely the runtime page address; no new raster vertex UV channel is needed for this first milestone.

Use the existing `.gtex` serialization/compression/upload infrastructure, with an explicit finite-face layout mode. Eight recipes times two faces gives sixteen layers, but they are not Wang tiles: each layer needs a physical UV domain, rigid face frame, guard rectangle, signed metric height range and coverage. Rectangular faces can occupy padded square layers without stretching. Sample them with clamped finite coordinates. Coverage selects mortar or brick in the wall compositor. Preserve layer metadata in cache identity.

`solid_sdf_meshing.h` and `solid_field.glsl` expose the same restricted source tape and gradient. The version-1 primitive fields are 1-Lipschitz under proper rigid transforms; polynomial smooth min/max preserve this bound. A projection kernel still needs conservative finite-AABB traversal, floating-point margin, bounded iteration, crossing refinement and coverage/normal oracle tests. Future arbitrary displacement cannot inherit this step bound without proof.

Start with opt-in 512 texels/metre for the wall and source faces, then measure actual packed density, page residency and projection cost. `render/part_store.cpp` currently requests 16; `lod_bake.cpp` can reduce density to fit. Report both requested and achieved density. Height presently affects VT blending but is not a persisted VT output channel, so the first result is normal-mapped relief. Height/POM needs a separate explicit channel and raster/RT agreement.

## Normal-frame prerequisite

Current chart composition encodes a part-local normal against projected local +X, while raster and RT decode against projected world +X. Rotated instances misorient detail. Near +/-X the current frame also discards detail. Use one robust local frame in a shared GLSL include: project +X unless nearly parallel, then project +Z; B=N cross T. Encode in this frame, decode locally, and transform the decoded normal using the inverse transpose of the instance linear transform. Keep ground Wang helper semantics separate.

Raster must retain the interpolated object normal until fragment decode; interpolating tangents computed independently at vertices does not reproduce the nonlinear frame built around an interpolated normal. Transport a flat normal matrix. RT can build the transformed local basis at the hit and carry its three columns to raygen; no current raygen descriptor provides per-instance transforms. Preserve geometric normals for visibility and ray offsets. Apply existing RT back-face orientation consistently to the complete decoded normal. Nonuniform nonsingular transforms use the full inverse transpose, not normalized basis columns.

Required gates: directional normal under 0/15/45/90 degree rigid rotations; +/-X and near-threshold normals; neutral identity; nonuniform-scale oracle; curved interpolated normals; BC5 tolerance; raster/RT readback parity; zero Vulkan validation errors. Capture visible output with a grazing light. The implementation must document varying and RT payload costs.

## Later surface provenance

For curved walls, reveal ownership and timber end-grain, preserve per-triangle surface identity and three metric source UVs separately from `TriEx.uv0/1/2`, which chart packing overwrites. Persist a versioned, topology-validated bundle section; reorder the parallel provenance with `ChartAtlasRung.tri_order` in `vt_chart_gpu.h`; expose it through appended `VtPartContext` fields and a separate GPU buffer. Do not reuse material-weight lanes. Include analytic mapping/frame metadata for curved surfaces: UVs alone do not define normal orientation.

Main touchpoints are `part_bundle.h`, `part_asset_v2.*`, `render/part_store.cpp`, `render/vt_types.h`, `render/vt_chart_gpu.h`, `render/vt_compositor.*`, `tileset_gtex.*` and existing VT shaders. Cache identity includes source recipe/dependencies, projection version, finite layout, frame convention, requested density and appearance recipe. Invalidate cached compositor streams and resident pages together. There is currently no persistent VT page cache, so precomposed per-module charts would require more storage machinery than the recipe-driven compositor approach.

## Prerequisite implementation and commands

`vt_normal_frame.glsl` is shared by the compositor and both consumers. Raster keeps the existing normal varying local only on VT draws, adds a flat inverse-transpose matrix at locations 18–20, and requires an interface range of 84 components (checked explicitly at renderer initialization). Static, skinned and direct-water vertex shaders compile from the same `raster.vert`, so all producers write the contract. Impostor/chartless world-normal behavior is unchanged. RT adds nine scalar payload words (36 bytes), storing the transformed local frame at the hit. `kVtBakeVersion` is 3. This does not solve deformation-aware tangent transport for an independently skinned texture atlas.

CPU reference: `node tests/vt_normal_frame_reference_tests.mjs` (480 transformed probes). Native GPU compositor test retains directional detail on local +/-X faces. The visible full renderer fixture is `MATTER_VK_SMOKE_MODE=vt-normal-frame`, using the canonical native `vulkan_smoke_tests.exe`; it generates a constant directional `.gtex`, runs yaw and roll at 0/15/45/90 degrees, and compares G-buffer and RT world normals with an analytic oracle and each other. Near-band detail is disabled for this page-normal isolation gate. The fixture writes RT normal readback into the existing output words 29–31; no readback buffer expansion is needed. Native visible normal-frame and compositor gates passed on 2026-09-11, with no validation errors.

## First wall using the existing texture transport

The first wall composes 16 transient finite source faces into one genuinely
periodic 1.25 m running-bond image, then repeats that image in all 16 existing
Wang layers. This preserves the existing `.gtex` contract and reuses upload,
compression, triplanar sampling and chart-VT shading without a new runtime
finite-face format. It is a valid periodic texture with eight brick shapes,
not 16 independently randomized Wang tiles. Per-face provenance and richer
non-periodic damage remain later extensions.

`defineMaterial(...,{detail:'CastleBrickBondDetail'})` selects a declarative
source recipe. The provider evaluates eight physical source variants without
meshing or writing source bundles, projects their front/back faces, composes
the atlas and persists only the finished `.gtex`. Cache identity includes the
authored descriptor, folded source dependencies, each actual field recipe,
projection algorithm and bond/palette recipe. A failed projection or upload
must fail the detail operation rather than advertise a completed textured wall.

`CastleWallBakeProof` places two rigid copies of one low-poly 4 × 3 m wall. Its
explicit 512 t/m runtime chart-density override exposes the small source detail;
it is not yet a per-part production density policy. The first milestone uses
normal and material channels. Wall height/parallax is now connected through
`detailMode: 'surface'`; see `castle-surface-parallax.md`.


The first wall now passes visible raster and RT capture, including a 30-degree
rotated instance. The wall is 12 triangles and no source brick meshes/bundles
are created. See the `CastleWallBakeProof` README for fresh/cached timings and
capture locations. This establishes SDF-to-texture transport; finite unique
assembly projection, curved/reveal provenance, production
texel-density policy, and subsecond texture loading remain open. Wall parallax
now uses the finished-material path described in `castle-surface-parallax.md`.


### Finished-detail near-band policy

The first capture exposed an existing convention mismatch in the live near
band: the compositor uses part-local positions and +V source normals, while
the G-buffer near overlay re-samples in world coordinates using the legacy
inverted-green basis. Native RT primary visibility also uses this G-buffer.
For the accepted first-wall capture both near-band distance and fade were
set to 0 and 0.1 m respectively. Completed geometry-derived detail should
opt out per material: correcting coordinates alone would still apply the
same frequency of normal relief twice. Preserve legacy landscape modulation
for materials that need it. This policy is now implemented by `detailMode: 'surface'`: the finished
material samples its local height and channels directly and bypasses the
legacy overlay. See `castle-surface-parallax.md` for validation and limits.


### Immediate texture-load optimization

`VkSceneRenderer::load_tileset_slot` currently decodes the entire `.gtex`,
slices all channels into 16 layers, generates their CPU mip chains, and
BC7/BC5/BC4-compresses them on every load before staging/upload and a device
idle. Thus the measured `load_upload_ms` includes texture processing, not
just transfer. The first periodic wall uses identical layers, so it also
repeats equivalent work 16 times. A GPU-ready final cache should retain
compressed layers/mips (with version/format/dimension validation), and the
periodic input should prepare one unique layer once. Reuse existing encoders
for generation; warm loads should read ready bytes and upload. Profile decode,
mip/encode, allocation/copy and submit separately before claiming a specific
percentage of the measured second.
