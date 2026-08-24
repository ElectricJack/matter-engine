# Sequential river sections and waterfall — design

**Date:** 2026-08-24

**Status:** approved; implementation not started

**Depends on:** `2026-08-22-physx-fluid-bake-integration-design.md`
**Goal:** extend the accepted single-section PhysX hydrology bake into a
deterministic chain of independently cached static river sections, prove the
chain with two 100+ metre reaches, and include a 12 metre waterfall, two filled
pools, a broad natural spillway handoff, and DSL-authored boulders.

## 1. Product decision

MatterEngine will not solve a complete river network to global inflow/outflow
equilibrium. It will build the river incrementally:

1. Bake one bounded section against a temporary downstream dam.
2. Stop when a sensor proves that the section's terminal pool has reached its
   authored spillway level.
3. Freeze that section as an immutable static artifact.
4. Start the next section from the accepted pool's spillway using inherited
   authored discharge.
5. Assemble the independently baked sections with a small derived handoff
   patch.

The first playable milestone has this topology:

```text
authored inlet
    |
    v
100+ m upper rapids -> 12 m waterfall -> plunge/fill pool
                                              |
                                  broad natural spillway
                                              |
                                              v
                                 100+ m lower rapids
                                              |
                                              v
                                      second fill pool
```

The waterfall is wholly inside the first section. The first section boundary
is at the calmer spillway, not in the free fall or impact zone. The second pool
uses the same temporary-dam and fill-sensor lifecycle to prove that the process
can repeat.

Water remains static at runtime. The published result contains visual water,
CPU query/collision output, and a gameplay velocity/depth field. Players and
runtime terrain do not alter the fluid.

## 2. Scope and non-goals

### 2.1 In scope

- two sequential PhysX PBD section bakes;
- at least 100 metres of river between the initial inlet and first pool, and at
  least 100 metres between the first and second pools;
- one approximately 12 metre waterfall immediately before the first pool;
- one plunge pool, one 8–12 metre broad natural spillway, and a second fill
  pool;
- an imperative builder-style scene DSL that passes authored river curves to
  the river generator;
- waterfalls, pools, spillways, boulders, and other scene obstacles placed by
  the DSL;
- immutable per-section artifacts, a dependency manifest, and a derived
  spillway handoff patch;
- deterministic cache invalidation and failure recovery;
- diagnostic renders for both successful and failed bakes; and
- timing broken down by setup, simulation, meshing, stitching, and total wall
  time.

### 2.2 Explicitly out of scope

- a monolithic full-river equilibrium solve;
- a coupled solve spanning two accepted sections;
- dynamic runtime fluid or player-to-fluid feedback;
- automatic native meander or native boulder scattering;
- tributary fan-in execution, although the manifest must not preclude it;
- the final level lake and level-completion rule;
- water streaming or section eviction at runtime; and
- replacement of PhysX PBD or MatterEngine's CPU/GPU meshers.

## 3. Ownership boundary

### 3.1 The DSL owns world authoring

The JS scene/build layer owns all creative and procedural layout decisions:

- construction of the full three-dimensional river curves;
- control-point placement, curvature, meander, and elevation/base-slope
  changes;
- river widths and carve-profile parameters along those curves;
- section ranges measured in metres along a curve;
- waterfall, pool, spillway, inlet, and sensor markers;
- boulder, log, ledge, bridge-support, and other obstacle placement; and
- deterministic seeds for any JS helper used to create those authored values.

The DSL may use loops, noise, seeded helpers, or explicit controls to construct
a curve or place a collection of objects. The authoritative result passed to
the engine is still an explicit curve and explicit scene-object set. The river
generator may not invent additional bends, apply a hidden meander function, or
scatter boulders.

The existing native `reach.meander` generation and `boulders({density, ...})`
selection therefore move out of native river geometry. Existing scene content
must be migrated to DSL curve helpers and ordinary DSL-created scene objects
before those native paths are removed.

### 3.2 Native code owns canonical construction and baking

Native river/hydrology code:

- validates and arc-length-resamples the supplied curves;
- canonicalizes all values for hashing and native consumption;
- applies the requested rounded-V terrain carve and smoothing to the supplied
  centerline and profiles;
- resolves distance markers to stable local frames;
- builds one continuous terrain result for the authored network;
- extracts section-local terrain collision with a dry collar that is never a
  containing wall;
- collects explicitly tagged scene collision geometry intersecting each
  section;
- adds temporary bake-only dams and sensors;
- invokes the existing in-process PhysX adapter;
- builds visual, CPU-query, and gameplay products; and
- publishes section, handoff, and network artifacts.

PhysX continues to own the fluid mathematics. MatterEngine only marshals
inputs, applies stopping/safety policy, and converts accepted results.

## 4. Imperative DSL contract

The exact JS symbol names must follow the repository's builder conventions,
but the semantic contract is represented by this sketch:

```js
const mainCurve = riverCurve()
  .moveTo([0, 48, 0])
  .cubicTo([36, 42, 22], [70, 34, -18], [104, 28, 8])
  .cubicTo([126, 22, 18], [142, 9, 2], [154, 4, 0])
  .cubicTo([190, -1, -20], [230, -8, 24], [278, -15, 2])
  .build();

const network = riverNetwork({
  cellSize: 0.5,
  seed: this.worldSeed ^ 0x52495645,
});

const main = network.river("main")
  .curve(mainCurve)
  .channelProfile(mainWidthAndDepthProfile)
  .carveProfile({ shape: "roundedV" });

main.section("upper", { from: 0, to: 145 })
  .inlet({ id: "headwater", at: 0, flow: 600 })
  .waterfall({ lipAt: 105, landingAt: 117, expectedDrop: 12 })
  .pool({ from: 117, to: 145, fillLevel: 24 })
  .spillway({ at: 145, width: 10, overlap: 5 });

main.section("lower", { from: 145, to: 275 })
  .after("upper")
  .fromSpillway("upper")
  .pool({ from: 255, to: 275, fillLevel: 3 });

const midstreamRock = boulder("midstream-rock")
  .position(mainCurve.atDistance(62).offset(-3.5, 0))
  .scale([2.8, 3.6, 2.4])
  .rotation([0.1, 0.6, -0.2])
  .fluidCollider(true);

world.add(midstreamRock);
network.colliders(world.tagged("fluidCollider"));
network.bakeSequential();
```

This example is not permission for native path generation. `riverCurve()` is a
DSL-side builder and `.curve(...)` passes its completed curve to the river
generator. Width, slope, and feature markers use physical distance along the
curve rather than normalized `[0, 1]` parameters. Editing upstream controls may
change later arc-length positions, but dimensions such as a 100 metre reach or
12 metre drop retain physical meaning.

The curve is three-dimensional. Its elevations are authoritative. A DSL helper
may generate a curve from a base-slope profile, but the native river generator
does not rewrite its height to satisfy an implicit grade. Waterfall lip and
landing distances refer to elevations already present in that curve;
`expectedDrop` validates the authored result and does not insert a native
height discontinuity.

Scene obstacles are ordinary authored roots/parts with explicit transforms and
a fluid-collider tag. The same resolved transform and shape feed rendering and
PhysX collision. Obstacles are rendered once even if a section overlap causes
their collision geometry to be supplied to both adjacent bakes.

The DSL may schedule a Box3D-style drop/settle operation when an obstacle needs
the finished terrain surface. That authoring node runs after terrain
construction and before hydrology collision extraction, freezes the resolved
transform, and includes it in the strict key. Native physics may execute the
settle, but only because the DSL explicitly placed and requested it; the river
generator never decides that an obstacle should exist.

## 5. Terrain and section geometry

The complete set of DSL curves and terrain features is resolved before the
first fluid section runs. The river generator creates one continuous terrain
revision so adjacent sections cannot disagree about the channel or spillway.
Each bake then extracts only the collision triangles intersecting its section
bounds plus an authored dry margin.

The first test world remains a steep alpine ravine. Its authored curves should
preserve the previously accepted approximately 15 percent upper-ravine grade,
allow different base slopes between lake sections, and deliberately use
stronger curvature in slower reaches. The terrain must be wide and high enough
that ravine geometry, not the fluid-domain extent, visibly contains the water.
Sky remains visible from at least one side rather than placing the river at the
bottom of an implausibly deep trench.

The waterfall is authored into the curve/feature stream immediately before the
first pool. Terrain construction must produce a rock lip, approximately 12
metres of free fall, and a landing basin deep and broad enough for a visible
impact, spray, and recirculation region. It is not a visual-only displacement;
the same geometry is present in the PhysX collision mesh.

## 6. Section lifecycle

Each `RiverSectionDefinition` contains:

- stable section and river IDs;
- start and end distance along its supplied curve;
- one or more explicit or inherited inlets;
- section-local terrain/collider selection;
- a terminal pool, spillway, temporary dam, and fill sensor;
- dry-margin and safety settings;
- visual/query/gameplay ownership ranges; and
- references to upstream section dependencies.

The coordinator validates that all referenced sections and spillways exist and
that the dependency graph is acyclic. It then bakes required sections in a
stable topological order. GPU simulation remains serial so two sections never
compete for the particle budget or CUDA context.

For a section:

1. Resolve the section collision input from the shared terrain revision and
   authored fluid colliders.
2. Insert a bake-only temporary dam a short distance downstream of the visible
   spillway lip, inside the handoff collar.
3. Place the fill sensor across the pool immediately upstream of the authored
   spillway level.
4. Run the existing PhysX PBD bake and emission schedule.
5. Accept only after the sensor's spatial wetness rule remains satisfied for
   the configured stable-step count.
6. Preserve the accepted particles and derive that section's visual mesh, CPU
   query mesh, and gameplay field.
7. Publish the immutable section artifact.

The fill sensor remains spatial, not merely a total-particle-count test. A
narrow jet or waterfall splash crossing a cell may not falsely complete the
pool. The sensor samples a broad crest region and uses both wet fraction and a
stable-step window.

The second section does not numerically drain the frozen first pool. The two
simulations are independent content-building operations connected by explicit
handoff metadata. Their assembled static products represent a continuous river
at the accepted moment.

## 7. Spillway handoff

`RiverHandoffDefinition` and its accepted record contain:

- stable upstream section, downstream section, and spillway IDs;
- lip position, tangent, cross-stream vector, and up vector resolved from the
  supplied curve/terrain;
- authored spillway width and effective water depth;
- inherited discharge;
- derived initial velocity;
- upstream and downstream ownership distances; and
- the particle/mesh collar used to build a seam patch.

The first implementation transfers authored discharge, not an estimate from a
noisy particle flux. With one headwater inlet:

```text
Q_lower = Q_upper
```

The data model may later represent tributary fan-in as the sum of accepted
authored discharges, but fan-in execution is not part of this milestone.

The downstream inlet is a shallow rectangular ribbon hidden just beneath the
natural spillway surface. A disc emitter is not suitable for a broad overflow.
The PhysX adapter therefore needs a ribbon activation layout described by
width, depth, local frame, and particle spacing. It still activates ordinary
PhysX PBD particles; it introduces no new fluid solver.

Initial mean speed follows the authored cross-section:

```text
speed = discharge / (spillway_width * effective_water_depth)
```

The direction follows the local downstream curve/spillway tangent, including
its vertical component. Invalid or implausible zero-area handoffs fail
validation rather than being silently clamped into a different river. Any
intentional velocity override must be explicit DSL data and therefore part of
the strict key.

## 8. Removing the temporary dam from the final river

The temporary dam is collision input only. It is never installed into the
rendered world or runtime collision scene.

The dam sits downstream of the natural lip by a small handoff-collar distance.
The upstream visual/query ownership range ends before particles reach the dam
face. Particles pressed into that face may remain in the diagnostic snapshot,
but they are excluded from the accepted upstream visual product. This prevents
the vertical particle curtain seen in the current single-section bake from
shipping as water.

After both adjacent sections are accepted, a separate
`HydrologyHandoffArtifact` is generated:

1. Gather accepted particles from both section artifacts inside the spillway
   collar.
2. Transform them into the common world-space handoff frame.
3. Run the existing GPU visual isosurface mesher over their smooth union.
4. Clip the patch to deterministic upstream/downstream ownership planes.
5. Mask the corresponding ends of the two section visual meshes.

The handoff patch depends on both section artifact hashes. It does not mutate
or republish either section. The CPU query surfaces use a deterministic cut at
the ownership plane rather than duplicating collision faces. The gameplay
field blends from slow pool flow through accelerating spillway flow and then
uses the downstream section field.

If visual union proves unnecessary after measurement, the implementation may
use a cheaper clipped overlap only if the same no-gap, no-curtain acceptance
tests pass. It may not fall back to a visible artificial wall.

## 9. Artifact and cache model

An accepted `HydrologySectionArtifact` extends the existing single-section
payload with stable section identity, curve-distance ownership, terminal-pool
metadata, and accepted handoff output. It continues to contain:

- the accepted particle snapshot;
- the high-resolution GPU visual mesh;
- the coarse CPU query/collision mesh;
- the section-local gameplay velocity/depth field;
- solver/sensor diagnostics; and
- strict input hashes and backend identity.

A `HydrologyHandoffArtifact` contains the seam patch, ownership masks/cuts, the
resolved spillway record, and both source section hashes.

A `HydrologyNetworkArtifact` is a small manifest containing:

- network and terrain revision IDs;
- section IDs, dependencies, strict keys, and artifact references;
- handoff IDs, source hashes, and artifact references;
- deterministic topological order;
- aggregate bounds and runtime product references; and
- overall state: incomplete, failed, or ready.

Cache invalidation follows the dependency graph:

- changing only section two does not invalidate section one;
- changing a handoff's visual settings invalidates the handoff patch, not either
  accepted simulation;
- changing an upstream discharge, pool/spillway geometry, terrain revision, or
  another value consumed by section two invalidates section two and downstream
  dependents;
- changing a DSL curve or obstacle invalidates every section whose collision,
  marker frame, or ownership interval changes; and
- canonical DSL output, not JS object identity or iteration accident, enters
  strict keys.

The ready network manifest is published atomically only when all required
section and handoff artifacts are accepted. Previously accepted section
artifacts remain independently inspectable during a failed continuation.

## 10. Failure and safety policy

A non-finite particle or invalid native/GPU state remains an immediate hard
failure. One particle leaving the diagnostic bounds is not.

Escaped particles are recorded and safely retired from later simulation work.
A section fails only when escapes exceed both an authored absolute threshold
and ratio threshold, or when the persistent escape rate indicates broken
collision/domain geometry. The default acceptance policy must tolerate at least
32 isolated escapes and must be expressed in the strict key. Tests cover the
exact boundary. Diagnostics report activated, active, retired, escaped, and
non-finite counts separately.

Other hard failures include:

- invalid/cyclic section dependencies;
- invalid curve distances or overlapping section ownership with no handoff;
- missing terrain or authored collision input;
- impossible spillway cross-section or emitter placement;
- particle, neighbor, mesh, or GPU capacity exhaustion;
- no sensor progress within the bounded step budget; and
- artifact serialization or hash mismatch.

If a downstream section fails, accepted upstream sections are not discarded.
The network manifest remains incomplete/failed and cannot be consumed as a
ready gameplay river. The failed attempt retains counters, timing, sensor
history, particle diagnostics, and enough renderable state to capture the same
diagnostic camera set used for successful bakes.

## 11. Testing

### 11.1 DSL and canonicalization

- a DSL-created curve reaches native code without native meander changes;
- control points, sampled positions, width/slope profiles, and distance markers
  canonicalize deterministically;
- explicitly placed/tagged boulders reach both render and collision input with
  the same resolved transforms;
- unknown sections, duplicate IDs, missing spillways, and graph cycles fail
  with field-specific errors; and
- DSL object enumeration order cannot change the strict key.

### 11.2 Coordinator and cache

- a fake backend proves stable topological order and serial execution;
- section two never starts before section one is accepted;
- a failed section two leaves section one reusable and the network not ready;
- downstream-only edits reuse section one;
- upstream handoff edits invalidate all dependents; and
- the ready manifest publishes atomically after required patches exist.

### 11.3 Handoff and products

- transferred discharge equals the accepted authored upstream discharge;
- ribbon activation is deterministic and respects width, depth, orientation,
  spacing, fractional carry, and capacity;
- the temporary dam is absent from installed world/render geometry;
- upstream visual ownership excludes dam-contact particles;
- the handoff patch has no open boundary or visible vertical curtain at the
  spillway seam;
- CPU query ownership does not create overlapping collision faces; and
- gameplay-field samples transition continuously through the spillway.

### 11.4 Real PhysX acceptance

The deterministic test world must:

- bake two sections with at least 100 metres per lake-to-lake reach;
- produce the 12 metre waterfall, visible free fall, impact, and plunge-pool
  response in section one;
- fill and accept both terminal sensor regions;
- begin section two from the broad natural spillway without a visible emitter;
- remain within configured capacity and escape policy;
- produce finite particles, meshes, and gameplay samples; and
- assemble into one static playable river product.

## 12. Visual and timing evidence

Every milestone run records matched-camera screenshots for:

1. full two-section overview;
2. upper ravine and curved rapids;
3. waterfall approach;
4. waterfall side/profile view;
5. fall impact, spray, and plunge pool;
6. broad spillway and handoff seam;
7. lower-section rapids and DSL-authored boulders;
8. second filled pool; and
9. low player-height navigation views.

Failed bakes capture the applicable subset from retained diagnostic particles
and counters. A bake is not rejected merely because it did not publish a final
artifact before screenshots were taken.

Timing output separates:

- DSL/native definition and terrain setup;
- PhysX/CUDA initialization;
- PhysX simulation per section;
- GPU visual meshing per section;
- CPU query meshing per section;
- handoff stitching;
- serialization/publication; and
- total wall time.

Particle counts, activated/retired/escaped counts, accepted step, mesh vertex
and triangle counts, and peak relevant GPU memory accompany the timings. This
makes startup cost distinguishable from simulation and meshing cost.

## 13. Implementation sequence

The implementation plan derived from this design should use these vertical
slices:

1. Move curve generation and boulder placement ownership into the DSL while
   retaining the accepted single-section visual result.
2. Generalize `firstSection` into stable section definitions and a serial
   dependency coordinator, initially with one section.
3. Add immutable per-section artifacts and the network manifest.
4. Author and bake the waterfall, first pool, and spillway in section one.
5. Add ribbon spillway emission and bake section two into its second pool.
6. Add dam-contact masking, handoff patch generation, query cuts, and gameplay
   field blending.
7. Run the complete deterministic acceptance suite and capture the full visual
   and timing evidence set.

Each slice must keep the existing accepted one-section artifacts inspectable
and avoid combining unrelated migration work into the river change.

## 14. Acceptance gate

This design is complete when the written spec is approved. The implementation
is complete only when:

- the DSL, not native river code, supplies the authoritative curves and scene
  obstacle placement;
- two independently cached 100+ metre sections bake in stable dependency order;
- section one includes the accepted 12 metre waterfall and filled first pool;
- section two begins invisibly at the 8–12 metre natural spillway and fills a
  second pool;
- the temporary dam and dam-contact particle curtain are absent from the final
  visual/runtime products;
- the combined GPU visual surface, CPU query surface, and gameplay field have
  no unacceptable handoff seam;
- isolated escaped particles do not abort an otherwise valid bake;
- success and failure diagnostics include screenshots and separated timings;
  and
- the ready static network artifact loads in the editor as one playable river.
