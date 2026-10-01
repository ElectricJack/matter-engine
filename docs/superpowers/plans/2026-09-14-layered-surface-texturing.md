# Layered surface texturing implementation plan

Date: 2026-09-14. Revised: 2026-09-16. Status: active visual-development priority; implementation and acceptance pending.

Design: [layered terrain and building materials](../specs/2026-09-14-layered-surface-texturing-design.md).
Foundation: [VT reliability implementation](2026-09-14-vt-reliability-and-throughput.md).
Evidence: [texturing investigation](../../findings/texturing-system-review-2026-09-14.md).

## Delivery contract

Deliver faster source loading, consistent material/height blending, less repeated/busy detail, cached local/world splats, and terrain/building contact blending. Keep POM and existing procedural content. Establish the shared semantics before converting content broadly.

### Current priority — StreamMountain, 2026-09-16

The user requested a shift from the brick wall proof to cleaner, more realistic
StreamMountain texturing and faster generation, with less reliance on settling.
Resume the broader wall work after a useful terrain material checkpoint. Its
latest validated boundary is the cached material read lease; sparse layer
composition and connected-wall acceptance remain open.

- Replace the mountain's repeated terrain-detail atlases with a continuous
  world-space GPU material using the existing direct-source VT path. Author
  muted soil/vegetation/mineral variation at metre, landscape and grain scales;
  filter unresolved relief using the actual VT footprint.
- Retain terrain shape, forest placement, scene lighting and streaming distances
  during comparison. Keep optional Wang/geometry-baked sources available to
  other scenes and to deliberately selected material stamps.
- Remove terrain source geometry/physics preparation from this scene, and avoid
  evaluating appearance when CPU geometry classification only needs a constant
  carrier material. Measure source preparation separately from forest generation,
  VT refinement and frame rendering.
- Validate the real authored recipe through the native loader/compiler and GPU
  budgets, check world anchoring and height bounds, and capture overview, close
  and grazing views. Keep complete source/binary manifests. A cleaner first pass
  is not final material or performance acceptance.
- Close review found the legacy 16 texels/m terrain chart ceiling filters out
  centimetre detail. The generic `streaming.terrainTexelsPerMeter` setting now
  reaches native terrain staging, preserving the default for existing scenes and
  nested density scaling. StreamMountain requests 64; check actual packing,
  visible refinement and indirection cost before accepting this choice.
- The user explicitly requires useful POM in the new terrain materials. Author
  resolvable embedded stone profiles, shallow soil depressions and weathered rock
  relief in metres; derive normal, pigment and roughness from the same features.
  Keep unresolved microdetail out of the height march. Check POM enabled/disabled
  at matched close and grazing cameras, along with mip transitions, terrain chart
  boundaries and the shared raster/RT path. Merely adding noisy normal maps does
  not satisfy this requirement.
  The [POM material checkpoint](../../agent/evidence/2026-09-17-mountain-pom/README.md)
  records the 184-op recipe, sampled metre relief, paired footprint averages and
  passing native source/POM gates. Generic adjacent-mip sampling is still open:
  the initial implementation exceeded the shader optimizer's ID budget and was
  removed from production pending a smaller sampler/connected-walk design.

The [StreamMountain checkpoint](../../agent/evidence/2026-09-16-streammountain-materials/README.md)
records changes, captures, timing limits and remaining work. Rock/soil/moss detail,
cross-object contacts, authored splats and the original acceptance targets remain
part of the active goal.

The user reprioritized this work on 2026-09-15: develop the visual results now, retaining VT correctness checks and lightweight timing, then finish strict performance acceptance once the material model is established. Original VT targets remain unchanged and unproven. Task numbers are stable identifiers, not a mandatory serial order: start L0, the required L1 fixes, L2a and L3; exercise L4–L7 through the same proofs. Complete L2/L2b as their generation methods require them, without making a comprehensive source-cache rewrite a prerequisite to seeing new materials.

### Current working objective — user revision, 2026-09-15

Implement and visually validate surface-specific procedural materials for terrain and buildings under this plan and its linked design. Make GPU DSP/SDF generation the primary source path, support analytically placed instanced geometry, and retain explicitly selected, cached physics for arrangements that need it. Preserve optional Wang sources and legacy content. Generate stable authored/procedural splats from the same material and coverage recipes, compose coherent color/normal/ORM/height into VT, preserve POM and raster/RT compatibility, and demonstrate varied brickwork, peeling paint, natural terrain and cross-object moss/soil/dampness. Start with a quiet rock/soil/moss slope and a weathered brick wall. Iterate against user visual feedback before broad conversion. Preserve unrelated work and the completed VT stability changes; measure preview/final latency and memory, and fix regressions that prevent useful iteration. Strict original VT timing/latency acceptance is deferred, not relaxed or marked complete. Finish with native correctness evidence, repeatable visual captures, documented limits and completion of the deferred VT correctness/performance gates.

The user updated and activated the app goal on 2026-09-15 with the summarized visual-first objective above: procedural sources and splats, coherent channels/POM, terrain/building proofs, retained VT stability and deferred acceptance against the existing targets. The goal record now matches this direction. The previous VT acceptance work remains open under L8; activating the revised goal does not establish completion of those gates.

Follow the canonical MSVC workflow and preserve concurrent working-tree changes. New files named below are proposed implementation locations; confirm existing ownership before creating them. Reuse existing source modules, shader helpers, asset storage and capture infrastructure. Register new compilation units/tests in the actual CMake/Make inventories and rebuild embedded SPIR-V through the supported build graph.

## L0 — Freeze material proofs and reference measurements

**Files/content:** StreamMountain and CastleUpgraded reference cameras; an isolated scene/fixture under `projects/world_demo/scenes/` if needed; evidence manifests and existing capture tooling.

- [ ] Establish a rock/soil/moss slope and an SDF brick wall with peeling paint and a ground contact. Both initial recipes must require no physics. Record physical scale, materials/seeds, source hashes, height ranges, normal strength, lighting/exposure and camera distances.
- [ ] Capture close/middle/far, grazing-light, moving-camera and RT views for the current implementation. Include unlit color, normals, roughness, height and material identity where diagnostics support them.
- [ ] Baseline fresh generation, GTEX hit, source upload, first drawable surface, VT refinement and steady shading separately. Reuse the CPU inspection diagnostic for source statistics; native timings remain authoritative for runtime acceptance.
- [ ] Define matched legacy/candidate toggles and keep reference assets identifiable. Art iteration must not silently change the performance fixture.

**Exit:** reproducible appearance and performance references covering both terrain and constructed surfaces.

## L1 — Correct current material sampling and frequency ownership

**Files:** `MatterEngine3/shaders_vk/gbuffer.frag`, `vt_common.glsl`, `vt_composite.comp`, shared detail/RT helpers; auxiliary page-format/sampler code in `src/render/vt_*` and `vk_scene_renderer.*`; relevant GPU tests.

- [x] Keep the current 256-entry material table's auxiliary records lossless through storage and sampling. The existing RGBA8 channel stores both u8 IDs exactly and bypasses BC encoding; the shared sampler now fetches the entire ID/weight record at one texel instead of filtering material numbers. A native 30/34 boundary regression fails before the change and passes after it, with no page regeneration on unused-material edits. [Evidence](../../agent/evidence/2026-09-15-material-identity/README.md). Any future smooth reconstruction must gather IDs and combine weights by identity.
- [ ] Test adjacent IDs such as 30 and 34; ID 32 must never appear from reconstruction. Test both stored contributors and endpoint weights, including page/mip transitions.
  The unused-ID boundary case now passes at five subtexel positions through the real compositor/G-buffer; explicit secondary-contributor, weight-endpoint and transition cases remain open.
- [ ] Make current near material/height evaluation respect the same contributor weights as the page path. Keep finished-surface POM's working behavior until its composed replacement is proven.
- [ ] Remove duplicated frequency contribution from the terrain overlay. Compare complete single material sampling with any proposed matched residual using fixed source/mapping/footprint tests. Preserve normal-frame and ORM conventions.
- [ ] Add focused raster/RT fixtures showing smooth transitions and unchanged uniform-material endpoints.

**Exit:** no invented material IDs, no single-material near-detail override of a two-material blend, and no uncontrolled duplicate application of the same relief/color signal.

## L2 — Prepare source textures once

**Files:** `src/tileset_gtex.*`, `src/render/tileset_slicer.*`, `bc_encode.*`, `vk_scene_renderer.*` source loading/publication, `src/tileset_bake.cpp` and `src/render/tileset_bake_vk.*` only where preparation/publication hooks are needed. Add a prepared-artifact reader/writer and tests beside the existing texture modules; use existing asset-storage helpers.

- [ ] Implement the versioned source-artifact contract from the design, including dependency key, checked directory, channel/layer/mip formats, physical height metadata and integrity validation.
- [ ] Build the artifact after generation or a GTEX miss. Deduplicate identical layer preparation and payloads while preserving the logical tile layout expected by shaders. Avoid changing runtime tile addressing merely to deduplicate CPU work.
- [ ] On a prepared hit, read final payloads and upload asynchronously with zero decode/slice/mip/encode work. Publish through immutable source snapshots and VT dependency invalidation.
- [ ] Test round trips, exact mip contents and formats, corrupt/truncated/oversized records, invalid offsets/overlaps, checksum/version mismatches, unsupported device formats, cancellation, concurrent readers and failure preserving the active source.
- [ ] Measure three native runs for each reference source/cache state. Report median/p95 where sample count supports it; collect enough repetitions for a meaningful percentile. Record bytes, peak preparation memory, CPU stages and upload time separately.

**Exit:** a valid prepared hit proves zero texture preparation stages and materially improves native warm loading without output changes. No user cache needs blanket deletion to regenerate a changed source.

## L2a — Implement direct DSP/SDF material generation

**Files:** inspect existing field/tape evaluation, authoring bindings, `tileset_spec.*`, material bake shaders and `vt_composite.comp` before selecting new module boundaries. Reuse common operators and the L3 material-sample contract. Proposed modules are not existing APIs.

- [ ] Define a versioned recipe/evaluation contract with physical coordinates, stable feature identity, footprint, read-only context and coherent material/height/coverage outputs. Distinguish height-derived normals from residual detail.
- [ ] Implement the small operator set needed by the proofs: ramps/remaps, filtered noise, bounded domain warps, 2D distance shapes, bevel/height profiles and masks. Compile per-texel GPU evaluation through existing authoring facilities; a node UI is not required.
- [ ] Permit direct field evaluation into composed pages without generating a Wang atlas, a repeated source tile, or a physics world. Preserve prepared images and geometry stamps as alternative source types.
- [ ] Declare finite filter/warp support, generate expanded regions and crop, and propagate that support into dirty bounds. Prepare stable larger-domain artifacts for nonlocal operations such as distance transforms.
- [ ] Test simple analytic values and CPU/GPU tolerances, correlated channels, page/region seam equivalence to an untiled reference, repeat generation after eviction, stable identity across mips/resolution, and bounded cancellation/failure behavior.
- [ ] Build the SDF brick/paint proof and field-based terrain proof early. Capture parameter edits and distinguish compilation, evaluation, composition, encoding and upload costs. Record actual preview latency; do not describe unmeasured graphs as instantaneous.

**Exit:** both proof recipes use the direct field path with zero physics work; generation is spatially coherent and the resulting material channels agree. Visual review and broader performance acceptance remain explicit.

**L2a progress (2026-09-15):** direct-source v1 now records complete RGB/ORM/metre-height
outputs through `surfaces(s).source`, carries footprint to the GPU program,
and composes pages without a source atlas or physics bake. Analytic GPU readback
checks cover two mips, height-derived normals, eviction regeneration and source
edits; raster/RT integration and the affected legacy gates pass. This is partial L2a work: it does
not complete the recipe/filter/seam contract or visual acceptance. Subsequent
L3/L5 work supplies bounded layer/coverage helpers; L4 stores composed height and
connects its first shared raster/RT POM implementation. Native brick/paint and
rock/soil/moss prototypes now have
close/middle/far/grazing-camera/RT captures at explicit review density and exposure.
The brick uses a tested integer cell-hash operator; both recipes use the shared
height-aware authoring helper. They remain early art, with lighting differences,
fixture cleanup and the full filtered/seam contract open. See the design's
implementation boundary, [initial implementation evidence](../../agent/evidence/2026-09-15-direct-source/README.md)
and [current material checkpoint](../../agent/evidence/2026-09-15-material-look/README.md).

## L2b — Separate geometry placement, optional settling and material baking

**Files:** `tileset_placement.*`, `tileset_settle.*`, `tileset_bake.*`, `tileset_spec.h`, authoring bindings, cache/versioning and geometry bake tests. Preserve existing recipe semantics through versioning.

- [ ] Make physics explicit for new generation recipes, preserving legacy defaults through versioning.
- [x] Bypass world creation/finalization when no placement requires simulation. Native bake tests verify no world is created for analytic-only or empty physical layers, preserve all 48 analytic transforms/order, and retain a physical positive control. Bake and physics suites pass; the editor rebuilds. [Implementation evidence](../../agent/evidence/2026-09-15-procedural-generation/README.md). This does not complete the direct DSP/SDF path or change existing physical recipe settings.
- [ ] Reuse uniform/Poisson/cluster placement and add the proof-required surface alignment, curve/structural placement, density masks and bounded overlap rejection. Preserve deterministic feature IDs across region traversal and edits.
- [ ] Separate cached prototype geometry, placement/settled transforms and material bake outputs. Prove that color/roughness edits reuse poses and that geometry, scale, collider or solver changes invalidate the correct dependencies.
- [ ] Expose deterministic preview/final settling profiles. Compare reduced substeps, tick/duration caps and final relaxation on identical seeded debris; report convergence, contact/overlap quality and stage timings. Existing sleep-based early exit is baseline behavior.
- [ ] Bake analytically placed or settled geometry into reusable color/normal/ORM/height/coverage stamps, with projection-error and relief bounds. Use the same stamp sources in base materials and generated splats.
- [ ] Test no-simulation, mixed placement, physics-required, preview/final cache separation, changed collider, cancellation and retained valid output. Do not silently choose final poses using wall-clock timing.

**Exit:** physics is used only when requested by the new recipe, material edits reuse placement, and geometry stamps share the field/layer composition contract.

## L3 — Implement the shared bounded layer evaluator

**Files:** proposed `src/surface_layers.h/.cpp`, shared layer evaluation GLSL, `vt_composite.comp`, existing material/surface tape authoring and serialization modules; compositor tests and small CPU semantic fixtures.

- [ ] Define deterministic layer records and validation for anchor, projection, coverage, operation, ordering/seed, channel controls and field/image/geometry-stamp dependencies. Reject unsupported/malformed records with useful author-facing errors.
- [ ] Implement the design's reference height-aware blend, channel conventions, physical units and stable accumulation order. Add zero/full coverage, equal-height, height-bias, scale, normal-frame and material endpoint cases.
- [ ] Use a CPU semantic oracle for small layer fixtures and compare GPU results within declared format tolerances. Keep it as a reference evaluator, not a duplicate residency implementation.
- [ ] Compose ordered contributor chunks through bounded intermediates. Test more than eight overlapping layers, cancellation and allocation failure; never publish a partial stack as complete.
- [ ] Record the exact dependencies of a composed page and use VT's local invalidation/replacement machinery. Exercise source edit, layer reorder, deletion and repeated edits while work is pending.

**Exit:** the same stack yields deterministic, coherent channels with bounded temporary work and complete publication.

**L3 progress (2026-09-15):** `s.layer` now lowers ordered replacement,
nonnegative deposit and appearance-only blends into the shared scalar evaluator,
including physical height-aware coverage and squared roughness. Both proofs use
it. Native JS/CPU semantic fixtures pass and native material captures exist.
Direct sources now support 512 operations through register reuse within the
existing 96-register shader workspace and fixed instruction arena. A ten-layer,
196-op native authoring fixture matches the CPU oracle; a separate maximum-length
GPU recipe passes channel/normal, mip, regeneration and edit checks. Allocation,
fragmentation, parser overflow and excessive live values have explicit checks.
This does not complete L3: record validation/projection, chunked contributors,
local dependency tracking and the full GPU layer/overflow/seam matrix remain open.
[Visual proofs](../../agent/evidence/2026-09-15-material-look/README.md) and
[capacity checkpoint](../../agent/evidence/2026-09-15-bounded-layers/README.md).

## L4 — Add composed height and preserve POM

**Files:** `vt_types.h`, pool/slot accounting and copies in `vt_residency.*`, compositor output/encoding, `vt_common.glsl`, `surface_detail.glsl`, raster/RT material consumers and associated descriptors; extend native POM and VT seam tests.

- [x] Add the initial `R16_UNORM` height output and snapshot-level metre datum/range. Update every channel-count/format/binding/byte-size/serialization consumer and feature compatibility key.
- [ ] Produce representative height mips and valid borders. Supply chart metrics and valid-region information for view-ray travel, including nonuniform transforms.
- [ ] March composed height with resident/coarser lookup at each required position. Sample all final channels at the displaced coordinate. Bound chart-boundary and grazing travel; filter/fade unresolved relief consistently.
- [ ] Test constant recess against analytic depth, two-layer seams, height/normal/color registration, page/mip transitions, absent fine pages, failed replacement, rotated/scaled instances and raster/secondary-RT agreement.
- [ ] Preserve proxy-based RT visibility/origins. Keep the current direct surface route available until the new route passes its existing `surface-parallax` cases plus layered cases.
- [ ] Measure incremental pool memory, page-production time and near shading cost. Run affected VT correctness gates with the enlarged format; retain complete timing/latency acceptance for the deferred performance pass. Include the added channel in accounting throughout.

**Exit:** layered POM has a single coherent height/material result, valid coarse fallback and no regression in the established surface-parallax contract.

**L4 storage progress (2026-09-15):** five-channel VT now stores R16 composed
height. A 16-byte per-slot metadata record publishes input-bank identity and the
immutable source's metre minimum/range/version together with the pixels. Failed
or stale candidates retain prior height/decode; compatible input-bank rebinding
preserves height. Fixed MiB pool sizing includes all nine bytes per texel.
Native analytic two-mip, negative/constant height, adjacent-page gutter,
regeneration, legacy reset and queue-ordered publication cases pass. Existing
renderer/POM, base VT and no-RT compositor checks pass; the editor builds. This
is a live GPU ABI with no persisted physical-page serialization to migrate;
source/part content serialization is unchanged. Details, source hashes and the
memory tradeoff are in the [storage checkpoint](../../agent/evidence/2026-09-15-composed-height/README.md).

**L4 POM progress (2026-09-15):** raster and secondary RT now share a bounded
composed-height marcher with per-position residency lookup and final color /
normal / ORM sampling at the displaced coordinate. Direct AUX records reuse
existing bytes for chart identity and interior/padding validity; no additional
pool image is required. Chart metrics handle rotation and nonuniform scaling.
The primary pixel carries a composed-height flag so lighting/shadow origins can
recover the proxy without relying on the carrier material's legacy flag.
Native analytic depth, curved color/normal registration, three transformed
instances, coarse fallback, chart-edge rejection, broad-footprint fading,
travel limits and 41 deepest-envelope ray angles pass. Existing compositor,
direct-source, surface-parallax and feedback gates pass, with zero Vulkan
validation errors. Paired native POM-on/off captures exist for both proofs at
five views. [Implementation, validation and images](../../agent/evidence/2026-09-15-composed-pom/README.md).
Representative nonlinear filtering, complete layered page/mip/seam cases and
incremental timing remain open. Terrain captures retain visible thin breaks
that need chart-boundary investigation, and both scenes need art/lighting work.
L4's exit condition and the overall visual/performance acceptance are not met.

**L4 boundary diagnosis (2026-09-15):** native status overlays identify the
terrain's thin breaks as chart-footprint rejection, with travel-limit failures
appearing separately near the distant silhouette. A new `vt-composed-seam`
acceptance mode deliberately fails: for a continuous 10 mm recess across two
packed charts, 9 of 41 samples return flat from each viewing direction. Existing
composed-POM, direct-source, legacy surface-parallax and feedback regressions
pass with zero Vulkan validation errors. The actual continuity fix is open;
GPU picks now identify the same part on both sides of the observed cut. An
opt-in manifold-neighbor preparation helper passes native adjacency and packing
tests, with no GPU format growth; it has no production consumer yet. Implement
and test connected-surface traversal while preserving
safe rejection of unrelated faces. [Diagnostics, images and failing acceptance](../../agent/evidence/2026-09-15-pom-boundaries/README.md).

**L4 connected-geometry publication (2026-09-16):** production preparation now
builds the manifold-neighbor stream. Direct-source pages publish owned device
addresses and bounds for the compositor's existing immutable geometry, with
generation-checked publication and retirement for earlier readers. The native
compositor and residency-queue tests pass geometry reuse, rejected/stale fills,
compatible retags, replacement and final release. Metadata grows from 16 to 48
bytes per physical slot; chart/triangle rows are unchanged.

**L4 connected traversal (2026-09-16):** the shared raster/RT marcher now follows
those neighbors, remaps charts, joins recessed faces at their edge bisector and
folds bilinear taps through connected bends. Planar continuity passes all 82
rays; -45/+45/90-degree fixtures exercise actual face crossings and remain below
0.002 mm position error. True gaps retain the valid proxy fallback. The composed
parallax, seam, queue, input-snapshot, direct-source and legacy surface-parallax
smoke modes pass; the compositor passes 667 fills with no skipped pages or Vulkan
validation errors. **L4 remains open:** actual scene visual review, streamed-part
and LOD continuity, the complete channel/filter matrix and performance acceptance
are still required. [Implementation, limits and native evidence](../../agent/evidence/2026-09-16-connected-pom/README.md).

The subsequent diagonal-chart fixture exposed 18 flat rays out of 82 despite
the axis-aligned checks passing. Geometry-proven same-chart edge sampling now
passes all 82 rays and 41 additional disconnected-gap controls; composed POM and
input-snapshot regressions pass. Maze and terrain captures are recorded; the
matched terrain reference diagnostic now has zero initial/path boundary
rejections (previously 520/1,287 pixels). This resolves that view's false flat
strips, while cross-part/LOD cases and final visual acceptance remain open.
The terrain proof's saved streaming radius now matches its authored 24 m
instead of inadvertently requesting 3,871 m; capture hashes include saved JSON
settings, and visual capture success requires empty VT queues for every view.

## L5 — Introduce cached local and world splats

**Files:** layer records/compiler, JS DSL bindings and shared authoring helpers, surface/page spatial candidate data, compositor, dependency tracking and tests. Extend the existing authoring system rather than building a separate decal renderer/editor first.

- [ ] Add bounded box/ellipsoid splats, planar/triplanar projection, local/instance/world anchors, receiver tags, normal/depth limits and deterministic seeds. Add spline strips after basic projection/editing passes.
- [ ] Combine a material recipe, a coverage recipe and a placement in each splat. Support direct DSP/SDF contents and prepared geometry/image stamps through the same evaluator as base materials.
- [ ] Add procedural splat generation from stable spatial/structural candidates and upstream surface context, plus directly authored records and persistent manual overrides. Continuous broad fields must not allocate a splat per texel or grain.
- [ ] Generate candidates independently of camera/page order, include neighboring influence, deduplicate by stable identity and preserve unrelated candidates during local edits. Never rerun source physics per receiver or VT page.
- [ ] Build spatial page/subtile candidate lists. Evaluate only relevant splats, with ordered chunking from L3. Validate deletion/movement dirties old/new bounds including support, gutters and coarse mips.
- [ ] Implement sparse placement-specific overridden pages over shared base pages. Include placement/context identity only where needed, preserve shared base ownership and alias identical contexts.
- [ ] Test two instances with different world dirt patches, an unaffected third instance, overlapping/reordered splats, repeated moves, owner deletion and large instance counts. Verify local-anchored texture identity survives object movement.
- [ ] Test paint removal revealing the composed brick/mortar substrate, moss thickness sharing its coverage/normal mask, appearance-only stains preserving height, region-border candidates and mixed manual/procedural records. Verify dependency edits reuse placement and affect only dependent pages.
- [ ] Ensure unchanged splats cause zero page regeneration in a settled scene and no per-splat draw calls. Record candidate count, evaluated layers, update latency, override bytes and steady shading cost.

**L5 progress (2026-09-15):** bounded box/ellipsoid coverage and `s.splat` authoring
now lower local/world placements, optional rotated axes, and material contents
through the existing layer evaluator. Native CPU reference cases cover signed
coordinates, world translation, footprint widths and all three operations across
RGB/ORM/height. New native brick and terrain captures show authored patches baked
into VT. This is not the record/index/invalidation system: placements still
compile into whole-surface programs, with no stable record IDs, receiver filters,
local dirty bounds, sparse overrides or complete subpixel filtering.
[Evidence and current images](../../agent/evidence/2026-09-15-weathering/README.md).

**Exit:** authored patches produce organic material transitions on both scene families, edit locally and preserve VT stability/memory limits.

**L5 storage groundwork (2026-09-16, native CPU/six GPU modes pass):** receiver
coverage/chart metadata and connected-POM geometry now address a separate
material-pixel allocation. The allocator supports explicit identical-payload
sharing, copy-on-write edits, rejoining a base and deferred retirement. Private
coverage and material sampling use independent addresses in raster/RT POM.
The wall compositor still supplies private payload identities; canonical periodic
page mapping, early reuse before composition, sparse weathering/local invalidation,
and context-specific enrichment remain open. This does not complete L5 or prove
lower reserved GPU memory. [Evidence](../../agent/evidence/2026-09-16-shared-vt-pixels/README.md).

## L6 — Improve source appearance and variation across distances

**Files/content:** Alpine source recipes, brick/wood source generation, `tileset_spec.h`/primary bake only where richer source channel evaluation is needed, existing StreamMountain environmental fields, layer authoring helpers and the material proofs.

**Current masonry priority — user revision, 2026-09-15:** bake from actual
detailed 3D clay bricks with rounded/chipped edges, dents/divots and textured
faces. Keep the full layered-material objective; the source-shape preview alone
does not satisfy the requested low-poly wall/VT result.

**Whole-brick wall sizing — additional user requirement, 2026-09-15:** make a
shared physical brick layout authoritative for wall extents, geometry and VT
mapping. Exposed ends/tops must contain complete bricks/courses. Ordinary
cropped running-bond tiles do not satisfy this requirement.

**Whole-wall receiver correction — user clarification, 2026-09-15:** the proof
must render three boxes, twelve triangles each. The detailed brick assembly is
bake input; individual low-poly bricks and mortar boxes are not the requested
runtime representation. Implement the [whole-wall composition sequence](../specs/2026-09-15-whole-wall-vt-composition.md):

- [x] Add generic composite-receiver declarations with a reusable source table
  and stable rigid placements; keep brick generation and layout in JS.
- [ ] Deduplicate source preparation and GPU payload uploads across wall sizes.
- [ ] Gather bounded candidates per requested VT page and compose structural
  depth/coverage plus coherent color/normal/ORM/height over recessed mortar.
- [x] Emit exactly one box per wall; demonstrate 36 triangles total, all six
  face orientations, non-flat baked detail and POM on/off. Native evidence:
  `2026-09-15-wall-surface/composite-walls-v2`, 22 views, zero validation errors.
- [ ] Compare every exposed face against the detailed reference, finish POM
  edge/RT agreement, and accept visual quality. Dark rear/left captures alone
  do not establish fine-detail equivalence.
- [ ] Apply wall-coordinate moss/peeling-paint/repair splats across bricks and
  joints through the same source/candidate/composition machinery.
- [ ] Reuse a periodic interior module across 1x, 2x and 4x wall dimensions,
  with metre-scale UVs, wrapped filtering/mips and whole-brick end/cap treatments.
  Keep reusable source identity independent of wall dimensions and report page
  storage separately from source payload reuse.
- [ ] Add left/right 90-degree junction proofs from one authoritative corner
  bond layout; validate complete bricks, corner interlock, non-overlap, cap
  consistency and POM inside/outside the turn.
- [ ] Add gentle/tight curved-wall proofs with continuous metric coordinates,
  rigid source bricks and bounded inner/outer joint widths. Test an impossible
  radius explicitly; use a low-poly curved receiver and report its triangle
  count, curvature error and POM seams.


- [x] Implement count-based layout and requested-size snapping with explicit
  fit policy and reported actual dimensions. Preserve unchanged brick IDs
  when extending the wall or adding courses.
- [x] Implement stack and modular `running-headers` solid-panel layouts with
  whole header bricks at both staggered ends, exact thickness, and no 3D brick
  overlap. Provide rigid physical source frames for all six faces. Validate
  40 size/bond combinations, fitting and anchored resize identity in Node.
- [ ] Implement valid whole-brick bond terminations, including modular header
  or return/corner rules for staggered bonds; validate dimensions, thickness,
  joint ranges and 3D non-overlap. Keep the stack-bond case as a simple oracle.
- [ ] Feed one layout into receiver/boundary geometry, finite source face
  selection, mortar and weathering/splats. Include exposed top/end faces and
  whole-brick opening rules; use physical source coordinates without stretching.
- [ ] Demonstrate multiple wall sizes/counts and resizing, with complete
  boundary bricks, coherent raster/RT mapping and stable variation. Add focused
  layout invariants and native top/end/corner captures.

- [ ] Review physical source shapes using `ClayBrickGeometryProof`; inspect the
  same recipes' GPU-projected finite height/normal/coverage channels.
- [ ] Prepare reusable finite brick stamps with physical frames, coverage and
  metre height ranges. Bound projection regions and publish complete faces
  atomically; separate geometry and appearance cache dependencies.
- [x] Evaluate clay color, roughness and fine detail at source-surface points
  through the shared GPU material evaluator. Retain geometric relief rather
  than replacing dents with color noise or a flat palette.
- [x] Add finite-source CPU/GPU sampling with transparent boundaries,
  coverage-weighted mip filtering, squared-roughness filtering and subpixel
  area attenuation. Validate all six faces of eight clay sources. This proves
  the sampler; production layer binding and the compressed artifact remain below.
- [ ] Integrate finite stamp sampling into the shared layer/source evaluator.
  Select variants and placements by stable brick identity, compose mortar and
  weathering into VT, and reuse the source for localized repair/debris splats.
  Verify repeat reduction, displaced channel registration and unchanged-source
  reuse. A periodic atlas or the high-resolution source meshes alone is not
  completion of this milestone.
- [x] Resolve normal-oriented source microheight into projection depth before
  VT composition. Validate a tilted-plane analytic case and all 48 clay faces;
  reject nonconvergent/cancelled results while retaining complete sources.
- [ ] Connect finite C++ receiver bindings to the provider and JS whole-wall
  layout. Use matching brick dimensions for source and receiver, including
  modular headers; keep this separate from general overlapping splat selection.
  - [x] Add a bounded, artifact-free `static finiteSurface(p)` reader with
    owned geometry/material programs and separate geometry/appearance identities.
    Share the World/Part surface recorder; validate all eight clay variants and
    appearance-only edits. Native receiver construction emits twelve triangles.
  - [x] Derive six bounded source-face plans and receiver datum planes from the
    authored physical bounds; use matching source and receiver dimensions.
  - [x] Prepare/cache the six faces through provider-owned queued GPU work and
    publish the complete catalog with the receiver's base program. Read declarations
    on geometry-cache hits as well as cold bakes.
  - [ ] Bind categorical face IDs before VT admission/registration in both eager
    and deferred paths; preserve old complete pages when a source changes.
  - [ ] Switch the whole-wall scene to these receivers and capture all exposed
    faces at multiple sizes. Resolve silhouette/visibility limits explicitly.

  Provider publication and both renderer binding paths are implemented. The
  new `ClayBrickWallSurfaceProof` loads eight six-face catalogs with geometry
  cache hits, but its current captures show flat colors/normals: final display
  acceptance is still failing. Native preparation now also tests the exact
  modular recipes and fixes a filter discontinuity at chipped boundaries.
  [Implementation, passing native checks and failed visual review](../../agent/evidence/2026-09-15-wall-surface/README.md).
  The single-source per-triangle binding is an intermediate diagnostic. Extend
  it to composite candidate sets for the three-box wall target above; do not
  mark the wall milestone complete by fixing the per-brick preview alone.

- [ ] Add correlated source color/roughness/relief with adjustable strength and quiet regions. Preserve physically meaningful units and stable content seeds.
- [ ] Make surface-specific fields and generated features the main natural-material structure. Use stochastic source patches only as an optional ingredient. Retain explicit Wang sources and their edge contract for suitable repeating materials and legacy content.
- [ ] Add bond-aware brick/board variants and wear, avoiding arbitrary rotations that destroy structure. Measure unique layers and periodic correlation alongside images.
- [ ] Tune regional, object-scale and microdetail separately. Reuse existing macro masks and filter small-scale relief away with distance instead of increasing contrast to make it persist.
- [ ] Compare native proof captures at fixed lighting and exposure; iterate from concrete visual feedback. Keep quality settings/artifact identities explicit and remeasure composition/near costs.

**L6 progress (2026-09-15):** the brick proof now uses 255 x 95 mm bond pitch,
per-brick kiln/wear variation, rounded chipped edges, thin raised paint edges,
cavity occlusion preserved under coatings, damp streaks and localized joint moss.
Terrain adds tilted warped strata, quieter grain and bounded damp/moss placement.
Fixed-camera native captures were reviewed and the overly blotchy first moss pass
was revised. Terrain remains too smooth/painted and the blue lighting cast plus
raster/RT appearance difference remain unresolved. Do not count these images as
realism acceptance; improve rock-specific structure and lighting calibration next.

**L6 shape iteration (2026-09-15):** the brick recipe now adds slight course
wobble/offset, per-brick width/height/tilt and clay-tone variation, chipped
outlines, a physical bevel independent of color antialiasing, quieter pitting
and sharper footprint-aware paint loss. Final base bounds include all authored
tilt/grain extrema. Matched original/current captures at 512 texels/metre and
explicit review lighting show reduced small-scale chatter; realism is still
unaccepted. Native albedo/normal/wireframe views narrow the terrain problem:
broad faceting exists without POM, while enabling POM adds sharper color changes
near mesh edges. Chart/triangle traversal is a working hypothesis, not yet an
isolated fix. [Recipes, comparisons and diagnostic evidence](../../agent/evidence/2026-09-15-material-shapes/README.md).

**L6 geometry-source progress (2026-09-15):** eight actual clay solids now have
rounded/chipped edges, deeper dents, 60 small pockmarks and six short scratches
per broad face. Native bounded GPU projection passes all sixteen 1 mm faces;
two complete faces also match the CPU oracle. A separate GPU clay appearance
recipe supplies firing color, roughness and finer grain on a source inspection
mesh. Streamed hosts now reuse the existing queued solid-bake service. Native
source and material captures exist; appearance is still being tuned. The raw
projections contain height/normal/coverage only. Production preparation,
appearance-at-hit baking, finite stamp sampling, wall/splat composition and
realism acceptance remain open. [Source recipes, native checks and images](../../agent/evidence/2026-09-15-clay-brick/README.md).

**L2b/L6 finite preparation checkpoint (2026-09-15):** production preparation now
owns bounded region scheduling on the original face sample lattice and complete
publication. Individual geometry faces have an atomic, validated disk cache;
all sixteen actual clay faces pass a cold/warm GPU test with bit-identical
channels and zero warm projection submissions. Palette/source dependency tests
pass. The existing periodic brick consumer uses this cache; prepared textured
stamps and the new wall consumer remain open.
[Native checks and limits](../../agent/evidence/2026-09-15-face-preparation/README.md).

**L6 whole-wall checkpoint (2026-09-15):** `ClayBrickWall` now uses one shared
physical layout for count-based/fitted dimensions, rigid source instances and
recessed mortar. Stack bond and a modular two-row-thick staggered bond with
whole header ends are implemented. Native raster/RT/top/end captures show
three actual sizes (70 whole bricks), reusing eight source meshes. Native
material handles use scalar parameters; the array transport failure is fixed.
The 40-case Node suite checks layout, non-overlap, fit and resize identity.
This is an inspection representation; low-poly receiver/VT stamp mapping,
openings, corners and splats remain open. Textured top/end projection was
subsequently checked in the source-material checkpoint below.
[Authoring examples, images and limits](../../agent/evidence/2026-09-15-brick-walls/README.md).

**L2b/L6 source-material and filtering checkpoint (2026-09-15):** all 48 clay
faces now carry GPU-evaluated color, ORM, perturbed geometric normals and
normal-oriented microheight at their actual 3D hits. Geometry and appearance
identities stay separate; cancellation/supersession after a GPU batch retains
the prior complete output. The finite sampler has immutable complete mip
chains, physical coordinates, transparent filter support, coverage-weighted
channels, squared roughness and footprint-area attenuation beyond its final
mip. Native tests compare 646,896 GPU samples with the CPU oracle.
This is float preparation/interchange data, not the final compressed artifact
or a production wall VT consumer. Preserve geometry depth and normal-oriented
microheight as separate quantities until receiver projection resolves them.
[Native checks, channel figures, memory costs and next steps](../../agent/evidence/2026-09-15-face-material/README.md).

**L6 finite VT binding checkpoint (2026-09-15):** the first C++ source catalog,
immutable snapshot binding and compositor consumer now exist. Source pixels
are shared across receivers using the same catalog. Composition filters finite
coverage and publishes compressed RGB/normal/ORM, lossless AUX and metre height
together, including a normal contribution from the coverage transition.
Native analytic integration passes color/roughness/height, shared allocation,
in-flight replacement and cancellation of a partially uploaded source. A
349,504-byte source completes across 687 copying frames under an artificial
512-byte/one-allocation quota; repeated preparation requests cannot reset it.
This is a scheduling correctness test, not a runtime latency measurement.
Actual authored-clay page readbacks and the final validation status are recorded
in the [integration evidence](../../agent/evidence/2026-09-15-stamp-vt/README.md).
The JS/provider production wall connection, final source compression/cache,
general splats and visual acceptance remain open.

The eight-source native VT comparison also exposed a BC7 color fringe at
brick/mortar boundaries: componentwise endpoint extrema forced RGB channels
to vary in the same direction. A signed-correlation candidate now wins only
when its exact decoded squared error improves. Across these sources, linear
RGB RMSE decreases to 0.00214–0.00228 from 0.0076–0.0142. Compositor, residency
queue, direct-source, composed/legacy parallax and native editor build checks
pass. The [comparison and complete validation record](../../agent/evidence/2026-09-15-stamp-vt/README.md)
retain the failed attempts and distinguish source precision from final VT
quantization. The known internal-chart POM seam acceptance failure is still open.

**Exit:** retained before/after examples show reduced obvious repetition and busyness with coherent material scale. Record user visual feedback separately from automated functional acceptance.

## L7 — Blend terrain and building contacts

**Files:** layer world anchoring, receiver queries and placement-specific page overrides; existing spatial-query facilities; proposed surface-context records/compiler; contact fixtures.

- [ ] First prove shared world splats crossing terrain/rock/foundation boundaries using depth/facing/contact masks. Include opposite walls, roofs, cliffs, overhangs and stacked-floor rejection tests.
- [ ] Add a sparse 3D surface-context query for automatic soil, dampness and moss inheritance. Build from geometry/base environmental fields so final blended output never recursively feeds itself.
- [ ] Measure candidate index memory/build/query cost and select its concrete representation. Add source dependency/bounds tracking so edited/moved receivers invalidate only affected contextual pages.
- [ ] Validate placement-specific appearance and shared-base reuse under many building/rock instances. Keep vertical receivers and overhangs explicit; a top-down-only demonstration is insufficient.

**Exit:** terrain and constructed surfaces visually meet through consistent material transitions without projection leakage, recursive updates, global invalidation or unbounded per-instance atlases.

## L8 — Integrate, simplify and record acceptance

- [ ] Run complete source-artifact, layer, compositor, POM, seam and contact regressions through native MSVC builds. Exercise supported raster/RT paths and ordinary fallback content.
- [ ] After visual development, complete the deferred VT acceptance camera/edit/pressure and timing sequences with layered content enabled against the unchanged original targets. Report incremental source memory, override memory, height-channel cost, page latency, CPU/GPU time and near shading cost. Earlier visual approval does not mark these gates complete.
- [ ] Retain proof captures and visual feedback at all distances. State remaining art/geometry limits, including proxy silhouettes and visibility, accurately.
- [ ] Remove redundant legacy composition/overlay paths only after their supported materials have a validated replacement. Keep one semantic evaluator and one residency owner; remove temporary migration controls once no content needs them.
- [ ] Publish `docs/findings/layered-surface-texturing-acceptance-<date>.md` with raw artifacts, source identities, commands/exits, measured comparisons and open aesthetic feedback. Update plan/design/index status to match actual acceptance.

**Completion:** functional and performance evidence covers terrain and buildings; source preparation is cached; layered POM and contact blending work; representative visual results have been reviewed. Document creation or one attractive still image does not close the full material feature set.

## Maze example checkpoint — 2026-09-15

- [x] Add JS L/U whole-brick junctions and two rigid-brick curve radii, with three
  exact course heights, to `ClayBrickMaze` (20 walls, 608 wall triangles).
- [x] Support explicit planar receiver groups with rigid off-axis source
  placements; reject positive oriented-source overlaps.
- [x] Share complete projected source banks and GPU pixel/mip uploads across
  different layouts, retaining bounded upload and publication behavior.
- [x] Validate native recipes/provider/compositor, connected nonintersecting
  maze geometry, and 18 raster POM-on/off/debug captures.

Evidence: `docs/agent/evidence/2026-09-15-brick-maze/README.md`. This example
checkpoint leaves periodic composed-page reuse, arbitrary spline receivers,
weathering/splats, POM/RT seams and performance acceptance open.

## StreamMountain priority and POM checkpoint — 2026-09-17

The current visual iteration targets StreamMountain, following the user's
request for faster generation, cleaner organic terrain and useful POM relief.
The JS terrain material uses continuous GPU fields and replaces five terrain
source-atlas jobs; the remaining bark jobs and forest geometry are separate.
No terrain physics settling is required by this recipe.

POM is an explicit material requirement: resolvable stone crowns, recesses and
weathering must have coherent height, normal, pigment and roughness. Fine grain
stays shallow. Filter detail about its spatial mean so distant pages retain
the same overall material. Validate close and grazing profiles as well as
distance/motion transitions; increasing height noise alone does not meet the
realism requirement.

That source probe measured about 4.85 cm of height variation. Native
POM depth, scale, seam and corner checks pass. A targeted shader optimization
checks the existing fade before chart-footprint validation and connected
geometry search; native work counters prove fully faded samples skip those
operations while retaining the nearby depth result. See the
[material captures and limitations](../../agent/evidence/2026-09-17-mountain-pom/README.md)
and [POM work validation](../../agent/evidence/2026-09-17-pom-fade/README.md).
Matched settled scene captures reduce median G-buffer time by 59.6% overview
and 66.9% grazing, with albedo/normal differences bounded by one 8-bit level.
These observations retain the same material and POM quality settings; the
independently open r2 editor prevents treating them as isolated acceptance.

Sharper rock/soil/moss structure, distance-transition quality, contact layers,
moving-camera approval and isolated performance acceptance remain open. The
soft material shapes from that checkpoint are not accepted realism. The frozen r2 asset
export build remains separate from this development iteration.

## StreamMountain cellular relief checkpoint — 2026-09-17

The generic `cellular3` surface operator now supplies nearest-site distance,
distance-gap structure and stable per-site attributes through the shared CPU
and GPU evaluator. JS authors can combine these for base materials or splat
contents. It introduces no terrain-specific C++ authoring function or physics
dependency. Native reference, GPU page/height, regeneration and POM checks pass;
the final JS recipe also passes the native world-loading/GPU-compilation check.

Visual iteration rejected a dense paving-like profile and then smooth,
button-like stones. The revised recipe leaves exposed soil between stones,
uses one fracture field for outlines, face relief and pigment, and lowers stone
contrast. The native probe measures 4.35 cm of relief within the unchanged
[-0.064, 0] m height envelope. The recipe has 260 GPU operations and uses the
same 64 texels/metre density; five former terrain atlas jobs remain absent and
three independent bark jobs remain. Generation is procedural GPU evaluation.

Repeatable native captures retain close/grazing/overview, albedo/normals and a
POM-off control. The third recipe shows rougher stone profiles and clear POM
depth, and is retained as a development baseline. Its capture audit passes,
with zero Vulkan validation errors, an empty settled queue and unchanged
sources/binary. Final visual approval and all distance/motion/contact/isolated
performance gates remain open. Evidence and retained earlier candidates:
[cellular terrain development](../../agent/evidence/2026-09-17-mountain-cellular/README.md).

**Generation follow-up:** CPU/GPU cellular queries now share their distance,
gap and identity search. The GPU compiler proves matching immutable inputs;
the authored mountain source emits one search and two feature reads. Native
runtime, page/regeneration, raster/RT POM and geometry-face material checks
pass, with identical physical channel hashes. A warmed, alternating comparison
using identical shader binaries observes 0.124326 to 0.121253 ms/page (2.47%
lower) in the field fixture. This small, non-isolated result does not establish
whole-scene bake or frame-time acceptance. The final scene audit passes;
overview/close albedo and normals, plus grazing foreground, differ from the
previous recipe capture by at most one 8-bit level. Larger grazing differences
are confined to the distant foliage band; temporal lighting is not frozen.
[Query-reuse evidence](../../agent/evidence/2026-09-17-cellular-reuse/README.md).

## StreamMountain bedrock and deposition checkpoint — 2026-09-17

The next JS recipe adds a second, wider fracture scale, regional stone burial
and rock exposure, and irregular mineral variation visible beyond fine detail.
A first candidate with repeating altitude bands was rejected after native
captures showed contour stripes and excessive pale slab coverage. The retained
revision removes that repeating phase, reduces exposed-rock contrast and breaks
up fracture outlines. Both loose aggregate and substrate contribute real height;
normals derive from that same field. No terrain geometry/source bake, physics,
new DSL primitive or density reduction was added.

Native MSVC validation passes for the actual 352-op recipe (two cellular
searches, three reused feature reads). Sampled relief spans 5.49 cm inside
[-0.090, 0] m. Eighteen settled native images cover overview, ground close and
grazing, frontal/oblique cliff, debug material channels and three POM-off
controls. The capture audit passes with immutable source/binary identities,
zero Vulkan validation errors, restored props and the capture editor closed.
The frozen r2 asset build remains untouched.

POM visibly raises embedded stones, and the cliff material has more structure.
This is a retained development improvement, not final appearance approval:
stones remain too similar, soil/moss too soft, and cliff fractures too plate-like.
Receiver slope may still affect pigment but is rejected as a height dependency
under the current derivative contract. Full material-specific height blending,
contact layers, distance/motion review and actual-scene RT acceptance remain open.

**Performance follow-up:** overview G-buffer was observed at 33.646 ms versus
28.911 ms in the previous cellular-only capture (about 16.4% slower); grazing
and close are similar. These non-isolated timings require matched diagnosis,
not dismissal or a speedup claim. Root bake remains 53.271 s, mostly publish
(40.600 s), with initial streaming at 165.49 s for 2,586 sectors. Added material
work and persistent whole-load costs remain separate issues. Investigate POM
work/bounds/residency while continuing visual refinement; L6/L7/L8 stay open.

[Bedrock/deposition evidence, source snapshots and POM comparisons](../../agent/evidence/2026-09-17-mountain-bedrock/README.md).

## POM cost investigation — 2026-09-17

Matched on/off/on captures confirm substantial POM cost in the current bedrock
scene: overview G-buffer medians combine to 33.275 ms on versus 23.921 ms off,
with identical settled page counts. A private per-ray cache reduced repeated
bilinear chart validation from 37 calls to one in the native straight-ray
fixture while preserving its exact displaced hit. Native seam, bend, snapshot
and raster/RT checks passed, and scene material buffers matched within one
8-bit level.

The measured frame-time change was negligible: overview 0.96% slower, grazing
1.35% faster, close 1.28% slower, with no benefit after subtracting POM-off
controls. The cache was rejected and the original production shader restored,
rebuilt and verified. Keep the additional oblique-ray reference and the complete
comparison evidence. The wider bedrock overview regression remains unresolved;
removing AUX reads alone is not a useful fix. Investigate complete repeated page
resolution/height sampling and the declared-envelope resolution fade before
adding another optimization. Material/contact development and all remaining
acceptance requirements stay active; quality settings were not reduced.

[Rejected POM cache experiment and final restoration](../../agent/evidence/2026-09-17-pom-footprint-reuse/README.md).


## POM route diagnostic and return to materials — 2026-09-17

A second experiment reused immutable page addresses inside the starting desired
page. Eight native gates passed, including mapped/fallback/NPOT address oracles,
but settled scene changes were only -0.84% overview, -0.66% grazing and +0.71%
close. Reject it: all four shader/probe/test files are restored. Both attempted
chart caches are absent; quality, height bounds and material density are unchanged.

The rebuilt native editor now has raster `render.pom.horizon_debug 9`, read in
Raw albedo: green ordinary hits, yellow connected hits, other statuses unchanged.
Native fixtures prove both routes preserve displaced depth. Twelve settled
StreamMountain captures pass their source/binary/command/Vulkan audit. Connected
hits are only 0.564%, 0.155% and 0.076% of successful terrain hits in overview,
grazing and close regions. Widespread connected traversal is not the explanation;
its narrow bands and failed attempts may still be expensive. These are coverage
counts, not GPU cost attribution. Boundary fallbacks remain an open seam issue.
Ordinary albedo/normal output matches the baseline within one byte level.

**Resume visual material work next:** improve varied embedded stone forms and
height-aware soil/moss coverage, then contact layers. Preserve POM-on/off and
native relief checks, while continuing lightweight frame-time monitoring.
The larger overview regression, broad terrain/LOD continuity, moving-camera/RT
review and original acceptance gates remain open. Two ineffective cache trials
are sufficient evidence to stop speculative chart-lookup tuning for now.

[Rejected page reuse](../../agent/evidence/2026-09-17-pom-page-reuse/README.md) ·
[Route diagnostic, native gates and scene evidence](../../agent/evidence/2026-09-17-pom-path-diagnostic/README.md).


## StreamMountain height-aware terrain layers — 2026-09-17

The mountain JS recipe now uses the existing `s.layer` contract for soil,
embedded stone and moss. Height-aware weights compose RGB, squared roughness
and metre height together; normals derive from that composed height. Broader
stone sizes and uneven body shapes replace the more uniform earlier forms.
A first visual pass exposed pale stone contrast and a moss-mask error; the
retained second candidate corrects both and adds shallow correlated moss detail.
Receiver slope/snow-facing masks remain appearance-only under source v1's
height derivative restrictions; full context-dependent layers are not complete.

Native MSVC material checks pass on the actual authored recipe: 372 GPU ops,
two cellular searches/three reuses, 4.90 cm sampled relief, bounded channels,
stable world anchoring, and no terrain atlas/settling jobs. Paired near/far mean
height differs by 0.595 mm and red albedo by about 0.69% in the measured patch.
The independent unclamped height bound fits the original [-0.090, 0] m range.
Terrain geometry, 64 texels/m density, POM quality and the editor binary are
unchanged. Only the project JS material differs from the preceding validated
production source manifest.

Nineteen settled native captures cover overview, ground close/grazing and both
cliff views, with raw channels and four POM-off controls. The audit passes with
unchanged sources/binary, no Vulkan/command errors, exact props restoration and
the capture editor closed. Retain this as a material-development checkpoint,
not final realism: small stones remain busy, broad moss is simplified, soil
needs detail, cliffs are plate-like, and chart/LOD seams remain visible.

**Performance remains unresolved.** Overview/grazing/close G-buffer medians are
33.441 / 18.293 / 6.217 ms. Frontal cliff is 30.0295 ms versus 23.1725 ms in the
first candidate and 25.957 ms in the earlier bedrock capture: preserve this
non-isolated regression signal despite identical settled page counts between
candidates. Root bake is 54.573 s, dominated by 41.569 s publish. Further visual
work and contact layers remain the priority; do not claim faster total startup
or completed performance acceptance. Motion/mip and actual-scene RT review,
sparse overlays and the wider wall/terrain goals all stay open.

[Layered terrain evidence, source snapshots and POM comparisons](../../agent/evidence/2026-09-17-mountain-layering/README.md).


## Receiver categories and contact-binding gap — 2026-09-17

**Progress, not completed contact blending.** Generic `s.receiverMaterial`
exposes the original categorical triangle material before a source carrier.
CPU source/appearance, GPU evaluation and canonical shared-page keys agree.
Legacy/habitat and receiver-dependent reusable face recipes reject unsupported
use. Native surface, face material, residency, full compositor, POM, seam,
periodic mapping and residency checks pass without Vulkan validation errors.
The test recipe fits 364 GPU ops, retains 2.5 cm sampled substrate relief, and
adds up to 6.5 mm of dirt/moss. Protected and outside-volume CPU probes retain
all channels exactly. StreamMountain's retained material still passes.

The contact fixture is grouped under `texturing/terrain/SurfaceContactProof`.
Native captures rejected two assumptions: streaming worlds skip ordinary root
placement, and their world surface recipe is bound only to sector variants.
The fixture now uses `WorldSector.requires/placeChild`, with four visible
independent receivers and ground off the y=0 cube boundary. Ground POM is active,
but props still use original materials. The final 24-image capture passes
transport/source/validation checks and FAILS cross-object appearance acceptance.
Do not call this end-to-end contact blending or final art.

**Next concrete implementation:** add explicit selection of placed receivers for
world surface composition. Resolve actual expanded child world frames and
instance counts, not just sector manifest entries. Coordinate demand and eager
registration, source edits, moving receivers, sharing/fallback and invalidation.
Unselected foliage/assets must retain their material. Do not silently substitute
zero world coordinates for shared instances. Then repeat this proof's raw
channels, POM-on/off, protected back/shelf, depth bounds and native RT checks.
Geometry-baked finite/periodic bases still need their world overlay path.

The new receiver input alone does not resolve sparse splat records, spatial
filtering, local invalidation, per-instance overrides or the outstanding
StreamMountain performance/visual/seam acceptance. Those remain active.

[Native checks, rejected captures and precise next binding work](../../agent/evidence/2026-09-17-surface-contact/README.md).


## Explicit world receiver binding — 2026-09-17

`streaming.surfaceReceivers` now selects installed ordinary modules for the
world source. Expanded parent/child transforms and all resident physical
placements determine whether a variant has a unique rigid world frame.
Unselected/unsupported assets keep asset shading; finite-source modules fail
explicitly rather than lose their geometry-baked base. Frame changes copy new
source inputs and invalidate affected owners using the existing retained-page
contract. Per-instance overrides and precise splat-bounds invalidation remain
open. Terrain demand/live classification also now includes volumetric sector Y.

Native contact, CPU residency, mountain, queue, input-snapshot, POM, seam and
module tests pass. The default v4 contact capture and eager v6 repeat demonstrate
deposition on ground, rock and foundation, exclusion on protected/depth controls,
and working composed relief in POM-on/off and native RT views. Raw normals and
all but one albedo pixel agree within one byte across loading modes. Eager v5
exposed a startup crash from snapshotting null fallback images; the rebuilt
renderer initializes those images first and v6 completes with zero validation
errors. Capture audits now detect both engine validation-error log formats.

This is a limited functional milestone. Visible facets/chart artifacts, lighting
mismatch, final realism, whole-scene performance and motion/mip acceptance remain.
StreamMountain v3 completes nineteen audited captures with the corrected Y frame;
close/grazing/cliff POM remains active. Whole-scene startup remains about 55 s
root setup plus 168 s initial streaming. Next work should
retain these bounds/receiver controls while addressing terrain seams and material
art; shared finite-base overlays still need their dedicated sparse adapter.

[Binding, limitations and reproducible evidence](../../agent/evidence/2026-09-17-world-receiver-binding/README.md).


## StreamMountain soil burial and finer-scale filtering — 2026-09-17

Retained a JS-only material iteration after the world-binding checkpoint.
Cell distance varies worn stone crowns; regional weather varies presence;
reduced pale pigment/outline speckling quiets the ground. Shallow nonnegative
soil thickness buries low stones through the existing height-aware layer blend,
and clod-correlated moss height/coverage gives the ground more structure.
Centimetre chips/clods now fade before the larger stone bodies. The estimated
near-field mean is subtracted without reducing relief amplitude.

Native material checks pass: 404 packed GPU ops, two cellular searches/three
reuse reads, 4.60 cm sampled relief; paired near/far mean height differs by
0.599 mm and red albedo by about 0.17%. Independent raw bounds fit the original
[-0.090,0] m height envelope. The editor binary, engine/shader code, 64 texels/m
density and POM quality remain unchanged. There are no terrain atlas/settling
jobs. Candidate v1's 2.1 mm mean shift was corrected before visual capture.

Nineteen fixed-camera images pass the immutable-source/binary, command, Vulkan
and props-restoration audit. Retain the quieter stone/soil result as incremental
progress; it is not final realism or visual approval. Soil/moss still look soft,
cliffs remain plate-like and chart/LOD seams remain. Before adding more fine
noise, diagnose near-page resolution/filtering at the current physical density.
Then address cliff fracture structure, movement/LOD review and actual-scene RT.

The measured close/grazing G-buffer times increase about 10.3% / 7.0%, while
cliffs are slightly faster. These non-isolated observations remain a regression
signal, not a causal attribution. Root setup is 55.623 s (42.100 s publish),
initial streaming 168.23 s. Startup/performance acceptance and the full terrain,
wall, sparse overlay and local-invalidation requirements stay open.

[Recipe snapshots, bounds, images and measurements](../../agent/evidence/2026-09-17-mountain-organic/README.md).


### 2026-09-17: measured near-terrain VT resolution ceiling

Progress: added native-checked raster diagnostics for proxy requested/resident
mips and effective world-space texel density. Both full StreamMountain runs
(29 captures each) pass source/binary identity and Vulkan validation checks.
The settled close ground/cliff views already read mip 0 at about 63/59 t/m.
Requesting 128 t/m instead of 64 does not sharpen these surfaces: the chart
packer halves density to fit its 8192 atlas limit, while indirection high-water
storage rises 11.36 → 15.54 MiB. The experiment was rejected; the scene is
restored exactly to 64 t/m. Raw material channels remain consistent with the
previous build. Lit-session reproducibility and non-isolated timing regressions
are retained as limitations rather than passed acceptance.

Evidence and exact interpretation:
[`2026-09-17-vt-resolution/README.md`](../../agent/evidence/2026-09-17-vt-resolution/README.md).
Next investigate virtual address-space capacity/partitioning and its mip,
feedback, tail, allocation and seam contracts before adding more unresolved
fine material noise. All original wall, layered-splat, contact, visual, motion,
RT and performance requirements remain open; POM remains the displacement path.

### 2026-09-17: near-terrain resolution ceiling removed

Retained 16K virtual atlas support, including the ninth mip, bounded table
allocation, CPU/GPU record agreement, feedback coordinates and export dimension
checks. StreamMountain now actually retains its requested 128 t/m: the native
64 m staging fixture produces an 8320-square atlas, and scene diagnostics show
about 125 resident t/m on close ground and 121 on cliffs, roughly double the
previous density. POM settings and the material recipe are unchanged.

Six MSVC builds and fourteen focused checks pass. Two immutable full-scene runs
retain 58 images with zero Vulkan validation errors. Peak used pages rise
1386 to 1437 (3.7%); lookup-table use rises 11.36 to 36.96 MiB inside its existing
64 MiB allocation. The physical pool stays unchanged, while metadata upload
staging adds 6 MiB and maximum CPU/GPU records add 128 KiB each.

This improves resolvable detail, not final material acceptance. Warm root setup
is 53.883 s and initial streaming 168.84 s; these intervals overlap. First-use
setup is 126.417 s, with a large first bark-job cost still needing attribution.
Frame samples remain non-isolated. Material art, lighting, terrain chart/LOD
seams, motion/actual-scene RT approval, sparse per-instance layers and original
performance targets remain open. The broader terrain/building goal stays active.

[Implementation, completed repeat and source audit](../../agent/evidence/2026-09-17-vt-atlas-capacity/README.md).

### 2026-09-17: mineral face detail and cliff-height selection gap

Retained a modest JS material refinement: weather varies fracture width,
creased mineral detail varies stone crowns/bedrock relief, and soil has stronger
shallow clods. The actual source packs 428 GPU ops, retains the two cellular
searches/three reuse reads and needs no terrain source atlas or settling.
Sampled relief is 4.59 cm; an independent bound remains inside [-0.090,0] m.
The editor, terrain density, POM settings and geometry are unchanged. Native
material validation and all 33 full-scene captures pass with immutable inputs,
restored props, zero Vulkan/command errors and editor exit 0.

Clear-weather POM controls make the surface easier to inspect and expose a
structural limitation: cliffs select bedrock color but retain some loose-ground
height. Source v1 rejects receiver/field-dependent heights because its normal
evaluator holds those inputs fixed at offset positions. Keep that guard until
a consistent derivative/context sampling contract exists. The next substantive
material step is coherent context-driven height layering, including cross-chart
and LOD behavior, followed by real bedrock/soil exposure. Adding more noise to
the current shared height does not solve the material-selection issue.

Cliff plates, distance bands, lighting/art polish, sparse instance layers,
motion/actual-scene RT and original performance acceptance remain open.
Warm setup remains 54.401 s and overlapping initial streaming 169.59 s; no
whole-scene speedup is claimed. The full terrain/building goal stays active.

[Source, bounds, captures and measurements](../../agent/evidence/2026-09-17-mountain-facets/README.md).

### 2026-09-17: coherent receiver-dependent terrain height

Added explicit `heightContext: 'receiver'` source v2, retaining v1's position-only
height guard. Normal generation resamples interpolated receiver normals/field
lanes at each height offset and follows connected triangle neighbors. The
bounded walk preserves the existing physical height-page format; periodic and
finite bases remain v1-only. Mesh-interpolated context is not an exact field
query or a guarantee of continuity across terrain LODs.

StreamMountain now blends bedrock height together with color and roughness on
cliffs. Loose-ground pebbles no longer remain under rock pigment. Its source
packs 458 operations, retains two cellular searches/three reuse reads, and
samples 4.57 cm of relief inside the unchanged [-0.090,0] m envelope. Density,
geometry and POM quality are unchanged; terrain uses no atlas/settling job.

Six MSVC builds and twelve focused checks pass, including analytic context
height/normal probes across an internal triangle edge and source v2 raster/RT
publication. Full-scene v6 retains 33 valid raster images but its four RT-labelled
images are rejected: an invalid render-path command left raster selected. The
corrected v7 repeat has 20 verified captures, four in actual native RT, zero
command/Vulkan errors and immutable inputs. Both paths show working POM. Final
source/binary audit passes for all 1,013 manifest sources and six executables.

This is a material-selection milestone. Plate-like cliff shapes, visible bands,
raster/RT lighting differences, terrain motion/LOD and final art remain open.
The full route retains 1,437 used pages and 36.96 MiB indirection storage, with
54.391 s root setup and overlapping 173.17 s initial fill. Cliff/oblique raster
samples are about 7.5% slower than the previous source; shorter repeat timings
have different workloads and do not establish performance acceptance.

Next isolate the bands with POM-off raw channels, chart identity and wireframe
controls. Sparse layers, shared finite-base overlays, contact/wall completion,
original performance targets and user visual approval remain required.

[Implementation, rejected RT labels, corrected capture and limits](../../agent/evidence/2026-09-17-receiver-height/README.md).

### 2026-09-17: cliff recipe shapes separated from sector-boundary POM failures

Unchanged-source v8 adds 28 audited raster controls. Broad plate/bevel shapes
persist in POM-off normals within charts; those need material-recipe work.
Separately, 5,000 of 5,001 cliff POM boundary failures lie within three pixels
of chart-color boundaries meeting at the 64 m sector junction. Internal mesh
edges show successful connected hits. The current walker retains one variant
slot/geometry snapshot and cannot traverse to another sector. Cross-sector
ownership is supported by the camera coordinates and implementation; the
color diagnostic alone does not identify exact slots.

The independent-owner/reference-mesh fixture is now implemented as the native
`vt-sector-seam` gate: the connected reference passes, split owners fail in
raster and RT with all fine pages resident. See
[the native sector POM evidence](../../agent/evidence/2026-09-17-sector-pom/README.md).
Next implement bounded terrain
cross-sector POM sampling (neighbor transitions or an explicit overlap domain),
including world frames, immutable neighbor inputs, feedback/residency, edits,
eviction and unequal LODs. Preserve actual relief at the border. Validate all
channels, raster/RT and camera motion before claiming seam acceptance.
The ground control also has unresolved pixels confined to its far upper band,
which needs separate resolution/fade review. Final rock art and the wider
terrain/building, sparse-layer and performance requirements remain open.

### 2026-09-17: expanded environment goal

The user expanded Streaming Mountains to include improved rocks, boulder fields,
vegetation, roads, simple houses, traversable tunnels and power lines. The
[environment implementation plan](2026-09-17-streammountain-environment.md)
tracks those additions and carries forward unresolved texturing and performance
requirements. The first asset step is twelve analytic rock prototypes and
stable terrain-conditioned boulder/scree clusters. Their dedicated VT/POM
material detail and contact overlays remain required; basic rock geometry is
not completion of the visual goal. Cross-sector POM remains an open engine task.
