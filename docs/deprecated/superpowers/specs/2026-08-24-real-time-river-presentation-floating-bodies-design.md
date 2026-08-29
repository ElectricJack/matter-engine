# Real-time river presentation and floating bodies — design

**Date:** 2026-08-24

**Status:** approved (2026-08-24)

**Depends on:**
`2026-08-24-sequential-river-sections-waterfall-design.md`

**Goal:** prove that the accepted static river can drive passive Box3D crates
and raft-shaped boxes while a dedicated real-time water material animates
waves and whitewater along the baked flow and uses MatterEngine's ray tracer
for reflection, refraction, depth absorption, and in-water scattering.

## 1. Product decisions

The river simulation, accepted mesh, CPU query surface, gameplay field, and
ray-tracing acceleration structure remain immutable during play. Runtime work
is deliberately one-way:

```text
accepted PhysX bake
    |
    v
immutable river runtime/presentation field
    |---------------------|-----------------------|
    v                     v                       v
Box3D float forces   raster water shading   RT water transport
```

Dynamic rigid bodies react to the river but never modify it. The water mesh
does not deform, and its BLAS is not rebuilt. Visible motion comes from
flow-advected shading normals, foam, roughness, scattering, and optical
transport evaluated from the immutable field.

The first proof uses passive crates and flattened boxes acting as rafts. It
does not include player input, a vehicle controller, or scripted propulsion.
The result must run in real time rather than being an offline presentation
mode.

## 2. Milestone decomposition

The work is one product milestone implemented as three ordered slices:

1. **Floating-body proof.** Publish an immutable river runtime binding and
   apply sampled buoyancy/current forces to ordinary Box3D bodies.
2. **Flow-animated water material.** Derive and upload a presentation field,
   then animate waves and automatic whitewater along its velocity.
3. **Ray-traced water transport.** Use the same animated surface state for
   real-time reflection, refraction, absorption, scattering, and temporal
   confidence.

Each slice has independent tests and diagnostics. A later implementation plan
must keep these as reviewable vertical slices rather than combining the
physics and renderer changes into one untestable patch.

## 3. Scope

### 3.1 In scope

- one immutable CPU river-field binding installed only after a Ready network
  artifact is validated;
- one renderer binding for each accepted water network, with bounded slots and
  explicit instance-to-field identity;
- surface height, depth, full 3D velocity, base surface normal, turbulence,
  aeration, foam potential, wet validity, and feature classification;
- deterministic derivation and serialization of the new presentation data;
- multi-probe buoyancy and flow-relative drag for Box3D boxes;
- force-at-world-point support so distributed forces produce pitch, roll, and
  yaw naturally;
- crates of multiple dimensions/densities and flattened-cube rafts authored as
  normal DSL scene entities;
- flow-following wave normals at three spatial scales;
- automatic whitewater at rapids, shallows, boulder wakes, spillways,
  waterfall sheets, and impact zones;
- DSL material controls and local strengthen/suppress overrides;
- a dedicated water material domain shared by raster and RT shading;
- real-time Fresnel reflection, refraction, depth absorption, in-water
  scattering, foam transport, and temporal-denoiser integration;
- a deterministic RiverFloatLab acceptance scene using the accepted ravine;
  and
- matched stills, timed frame sequences, physics traces, and GPU/CPU timings.

### 3.2 Explicitly out of scope

- dynamic water geometry or per-frame water BLAS rebuilds;
- runtime PhysX PBD simulation;
- two-way rigid-body-to-water feedback, wakes, displacement, or splashes that
  alter the baked field;
- a controllable raft/kayak, player input, gameplay scripting, checkpoints, or
  level completion;
- underwater-camera medium transitions, waterline compositing, or underwater
  audio;
- full bidirectional/path-traced caustics;
- general atmospheric fog changes or replacing the froxel volume system;
- multiple vertically overlapping water layers at the same X/Z position;
- deformable/floating logs or arbitrary convex buoyancy in the first proof;
  and
- aerial waterfall mist beyond the existing volumetric-emitter system.

"Fog with depth" in this milestone means absorption and participating-media
scattering along a ray's path inside water. It is not global atmospheric fog.

## 4. River runtime and presentation field

### 4.1 Public runtime boundary

The current accepted `HydrologyNetworkProducts` remains provider-internal even
though it already contains surface height, depth, and velocity samples. Add a
Matter-owned public boundary, conceptually:

```cpp
struct RiverFieldSample {
    Float3 surface_position_m;
    Float3 surface_normal;
    Float3 velocity_mps;
    float depth_m;
    float turbulence;
    float aeration;
    float foam_potential;
    RiverFeature feature;
    bool wet_valid;
};

class RiverRuntimeBinding {
public:
    bool sample(Float3 world_position_m, RiverFieldSample& out) const noexcept;
};
```

`WorldSession` exposes an immutable, generation-safe binding or a batch query
over that binding. No public type exposes provider, PhysX, Vulkan, or Flecs
implementation details. Publication uses the same generation/Ready boundary
as the visual water mesh, so physics and rendering cannot observe different
network generations.

The installed binding covers the assembled network, not individual section
artifacts. Sampling across a spillway handoff therefore uses one layout and
one interpolation contract. Invalid or dry cells return `false`; callers must
not interpret them as zero-velocity water.

### 4.2 Derived samples

The existing height, depth, and mean velocity remain authoritative gameplay
inputs. The presentation build adds deterministic values derived from the
accepted particle snapshot and shared terrain/collider data:

- base normal from the accepted free surface and height gradients;
- velocity variance inside the sampling footprint;
- horizontal divergence and vorticity;
- vertical-speed and surface-slope contributions;
- shallow-water contribution;
- aeration from vertical motion, velocity variance, and waterfall/impact
  markers;
- foam potential from a bounded weighted combination of turbulence, aeration,
  shallows, obstacle wakes, spillways, and authored local overrides; and
- a stable feature enum: calm, current, rapid, waterfall, impact, spillway, or
  pool.

The recipe and all weights are versioned and hashed. The computation may use
authored waterfall/pool/spillway markers, but it may not invent new river
features or change hydrology acceptance.

The X/Z lattice has one free-surface sample per cell. A waterfall sheet may
carry vertical velocity and the waterfall feature, but it is not treated as a
second floatable horizontal surface. This matches the accepted test geometry,
whose fall does not overlap another water layer at the same X/Z location.

### 4.3 Artifact identity

Simulation identity remains separate from product and appearance identity:

- changing water color, wave scale, foam intensity, or RT quality never reruns
  PhysX;
- changing the presentation derivation recipe rebuilds only the presentation
  product from the accepted particles/terrain/markers;
- changing height/depth/velocity extraction invalidates runtime interaction
  and downstream presentation products; and
- a shader change invalidates renderer pipeline/cache identity, not section
  simulation artifacts.

The network manifest records the runtime-field and presentation-field digests.
A ready package stages them as part of the exact hydrology dependency closure.

### 4.4 GPU representation

The serialized artifact remains renderer-independent. The Vulkan upload
converts it into bounded sampled images associated with a water-binding slot:

- `RGBA16F`: surface height, depth, horizontal velocity X/Z;
- `RGBA16F`: vertical velocity, base-normal X/Z, turbulence;
- `RGBA8_UNORM`: aeration, foam potential, wet validity, feature value.

The missing normal Y is reconstructed as the positive hemisphere. The field's
origin, cell size, dimensions, and world-to-field transform live in a small
uniform/storage record. Linear filtering is allowed only for continuous
channels; validity and feature classification use explicit nearest/validated
sampling. Border sampling is clamped and then rejected when the authoritative
validity channel is dry.

The renderer uses an explicit per-water-instance binding index rather than
assuming a single global river or inferring a field from material id. Initial
capacity is eight simultaneously resident water-network bindings. Slot
exhaustion fails the new binding while retaining the last valid generation.

## 5. Floating rigid bodies

### 5.1 Authored component

Crates and raft boxes remain ordinary world-authored ECS/Box3D entities. A
small runtime component, conceptually `RiverFloatBody`, declares:

- displaced volume or effective density;
- probe lattice dimensions and inset;
- vertical buoyancy response;
- longitudinal/lateral/vertical drag coefficients;
- angular damping in water;
- maximum per-probe force and maximum total hydrodynamic force; and
- optional diagnostic naming/color.

The DSL may provide crate and raft helpers, but their result is explicit scene
entities and component data. The river generator does not spawn gameplay
objects.

### 5.2 Fixed-tick force model

Each body resolves a deterministic lattice of sample points through its box
volume. At every fixed tick, before the Box3D step:

1. Transform every probe into world space.
2. Sample the immutable river field.
3. Compute bounded submersion from surface height and the probe's represented
   vertical volume.
4. Apply the probe's share of displaced-water buoyancy upward.
5. Compute point velocity from body linear and angular velocity.
6. Apply drag from the difference between sampled 3D water velocity and point
   velocity.
7. Apply the force at the probe position, producing physical torque.

The nominal force forms are:

```text
F_buoyancy = water_density * displaced_probe_volume * gravity * submerged_fraction
F_drag     = 0.5 * water_density * C_d * area * |v_relative| * v_relative
```

This is a stable gameplay approximation, not a CFD pressure solve. Coefficients
and force caps are authored/versioned. No force is applied for invalid/dry
samples, non-finite body state, or a stale binding. The runtime reports and
disables a body that repeatedly produces invalid hydrodynamic input rather
than feeding a bad value into Box3D.

The physics API adds a queued `apply_force_at_world_point` command. It follows
the existing command ordering, world-ownership validation, trace, and deferred
Box3D mutation rules; gameplay systems may not call Box3D directly.

### 5.3 Waterfall and collisions

Buoyancy support tapers out at the authored lip. The waterfall feature is not
treated as a horizontal supporting surface, although its downward velocity may
contribute bounded drag while a body intersects the sheet. Gravity owns the
free fall. Pool probes reacquire valid water on impact, where submersion and
relative-velocity drag are force-capped to avoid an explosive impulse.

Terrain and DSL-authored boulders remain ordinary Box3D collision geometry.
Bodies may plausibly ground, collide, spin, or become trapped. Acceptance uses
one reference crate and one reference raft on a clear deterministic lane that
must traverse the upper reach, waterfall, first pool, spillway, and enter the
lower reach; additional bodies intentionally exercise collisions and may snag.

### 5.4 Reset and determinism

RiverFloatLab authors at least 24 bodies across upper rapids, boulder wakes,
the waterfall approach, and the first spillway. Existing Play/Stop snapshot
semantics restore initial transforms and velocities. Fixed-tick replay records
binding generation, field sample checksums, applied forces, and body transforms
so two runs from the same state can be compared exactly within the engine's
established physics determinism contract.

## 6. Flow-animated water material

### 6.1 Dedicated material domain

Accepted water no longer requires generic builtin glass material 4. The water
mesh stores a resolved material identity and an explicit water-surface flag or
domain. The renderer validates that identity and selects the dedicated water
path; it does not special-case a numeric material id.

The water material retains ordinary PBR values such as base color, roughness,
IOR, absorption, scattering, and transmission. Water-only controls live in a
separate bounded parameter record rather than expanding every generic
`MaterialDef` with river animation state.

One shader include, conceptually `water_surface.glsl`, owns world-to-field
sampling, flow backtracing, wave normals, foam evaluation, optical modulation,
and temporal reactivity. Raster primary shading and RT water-hit shading call
the same functions with world position, geometric/base normal, material
parameters, binding slot, and animation time.

### 6.2 Flow coordinates and phase continuity

The static water mesh has no meaningful river UV parameterization. Animation
therefore starts in world space and traces sampling coordinates backward
through the local baked velocity field. A bounded RK2/semi-Lagrangian trace
over a short loop period follows curved flow better than subtracting one local
velocity vector for unbounded time.

Every periodic wave layer evaluates two wrapped phases offset by half a cycle
and crossfades their reset windows. No layer may jump when its time coordinate
wraps. Flow speed is clamped for shading stability but retains direction and
relative acceleration through rapids/spillways. Dry or invalid backtrace steps
fall back to the last valid coordinate and raise diagnostic counters only when
persistent.

### 6.3 Wave bands

Three bands build the apparent surface:

1. broad flow-aligned undulations in deeper/current water;
2. short choppy waves strengthened by speed, slope, and turbulence; and
3. fine capillary detail for close views.

Each band has wavelength, normal amplitude, temporal scale, and response
curves. The final shading normal is energy-bounded and remains in the geometric
surface hemisphere. Calm pools reduce high-frequency amplitude; rapids raise
it; foam softens coherent normal energy. No band changes vertex position,
depth, motion vectors, collision, or the TLAS/BLAS.

### 6.4 Automatic whitewater

Whitewater is the product of a stable baked macro mask and advected runtime
detail:

```text
foam = classify(foam_potential, aeration, feature, local overrides,
                flow-advected breakup noise)
```

The macro mask keeps whitewater attached to real rapids, boulder wakes,
spillways, waterfall sheets, and impact zones. Advected detail provides moving
streaks, holes, and bubbles without letting the whole patch slide away from
its cause.

Foam modifies a physically coherent group of properties: higher diffuse
albedo and scattering, higher roughness, reduced transmission, softened
surface-normal energy, and increased temporal reactivity. It is not an opaque
decal. Authored local volumes can multiply, suppress, or threshold the
automatic potential; absent overrides, the bake is fully automatic.

### 6.5 DSL controls

The scene DSL owns artistic intent, with builder-style controls for:

- shallow/deep absorption color and absorption distance;
- scattering color, distance, and anisotropy/turbidity;
- wave-band wavelength, normal amplitude, and speed multiplier;
- foam threshold, gain, persistence, breakup scale, and optical response;
- quality-independent physical IOR; and
- explicit local foam/wave strengthen or suppress volumes.

Controls canonicalize deterministically. Invalid physical values fail with a
source-oriented field name; they are not silently coerced into a different
look. Quality tiers select cost, not authored water identity.

## 7. Ray-traced water transport

### 7.1 Existing foundation

MatterEngine's current RT path already implements bounded entry/exit
refraction, total internal reflection, path-length tracking, GGX reflection
and rough transmission, material IOR, Beer-Lambert absorption, and a dedicated
transmission denoiser. The new work specializes and extends those mechanisms;
it does not add a separate ray tracer.

### 7.2 Surface transport

For a primary water surface, the animated shading normal drives a
microfacet-consistent Fresnel split. Reflection rays see normal scene TLAS
content, including terrain, boulders, crates, rafts, sky, and atmosphere.
Refraction begins below the geometric surface, follows the existing bounded
interior walk, and sees the actual riverbed and submerged objects.

The geometric normal continues to control ray-origin offsets, entry/exit
orientation, and self-intersection safety. The animated shading normal controls
the optical lobe only and may never move a ray origin below the wrong side of
the triangle.

Water encountered by a secondary ray uses the same material/normal evaluation
for its local incident shading, but the milestone does not introduce unbounded
recursive water reflection/refraction.

### 7.3 Absorption and depth fog

Interior path length is the primary distance measure. The baked depth field is
a conservative fallback when a clipped handoff edge, open numerical boundary,
or bounded walk cannot produce a reliable exit. The fallback is diagnostics-
visible and must not hide systematic mesh defects.

For traveled distance `d`:

```text
T(d) = exp(-extinction * d)
L    = refracted_radiance * T(d)
     + in_scattered_sun_sky * (1 - T(d))
```

Absorption is wavelength-dependent; scattering has an authored color,
distance, and bounded anisotropy. This produces clear shallows, attenuated
riverbed detail, and deeper blue/green haze without applying atmospheric fog
in front of the water surface.

Whitewater is an aerated layer, so it increases diffuse scattering and
roughness while reducing coherent refraction. It still conserves bounded
energy between reflection, transmission, and diffuse/scattered contributions.

### 7.4 Caustics and quality tiers

High quality uses one bounded reflection/refraction path, analytic single
scattering, three wave bands, foam, and temporal denoising. Ultra adds one
rough-lobe sample, additional bounded internal-reflection allowance, and a
shallow-bed caustic approximation derived from flow-advected normal focusing.
It does not implement photon mapping or bidirectional path tracing.

Every loop and ray count is specialization/config bounded. Ultra changes
sample count and optional effects only; it does not change field derivation,
physics, or artifact identity.

## 8. Temporal behavior and renderer integration

Because the mesh is static, ordinary motion vectors are zero even while the
material changes. `water_surface.glsl` therefore emits a reactivity/confidence
value based on wave-normal change, foam motion, turbulence, and disocclusion.
The temporal and transmission-denoiser paths reduce history weight for highly
reactive water while retaining long history for calm pools.

Raster G-buffer normals/roughness and RT primary transport must agree on the
same time, binding generation, field sample, and water parameters. A generation
change invalidates affected temporal history. A frame may not combine a new
field texture with an old parameter record or vice versa.

Add explicit GPU timing zones for field sampling/water shading where separate
passes exist, and report the water-enabled delta in the normal performance
output. Shader sampling has no per-frame CPU upload beyond time and immutable
binding state.

## 9. Performance and quality gates

The acceptance target is the installed NVIDIA GeForce RTX 4090 at 2560x1440.
Measure the same settled RiverFloatLab camera/timeline with water animation off
and on; report total and incremental median, p95, and p99 frame time.

### 9.1 High

- median total frame time at or below 16.7 ms;
- p95 at or below 22.0 ms and p99 at or below 33.3 ms during the fixed
  acceptance timeline;
- animated water adds no more than 3.0 ms median over the matched static-water
  RT scene;
- one bounded reflection/refraction path;
- three animated wave bands, automatic foam, scattering, and temporal
  reactivity enabled; and
- no sustained shader/denoiser ghost visible after foam has moved away.

### 9.2 Ultra

- median total frame time at or below 33.3 ms;
- p95 at or below 41.7 ms and p99 at or below 50.0 ms during the fixed
  acceptance timeline;
- all ray/refraction loops remain explicitly bounded;
- the extra rough-lobe sample and shallow caustics are separately timed; and
- disabling either Ultra feature restores the corresponding measured
  cost without changing water state.

### 9.3 Physics

- at least 24 simultaneous float bodies add no more than 0.5 ms median to the
  fixed-tick CPU phase on the acceptance machine;
- the float phase remains at or below 1.0 ms at p99;
- the float system performs no steady-state heap allocation;
- force-at-point commands stay within the existing physics command budget; and
- no non-finite force, velocity, transform, or river sample is submitted.

These are target-machine proof gates, not minimum shipping hardware
requirements. A later gameplay milestone defines broader hardware tiers.

## 10. Failure and fallback policy

- A corrupt or digest-mismatched field prevents that network generation from
  publishing; the prior valid binding remains installed.
- A Ready visual mesh may not publish with a different runtime-field
  generation.
- Invalid/dry samples apply no hydrodynamic force and increment a bounded
  diagnostic counter; they are never treated as stationary water.
- A persistent non-finite body or force disables float behavior for that body
  and reports its stable scene identity.
- Presentation-field GPU upload or slot failure keeps the last valid renderer
  binding. If none exists, the water renders with clearly reported static
  fallback shading and physics remains tied to the valid CPU field.
- A shader compilation/pipeline failure follows the renderer's existing
  fail-closed last-valid policy.
- Capacity overflow, invalid field dimensions, unsupported overlapping water
  layers, and invalid DSL optical values fail with stable categories.
- A depth-field fallback during RT is counted. Repeated fallback above an
  acceptance threshold is a defect, not a reason to loosen validation.

## 11. Testing

### 11.1 Field and artifact tests

- deterministic particle/terrain/marker input produces byte-identical field
  payloads and keys;
- serialize/load preserves every channel and rejects corruption/truncation;
- simulation, presentation, shader, and artistic keys invalidate only their
  prescribed dependents;
- handoff sampling has bounded height, velocity, normal, turbulence, and foam
  discontinuity;
- dry/edge/invalid interpolation fails rather than bleeding valid water across
  terrain; and
- package staging requires the field payloads named by the Ready manifest and
  rejects stale extras.

### 11.2 Float-system tests

Use a fake analytic field to verify:

- density-appropriate equilibrium draft for crate and raft aspect ratios;
- convergence toward uniform current without overshoot instability;
- distributed force torque in a velocity gradient;
- lateral drag and angular damping;
- dry exit, unsupported waterfall fall, pool re-entry, and capped impact;
- force-at-world-point command ordering and world ownership;
- invalid sample/non-finite rejection;
- zero steady-state allocation; and
- fixed-tick replay/checkpoint restoration.

### 11.3 Shader and RT tests

- CPU reference vectors cover world-to-field mapping, RK2 backtrace, wrapped
  crossfade, normal hemisphere bounds, foam classification, and optical energy
  bounds;
- raster and RT shader code consume the same shared water functions;
- a Vulkan `rt-water` smoke mode covers reflection, refraction, actual and
  fallback thickness, absorption, scattering, foam, reactivity, field
  generation changes, and validation errors;
- calm-water temporal history remains stable while moving foam rejects stale
  history; and
- High/Ultra specialization bounds match the documented ray/sample limits.

### 11.4 Real visual/physics acceptance

RiverFloatLab must capture:

1. calm-pool reflection/refraction and depth gradient;
2. upper rapid flow-aligned waves;
3. crate and raft floating draft;
4. boulder collision/wake whitewater;
5. waterfall approach;
6. crate and raft free fall;
7. plunge-pool impact/recovery;
8. first-pool settling;
9. spillway handoff traversal;
10. lower-rapid progression; and
11. low player-height RT water views.

Four timed sequences cover upper-rapid advection, waterfall traversal,
plunge-pool recovery, and spillway traversal. Each captures at least eight
frames at fixed 30-tick intervals from a matched camera, plus a contact sheet,
so wave advection, foam motion, and body progression are visible rather than
inferred from one still. Every promised image has a `.done` sidecar, the editor
exits cleanly, and the run directory includes body traces, field diagnostics,
renderer stats, GPU/CPU timings, and the exact quality/material settings. A
live editor run is the final user-facing acceptance gate.

## 12. Implementation sequence

The later implementation plan should preserve these vertical slices:

1. Publish/version/query the assembled runtime and presentation field with
   debug visualization, without changing rendering or physics.
2. Add force-at-world-point and the deterministic fake-field float system.
3. Author RiverFloatLab crates/rafts and prove the real river physics path.
4. Replace hardcoded glass identity with a dedicated water material/binding.
5. Upload the field and add flow-coordinate/wave animation with raster/RT
   agreement.
6. Add automatic foam and temporal reactivity.
7. Add water absorption/scattering specialization and bounded Ultra options.
8. Run the complete performance, visual, packaging, and live-editor gates.

No slice may compensate for a failed earlier contract by reading provider
internals, calling Box3D directly, using a global singleton river, hardcoding a
material number, or rebuilding the water BLAS.

## 13. Acceptance gate

The milestone is complete only when:

- one reference crate and one reference raft traverse the upper rapids,
  waterfall, first pool, spillway, and enter the lower reach under passive
  sampled forces;
- additional crates/rafts demonstrate stable floating, collisions, tumbling,
  grounding, and pool settling;
- the same immutable field and generation drive physics, raster water, and RT
  water;
- waves and foam follow local curved flow with no wrap jump or section seam;
- whitewater appears automatically at the accepted physical features and
  remains artist-adjustable through the DSL;
- reflection, refraction, depth absorption, and in-water scattering remain
  temporally stable and energy-bounded;
- the water mesh, CPU surface, simulation, and BLAS remain static at runtime;
- High and Ultra meet their real-time gates on the target RTX 4090;
- all CPU, physics, artifact, shader, Vulkan, RT, packaging, and WSL/MSVC gates
  pass; and
- the user can watch the complete proof live in RiverFloatLab.
