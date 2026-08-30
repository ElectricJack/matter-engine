# MatterEngine roadmap

**Updated:** 2026-08-30

This is the current product priority list. Completed implementation records
live under `docs/completed/`; superseded directions live under
`docs/deprecated/`. The
[implementation-gap audit](docs/findings/spec-implementation-gap-audit-2026-08-28.md)
contains the supporting inventory.

## Now — finish raster-water quality and acceptance

Authored per-part/per-instance `rayTraced` eligibility and raster-only animated
water are [accepted](docs/findings/render-eligibility-acceptance-2026-08-29.md).
The forward optics are implemented, but overall raster-water quality is
[not accepted](docs/findings/raster-water-forward-optics-acceptance-2026-08-29.md).

1. Resolve the waterfall's faceted curtain and localized plunge-pool whitewater
   with a newly approved approach; preserve accepted section continuity.
2. Prove downstream foam advection in the turbulent lanes and convincing
   reflections/screen-edge fallback in every retained river view. Preserve
   shallow-bed visibility, depth fog/refraction, and the single shadow set.
3. Complete Task 12's centralized, overflow-safe memory enforcement: at most
   1 GiB per complete animation file (including its header), and 700 MiB for
   the aggregate section/handoff files before publication. Report sidecar and
   peak-build residency separately. This remains open independently of the
   rejected Task 11 refinement.
4. Finish the clean cold-bake/cache-hit and native screenshot acceptance,
   including matched frame/memory captures at 1, 10, and 16 shadow samples
   (10/16 remain unmeasured for the final optics candidate). Reconfirm zero
   animated-water RT decode, BLAS, TLAS, and RT records.

Authority:
[render eligibility and documentation lifecycle](docs/superpowers/specs/2026-08-28-render-eligibility-and-document-lifecycle-design.md).

## Next — playable river proof

Stage 1 animated-section continuity is accepted. Task 11 waterfall refinement
was rejected and its experimental implementation removed; waterfall/plunge
appearance and overall raster-water acceptance remain open. See the
[bounded rejection evidence](docs/findings/waterfall-refinement-rejection-2026-08-30.md).
Controller integration proceeds against the accepted coarse river.

1. Integrate the completed character-controller work from `main`/
   `feature/character-controller` into the fluid branch and validate its
   fixed-step movement and terrain collision in the river world.
2. Build the first focused rapids playtest around the controller-driven player
   and controllable floating craft.
3. Close remaining longer-river and waterfall/plunge visual quality with a
   newly approved bounded approach, preserving accepted spillway continuity,
   pools, and the shared animation clock.
4. Run a clean MSVC/PhysX cold bake and cache-hit acceptance with character
   movement, terrain collision, buoyancy, animated playback, the standard
   river cameras, and retained screenshots/timings.

The fluid simulation, collision mesh, and gameplay flow fields remain baked.
Runtime water interaction is not required.

## Scale before expansion

1. Implement the stable-slot, O(changed) RT TLAS CPU mirror before scaling the
   ray-traced world further.
2. Finish removing the remaining app-lane Vulkan registration tail from bake
   publication.
3. Close the LOD/VT proxy-world, visibility, unified-budget, and final measured
   acceptance endpoint.
4. Re-profile dynamic command-layout work with raster-only water and add the
   proposed memoization if it remains material.

## Deferred decisions

These are preserved ideas, not scheduled commitments:

- whether animated models should default to raster-only; individual models
  can opt out of RT first;
- authoring/editor additions: native Windows live-edit watching, lattice DSL,
  true round extrusion joins, and the interactive Settle Lab;
- animation additions: gameplay bindings, general constrained IK, and an
  explicit deforming-mesh RT policy; and
- material/rendering additions: part-local AO, ground macro variation,
  RT ice/snow, decals, and emissive-mesh lighting of volumetric fog.

Any deferred item needs a current design and priority decision before work
begins. The gap audit retains its original evidence.

## Retired directions

Do not schedule work from the deprecated archive without a new decision. The
retired directions include bespoke LBM river solvers, legacy OpenGL and
Explorer viewers, CUDA/OptiX renderer paths, old impostor generations,
animated ray-traced water, and runtime fluid interaction.
