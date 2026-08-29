# MatterEngine roadmap

**Updated:** 2026-08-29

This is the current product priority list. Completed implementation records
live under `docs/completed/`; superseded directions live under
`docs/deprecated/`. The
[implementation-gap audit](docs/findings/spec-implementation-gap-audit-2026-08-28.md)
contains the supporting inventory.

## Now — raster water and render eligibility

1. Add the authored `rayTraced` boolean to part definitions and part
   instances. Instance settings override part defaults; the engine default is
   `true`.
2. Pin animated water to raster-only rendering. It must not create or cache
   animated-water BLAS geometry or enter the TLAS.
3. Bring raster water up to the quality of the surrounding ray-traced scene:
   screen-space refraction, shallow-visible depth fog/absorption,
   turbulence-driven foam, flow-aligned animation, convincing reflections,
   and correct non-duplicated shadows.
4. Prove the result with matched river screenshots and frame/memory captures,
   including evidence that animated-water BLAS work is absent.

Authority:
[render eligibility and documentation lifecycle](docs/superpowers/specs/2026-08-28-render-eligibility-and-document-lifecycle-design.md).

## Next — playable river proof

1. Extend the sequential bake into reliable longer river sections while
   preserving continuous spillway handoffs, waterfalls, pools, and the shared
   animation clock.
2. Integrate the completed character-controller work from `main`/
   `feature/character-controller` into the fluid branch and validate its
   fixed-step movement and terrain collision in the river world.
3. Build the first focused rapids playtest around the controller-driven player
   and controllable floating craft.
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
4. Re-profile dynamic command-layout work and add the proposed memoization if
   it remains material after raster-only water lands.

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
