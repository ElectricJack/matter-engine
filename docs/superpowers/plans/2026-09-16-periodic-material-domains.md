# Periodic material domains and sparse wall overrides

Status: explicit JS module/layout, native independent sampler, periodic page
producer, module residency, paired demand transport and JS/renderer binding
implemented. Planar mapping and real brick export are verified; live 1×/2×/4×
visual acceptance remains open. Coverage-only generation with separate receiver
AO passes native validation and has six live wall captures. This is the next part
of the existing whole-wall and layered texturing goals, not a replacement for
their terrain or acceptance requirements.

Priority update (2026-09-16): the user requested a StreamMountain material and
generation-speed pass. Wall work is paused at the validated
[material read lease checkpoint](../../agent/evidence/2026-09-16-material-read-leases/README.md).
The [layered-surface plan](2026-09-14-layered-surface-texturing.md#current-priority--streammountain-2026-09-16)
tracks the terrain work; this plan's remaining composition and acceptance work
is still required.

## Why the current sharing path is insufficient

Fixed physical grids and exact producer identities now remove duplicate pages
from differently sized walls. That only works when stored texel coordinates and
all material inputs match. The 2.04×0.376-m brick module is not an integer number
of 128-texel pages at 512 texels/m. A repeated point can therefore lie in a
different page phase. Hash normalization or a per-page material-slot alias
cannot correctly represent a receiver page spanning several module pages.
Whole-program weathering also still makes otherwise equal interiors private.

## Three separate records

1. **Material module:** immutable source recipes, base program, physical repeat
   domain and canonical texel/mip metrics. Identity excludes wall size, instance
   transform, placement phase and instance weathering. Appearance and geometry
   retain separate source dependencies. The new JS `layoutKey` describes only
   layout; it cannot substitute for this complete native identity.
2. **Receiver mapping:** receiver geometry/chart lifetime, coverage, and metric
   mapping into a module. Keep normal-frame conversion and displacement datum
   explicit. Corners/caps select the correct projected source treatment.
3. **Instance layers:** stable ordered layer/splat records with finite bounds,
   channel operation, projection constraints, dependencies and filter support.
   Allocate overrides only where these records affect the surface. Removing
   the last override restores the module reference after readers retire.

## Implementation sequence and evidence

### 1. Author a real repeat domain

Implemented in `brick_wall_layout.js`: counts include their terminal joints,
running-bond course parity is preserved, negative appearance phase is defined,
and headers remain explicit open-end treatments. The same placements serve
geometry and texture composition. `PeriodicBrickWallProof` retains three
twelve-triangle boxes at 1×/2×/4× counts. The JS surface recipe now authors two
independent periodic modules (front/back), with size-independent content and
bounded mappings that leave physical end treatments finite.

### 2. Give the module its own virtual address space

Sampler checkpoint: `vt_material_domain.glsl` now retains receiver coverage and
geometry while resolving a separate module address, height decode and feedback.
It wraps coordinates without wrapping derivatives, validates publication tokens,
and corrects fallback lookup across non-power-of-two mip/page boundaries. The
[native evidence](../../agent/evidence/2026-09-16-periodic-material-sampling/README.md)
records 741 GPU probes and 12 reproduced failures in the old single-lookup
control. This is the sampling primitive; the runtime publication/lifetime/demand work
below remains required before the wall scenes can use shared module pages.

Producer checkpoint: the generic native factory now creates complete immutable
module inputs, repeats source references across the boundary and fills compressed
pages through the existing compositor. The
[producer evidence](../../agent/evidence/2026-09-16-periodic-material-producer/README.md)
records exact repeated-channel equality for an analytic fixture and real
geometry-baked bricks. Owned preparation is reused, and preparation geometry is
not published as receiver geometry. These native fixtures do not yet register
module residency or bind wall draws.

Runtime ownership checkpoint: `acquire_material_module` now shares a registration
through retained render-thread leases. Binding queries gate publication on the
submitted tail and validate the allocation generation and runtime epoch. Final
lease release uses the existing reader retirement horizon. The
[native residency fixture](../../agent/evidence/2026-09-16-periodic-material-residency/README.md)
samples the actual compressed pool and routes its GPU-derived module request
through the production queue. This supplies ownership and activation for the next
step; wall mappings, dual receiver/module feedback and connected POM integration
still need to retain and publish these leases.

Demand checkpoint: the G-buffer attachment now has room for both receiver and
module requests from one visible fragment. Production extraction/readback queues
both, preserving primary raster/RT input tags and the fixed 8x8 sample grid.
The [paired feedback evidence](../../agent/evidence/2026-09-16-periodic-material-feedback/README.md)
records the explicit attachment/readback memory cost and native validation status.
Actual wall draws still need their geometric module mapping before they can
populate the second request; this checkpoint does not establish wall/POM reuse.

Receiver mapping checkpoint (verified in thirteen native checks): `bind_receiver_materials`
stages an immutable per-chart table and retains module leases through displayed
pages and their retired readers. It validates planar physical coordinates and
maps normal frame, phase, height datum and independent material LOD. Raster/RT
share the sampling path; the production POM marcher uses the module's physical
resolution. The [mapping evidence](../../agent/evidence/2026-09-16-receiver-material-mapping/README.md)
tracks the checks. Generic JS/renderer registration is now implemented through
`VkSceneRenderer::bind_vt_materials`; the real brick export also exercises the
authored module/frame data. Its chart-origin fix and independent exported preview
are documented in [export evidence](../../agent/evidence/2026-09-16-asset-export/README.md).
Mapped connected boundaries and live wall acceptance remain open.

Coverage-only implementation: a conservative whole-page proof, including stored
gutters and finite mapping bounds, allows planar mapped interiors to keep only
receiver AUX/geometry. Their fill skips base material evaluation, compression,
material copies and private material-slot allocation. Mandatory tails and uncertain
boundaries retain complete finite materials. Before removing/narrowing a mapping,
regenerate incompatible pages as full finite pages while retaining the old table;
publish the replacement only when those pages are ready. Failed replacement work
must keep the previous visible material. [Native coverage evidence](../../agent/evidence/2026-09-16-receiver-coverage/README.md)
records CPU/GPU checks; this is not yet an accepted memory or performance result.
The subsequent [receiver AO checkpoint](../../agent/evidence/2026-09-16-receiver-occlusion/README.md)
stores immutable R16 geometric occlusion factors separately from shared material
pixels. Eleven native checks pass, including coverage-only interiors under the
normal two-page/frame AO budget and actual differing traced occlusion over one
shared base. Factors publish only for current successful writes and retire with
GPU readers. Live/retired factor pages and actual slab allocation bytes are
reported separately. Six 1×/2×/4× wall captures show the normal AO budget active
alongside 73 coverage-only pages; raster and RT overview/close/grazing images are
retained in that checkpoint. User visual approval, motion and performance
acceptance are still required.
Reserved pool capacity
is unchanged: lower live material-slot use must not be reported as lower allocated
VRAM. Appearance overrides and curves still require the later steps below.

- Register the module independently of any drawn receiver, with its own
  deterministic content identity, page requests, resident tail and lifetime.
- Choose canonical texel dimensions from physical extent and requested density;
  round upward rather than silently lowering quality. Record physical texel
  metrics on both axes. Do not alter brick dimensions to fit a power-of-two page.
- For BC-compressed module pages, preserve the four-texel block phase across
  every repeat, including each mip through the resident tail. Each mip dimension
  must be a multiple of four, or one/two texels which divide a block. Round
  density upward to satisfy this chain; retain non-power-of-two dimensions where
  possible. Independent compression of different block phases can otherwise
  produce visible color/normal differences even when raw height matches exactly.
  Keep this producer constraint separate from the sampler's ability to address
  arbitrary odd-sized virtual textures. Report both logical texels and allocated
  pages so this rounding cost is explicit.
- Receiver sampling first resolves its coverage/geometry, then resolves material
  pixels in the module address space at the mapped physical point. A single
  receiver page may sample multiple material pages; do not force a single-slot
  alias to stand in for that lookup.
- Wrap sample coordinates and filter neighbours, keeping derivatives unwrapped.
  Use each mip's actual physical metric, including odd/non-power-of-two logical
  dimensions and the tail. Preserve coherent color, normal, squared roughness,
  height and coverage filtering across the period boundary.
- Extend demand feedback to the module addresses actually sampled, including
  POM travel. Keep private receiver coverage and module residency measurable
  separately. Existing equal-payload sharing remains useful where applicable.
- Resolve base lookup before composition. A base hit must avoid repeating its
  source shading/encoding; private receiver coverage work may still be needed.

Evidence: negative and exact-boundary samples, both sides of every repeat,
unequal receiver/module page phases, broad footprints and mip tails, plus native
1×/2×/4× captures showing page identity/reuse and correct POM. Compare all four
material channels against an untiled reference. Count actual avoided work.

### 3. Keep finite ends, corners and curves physically correct

- Open ends/final courses use bounded structural treatments over the periodic
  interior, with source-derived side/top projections and whole bricks.
- Orthogonal turns use one shared brick junction layout. Receiver mappings must
  agree at the physical corner; two unrelated front images are insufficient.
- Curves retain rigid brick placement and the inner/outer joint solve. Reuse a
  canonical curved module only where geometry/projection identity warrants it;
  do not stretch the straight module over both radii. Preserve correct finite
  composition while introducing and validating that mapping.
- Keep receiver topology independent of texture resolution and brick count.
  POM continues to use each receiver's geometry/coverage through chart crossings.

Evidence: both corner handednesses, odd/even course counts, caps/ends, two legal
curve radii and impossible-radius rejection; matched POM on/off and raster/RT.

### 4. Add sparse layer overrides

Dependency ownership checkpoint: exact-mip cached material reads now retain
explicit physical pixel addresses, height decodes and source/input revisions.
The material allocator keeps these pixels through receiver eviction and uses
copy-on-write while readers exist. Wrapped footprints queue missing dependencies;
stale reads cannot be admitted or published. The
[native read-lease evidence](../../agent/evidence/2026-09-16-material-read-leases/README.md)
records seven passing checks, including actual color/height retention under
eviction pressure. This completes dependency ownership only. The shader-readable
composition phase, ordered layer records, sparse publication/local invalidation
and JS/renderer integration below remain to be implemented.

- Separate weathering programs/records from the base module recipe. Use finite
  authored/procedural layer bounds expanded by filtering, displacement and warp
  support to select affected pages.
- Compose each affected page against the shared base with the existing coherent
  channel conventions. Include finite projection depth/facing to prevent leakage.
- Track exact dependencies and local dirty bounds. A moved/edited/deleted layer
  invalidates its old/new expanded regions, preserving untouched base pages.
- Keep large-scale grime continuous across bricks/charts and generalize the
  same records to terrain/rock/building contact splats.

Evidence: clean and independently painted instances sharing one module; layer
move/delete/reorder; changed source; deletion of one owner; eviction and pending
edits; unchanged source geometry/preparation on appearance-only edits.

#### Override production and publication

The current compositor writes scratch pages while the pool is in transfer-dst
layout, then residency copies successful candidates into resident slots. An
override producer cannot simply sample the module pool through that layout.
Implement an explicit read/composition phase with shader-readable module pages
and ordered publication copies (or a validated GENERAL-layout read/write phase).
Keep every existing reader/copy barrier; do not sample images described as
transfer-only destinations. No CPU texture readback belongs in this path.

At admission, collect the module pages covering the receiver page's full filter
footprint, including wrapped edges and height/normal support. Retain their
content revision and sampled resolution, and protect those physical allocations
from reuse until the recorded read finishes. A module lease alone only retains
the virtual owner; it does not pin each fine page against eviction. A fill that
uses GPU indirection must consume the table state actually visible at that point
in the command stream, not CPU mappings scheduled for upload later in the frame.
Defer unavailable dependencies while keeping the previous complete binding.

Each override result must identify both the module content and ordered layer
revision it consumed. Recheck those dependencies before publication, exactly as
the existing scratch-page path rejects stale receiver fills. Appearance edits
must reuse prepared source geometry. Publish private overridden material pixels
only for affected pages; removing the final layer returns an eligible page to
coverage-only ownership. A temporary coarse-base composition, if introduced,
must record its sampled mip and trigger bounded local refresh when that base
improves; it cannot silently masquerade as the final requested resolution.

## Publication and acceptance invariants

Receiver geometry, module content and override versions must form a compatible
published binding. A missing/failed replacement keeps the prior complete binding;
never combine a new override with an unrelated base generation. Retain all
referenced geometry/input/module data through the GPU reader horizon. Validate
bounded queues, allocation failure, cancellation and source replacement.

Report unique module pixels, private coverage, sparse overrides, preparation
bytes and configured pool reservation separately. Static screenshots and lower
occupied page counts do not prove steady frame time or edit latency. Complete
moving-camera, visual-approval and original VT gates after the visual workflow,
alongside the broader terrain/contact-blending requirements.
