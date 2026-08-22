# PhysX PBD fluid-bake integration — design

**Date:** 2026-08-22
**Status:** draft for user review
**Order:** specification 2 of 2; implementation begins only after
`2026-08-22-gpu-visual-meshing-foundation-design.md` reaches its fluid
prerequisite acceptance gate
**Goal:** integrate NVIDIA PhysX's existing GPU PBD fluid implementation into
MatterEngine's hydrology bake, in-process, and prove that it can fill and flow
through the authored 100+ metre, approximately 15% ravine. MatterEngine owns
terrain authoring, bake orchestration, stopping rules, artifacts, meshing, and
rendering; PhysX owns all fluid simulation mathematics.

## 1. Product constraints

These are fixed by the gameplay and prior investigation:

1. Water is baked during level construction and is static during gameplay.
   Runtime terrain or players do not modify the solved water.
2. Gameplay is navigating rapids. The bake must provide a convincing visual
   surface and a velocity/depth field for buoyancy and current forces; it does
   not need engineering-grade hydraulic predictions.
3. No Matter-authored fluid solver is permitted. Integration code may marshal
   particles, collision geometry, emitters, sensors, and results, but it may not
   replace or modify PhysX's PBD constraint solve.
4. No secondary process is launched. The solver runs in the editor process.
5. The first accepted scope is one upstream section. The section begins at the
   authored inlet, is contained by terrain plus a generated virtual dam, and
   stops when the authored fill sensor remains wet for the required stable-step
   count.
6. The fluid domain boundary may cull work but may not act as a hidden wall.
   Terrain, boulders, and the explicit virtual dam are the only containing
   collision geometry.
7. The current CPU mesher remains available for query/collision output. The GPU
   visual mesher from specification 1 produces the high-resolution water mesh.
8. PhysX's isosurface extractor is not used.

## 2. Why PhysX PBD

PhysX 5 exposes `PxPBDParticleSystem`, GPU particle buffers, fluid phases, PBD
fluid materials, particle/rigid collision, final GPU positions and velocities,
and optional diffuse particles. NVIDIA ships `SnippetPBF` and
`SnippetPBFMultiMat` as setup references. PBD is designed for stable,
visually plausible particle dynamics at real-time-oriented timesteps, which
matches a game-content bake better than restarting another hydraulics project.

The spike treats the unmodified `SnippetPBF` behavior as its control. If the
official example does not build and run first, no Matter integration work
starts.

## 3. Dependency and ABI architecture

### 3.1 External source checkout

The PhysX repository stays outside the MatterEngine repository. A small tracked
lock file records the exact upstream URL, commit, SDK version, supported CUDA
version, and expected binary-interface version. The build receives the checkout
through `MATTER_PHYSX_ROOT`; it never clones or updates dependencies implicitly.

The repository remains buildable without PhysX. Hydrology authoring and CPU
meshing tests do not acquire a CUDA, MSVC, or PhysX dependency.

### 3.2 In-process C-ABI bridge

MatterEditor is built with MSYS2/UCRT64 while PhysX's Windows GPU SDK is built
with MSVC and CUDA. C++ objects must not cross that ABI boundary. A thin
Matter-owned bridge is compiled with the same MSVC/CUDA toolchain as PhysX and
emitted into the normal editor build directory:

```text
external PhysX checkout + integrations/physx_bridge sources
                           │ MSVC/CUDA
                           ▼
MatterEditor/build/windows/matter_physx_bridge.dll
                           │ versioned C ABI, same process
                           ▼
MatterEngine hydrology bake worker
```

The bridge links the official PhysX libraries and redistributable GPU modules.
MatterEngine loads it with `LoadLibrary`/`GetProcAddress` only when a PhysX bake
is requested. There is no solver executable, command line, GenCase, temporary
worker process, or DLL-visible C++ type.

### 3.3 Bridge responsibilities

The bridge may:

- create/release PhysX foundation, CUDA manager, physics, cooking, scene, PBD
  particle system, material, phases, and buffers;
- cook and install Matter-provided triangle collision meshes;
- copy initial particles and activate additional inlet particles;
- step `PxScene::simulate`/fetch in batches;
- evaluate inexpensive fill-sensor reductions;
- report progress, memory, particle, exclusion, and timing counters;
- copy the final positions and velocities to Matter-owned host buffers; and
- catch all C++ exceptions and translate PhysX error callbacks to stable bridge
  status codes.

It may not change PhysX kernels, fluid constraints, neighbor search, collision
resolution, or timestep integration.

### 3.4 Licensing and distribution

The dependency lock records every PhysX source, library, and GPU runtime staged
into the build. The build copies the corresponding upstream license and notice
files beside the staged bridge. Editor-development use may proceed once the
official SDK builds; packaging the bridge or NVIDIA GPU modules into a game
distribution requires a separate redistribution checklist against the pinned
PhysX release. Shipping static water artifacts never requires the game runtime
to contain PhysX.

## 4. Versioned C interface

The bridge exports one version negotiation function and opaque-session
operations. The exact spelling is finalized in the implementation plan, but the
semantic contract is:

```c
typedef struct MxpApiVersion {
    uint32_t abi;
    uint32_t physx_major;
    uint32_t physx_minor;
    uint32_t physx_patch;
} MxpApiVersion;

typedef struct MxpSession MxpSession;

MxpStatus mxp_create(const MxpCreateInfo*, MxpSession**, MxpError*);
MxpStatus mxp_set_collision_mesh(MxpSession*, const MxpTriangleMesh*, MxpError*);
MxpStatus mxp_set_emitters(MxpSession*, const MxpEmitter*, uint32_t, MxpError*);
MxpStatus mxp_set_sensor(MxpSession*, const MxpFillSensor*, MxpError*);
MxpStatus mxp_run(MxpSession*, const MxpRunConfig*, MxpProgressFn, void*,
                  MxpRunResult*, MxpError*);
void      mxp_cancel(MxpSession*);
void      mxp_destroy(MxpSession*);
```

All structs begin with `struct_size` and `abi_version`, use fixed-width integer
and IEEE scalar fields, and contain only caller-owned arrays described by pointer
plus element count. The allocator boundary is explicit: Matter allocates inputs;
the bridge allocates opaque session state; final arrays are copied into buffers
whose capacity Matter supplies. No STL, exceptions, RTTI, callbacks with C++
captures, or ownership ambiguity crosses the ABI.

`mxp_run` is synchronous on the hydrology bake worker. Progress callbacks occur
on that same worker and may only publish engine events or inspect cancellation.
They never call the renderer or UI.

## 5. Matter-side bake components

### 5.1 `PhysxBridgeLoader`

A platform-specific loader owns the DLL handle, validates the ABI and PhysX
version, resolves every required symbol atomically, and unloads only after all
sessions are destroyed. Failure is reported as `backend unavailable`, not as a
world-loader error. Worlds without a requested fluid bake never load the DLL.

### 5.2 `PhysxFluidBake`

A pure orchestration component converts a `RiverNetworkDefinition`, generated
`RiverGeometry`, terrain collision mesh, and bake settings into bridge inputs.
It owns the semantic key, progress mapping, cancellation, final validation, and
artifact assembly. It contains no fluid-force or pressure calculation.

### 5.3 `HydrologyArtifact`

The artifact is persistent and versioned independently from terrain parts. It
contains:

- schema and backend contract versions;
- river-network canonical hash and terrain/river-geometry revisions;
- PhysX SDK, bridge, PBD settings, and GPU provenance;
- section bounds, inlet definitions, virtual-dam definition, and sensor result;
- completed steps, simulated seconds, peak/active/excluded particle counts,
  memory high-water mark, and wall time;
- final particle snapshot for debug/optional rebake diagnostics;
- high-resolution visual mesh generated by the GPU visual mesher;
- coarse CPU query mesh;
- sampled height, depth, and velocity fields; and
- payload digest and explicit acceptance status.

Debug particle data may be stripped from a shipping artifact after visual and
gameplay products have been generated. The accepted mesh and gameplay fields
remain self-contained; runtime gameplay does not load PhysX.

## 6. Terrain and obstacle collision input

The first-section bounds come from the generated river geometry plus a dry
margin, not from a hard-coded fixed box. Matter's CPU terrain mesher samples the
same `FieldRuntime` and river height overlay used by rendering, at an authored
collision voxel size. Relevant sector buckets are combined into a world-space,
indexed triangle mesh and cooked by PhysX.

Collision input includes:

- the carved terrain surface and banks;
- authored/generated boulders intersecting the section;
- an upstream backing surface only if required to prevent particles escaping
  behind the inlet emitter; and
- the generated downstream virtual dam.

The section AABB and PhysX broadphase bounds have a dry collar. They are not
represented by colliders. A particle reaching the collar is counted as an
escape and makes the bake invalid; it is never reflected by an invisible box.

Before fluid work starts, a collision-only fixture drops probe particles above
the ravine and verifies that they settle on the rendered terrain within one
collision voxel. This catches axis, scale, winding, cooking, and transform
errors independently from fluid behavior.

## 7. Initial fluid and inlet behavior

### 7.1 Particle scale

Particle spacing is an authored bake-quality parameter constrained by the
channel width, boulder scale, GPU memory, and PhysX offset relationships. The
bridge derives rest/contact offsets and particle mass using the same formulas
as `SnippetPBF`; Matter does not invent alternative PBD parameterization.

The first ravine spike sweeps a small declared set of particle spacings rather
than tuning arbitrary forces. Each result records its complete PBD settings.

### 7.2 Emitters

The input model accepts one or more emitters so later tributaries do not require
an ABI change. Each emitter includes stable id, position/orientation, cross
section, flow rate, initial velocity, start time, and optional stop time. The
first acceptance scene uses the authored main-river inlet only.

Flow rate is converted deterministically into particle activation over time.
Fractional carry is retained between steps so long-term emitted volume matches
the authored flow. Newly active particles receive inlet velocity and density;
they are not teleported from already active water.

No downstream outflow is present in the first section. The virtual dam is the
explicit completion boundary. Sequential removal of that dam and generation of
the next section's inlet belongs to a later specification.

## 8. Run loop and completion

1. Validate backend, GPU, collision mesh, section bounds, PBD settings, particle
   capacity, emitter capacity, and sensor before creating a scene.
2. Create the scene and PBD system using the `SnippetPBF` reference setup.
3. Activate initial inlet particles and advance fixed simulation steps.
4. Activate each emitter's owed particles before each step.
5. After `batch_steps`, fetch counters and evaluate:
   - cancellation;
   - non-finite particle data;
   - PhysX-reported/excluded particles;
   - dry-collar escapes;
   - particle and memory caps; and
   - fill-sensor wet fraction.
6. The sensor is complete only after its wet fraction meets
   `crest_wet_fraction` for `stable_wet_steps` consecutive solver steps.
7. Stop successfully at sensor completion. Stop invalid at `max_steps`, capacity
   exhaustion, escape, non-finite state, unrecoverable PhysX error, or device
   loss.
8. Copy the final particle positions/velocities once and destroy the PhysX
   session after all Matter products have been generated or copied.

This is a fill-to-completion bake, not an inlet/outlet equilibrium solve. It
implements the virtual-dam workflow chosen for sequential river construction.

## 9. Fill sensor

The sensor is a thin volume immediately upstream of the virtual dam crest. Its
wet fraction is the fraction of horizontal sensor cells that contain at least
the configured minimum particle contribution, not simply a global particle
count. That prevents a narrow jet from falsely completing a broad section.

The bridge evaluates the occupancy reduction from device particle positions and
returns only counts per batch. This small CUDA reduction is data plumbing, not
fluid simulation. A CPU reference evaluates recorded snapshots in tests.

The artifact records the maximum, final, and stable-window wet fractions plus
the step at which completion was first and finally observed.

## 10. Result conversion

### 10.1 Visual mesh

Final fluid particles are converted to the particle-water job defined by the GPU
visual-meshing specification. PhysX positions and a chosen render radius become
Matter particle samples; PhysX's isosurface API is not created or called. The
result uses the existing glass/water material.

### 10.2 CPU query mesh

The same snapshot is deterministically downsampled by stable particle id or
meshed at a coarser voxel size through MatterSurfaceLib. This produces selection,
ray-query, fallback, and optional hull input. It is not used as the boat's
detailed water collision model.

### 10.3 Gameplay fields

Matter bins the final particles into a section-local regular field and records:

- surface height;
- water depth above terrain;
- occupancy/wet mask;
- volume-weighted velocity; and
- optional rapid intensity derived from velocity magnitude and local variation.

Empty samples are explicitly invalid rather than zero-current water. Runtime
queries interpolate valid neighbors and fail outside the wet mask.

## 11. Determinism and caching

PhysX GPU PBD is treated as numerically reproducible within tolerance, not
promised byte-identical across devices or driver versions. Matter must not hide
that limitation.

- The semantic cache key is derived from all inputs and contract versions.
- An accepted artifact is immutable and reused without rerunning PhysX.
- Same-machine repeat bakes compare sensor completion, wet envelope, volume,
  surface distance, and velocity statistics within declared tolerances.
- Particle arrays are sorted or consumed by stable particle id before Matter
  conversion so GPU scheduling order cannot churn downstream artifacts.
- The Matter GPU mesher itself retains its same-device byte-repeatability gate.
- Cross-device output is validated geometrically/statistically, not bytewise.

Changing PhysX version, bridge version, PBD material/offset/iteration settings,
particle spacing, terrain revision, network hash, dam/sensor settings, or
Matter-mesher contract invalidates the artifact.

## 12. GPU coexistence and editor behavior

PhysX owns a CUDA context on the same selected NVIDIA adapter used by Vulkan.
On Windows the bridge and engine compare the CUDA device identity with Vulkan's
device LUID before allocating the full particle buffers; a mismatch is a hard
bake error rather than an implicit cross-adapter copy.
The first integration performs no CUDA/Vulkan memory or semaphore interop. This
keeps the failure and lifetime boundary small:

1. PhysX runs on the hydrology bake worker.
2. The editor displays progress but may experience GPU contention during the
   explicit bake.
3. PhysX finishes and copies the final snapshot to host.
4. Matter releases or idles the PhysX scene.
5. The Vulkan GPU mesher runs through the normal renderer job seam.

The bake UI reports that an offline GPU bake is active. Runtime rendering must
remain responsive enough to show cancellation and status, but the first spike
does not promise background gameplay while the content bake saturates the GPU.

CUDA/Vulkan external-memory sharing is allowed only as a later optimization
with a separate design and measurement proving the final copy is significant.

## 13. Error and fallback policy

Stable error categories include:

- bridge DLL missing or ABI mismatch;
- PhysX or CUDA GPU unavailable;
- selected CUDA/Vulkan adapters do not identify the same physical GPU;
- PhysX initialization/cooking/scene failure;
- collision probe mismatch;
- particle, neighbor, memory, or output capacity exceeded;
- excluded, escaped, or non-finite particles;
- sensor not reached before `max_steps`;
- cancellation or superseded world generation;
- Matter CPU/GPU meshing failure; and
- CUDA or Vulkan device loss.

A failed fluid bake never prevents the dry terrain world from loading. It sets
`HydrologyStatus::Invalid`, retains logs and counters, and renders no result as
accepted water. A CPU visual fallback may aid diagnosis but is labeled fallback
and does not convert a failed simulation into `Ready`.

## 14. Verification sequence

### P0 — dependency and official control

- Resolve the pinned external checkout without network mutation.
- Build the official supported PhysX configuration and Matter bridge.
- Run unmodified `SnippetPBF` on the target RTX GPU.
- Record PhysX/CUDA versions, adapter identity, particle count, steps, timing,
  and final finite-state checks.

Failure stops the integration. Matter code is not changed to compensate for a
broken official control.

### P1 — bridge conformance

- ABI version and struct-size negotiation;
- create/destroy loop with leak checks;
- PhysX error and C++ exception translation;
- cancellation between batches;
- too-small output capacity reports required size without overwrite;
- one triangle, one box, and one sloped-ramp collision fixtures; and
- final positions/velocities match the official reference setup within
  tolerance.

### P2 — Matter chute

- Export a short, steep Matter terrain chute.
- Drop and release a bounded water volume.
- Verify terrain containment, downhill center-of-mass movement, finite
  particles, and no hidden-domain-wall contacts.
- Convert the result through both Matter meshers and render the existing glass
  material.

### P3 — authored ravine

Use `RiverHydrology.js` from the current branch:

- at least 100 metres from inlet to virtual dam;
- approximately 15% overall fall with reach variation;
- variable-width rounded-V channel;
- terrain walls larger than the fluid domain's dry collar; and
- no domain-edge containment.

The acceptance run must:

- reach the fill sensor before `max_steps`;
- keep every accepted particle finite and within the dry collar;
- show a connected wet path from inlet through the principal curve to the dam;
- produce nonzero downstream velocity through the curve;
- produce visual and gameplay artifacts without capacity truncation;
- complete within five wall-clock minutes on the reference RTX 4090 at a
  declared particle spacing and no more than two million active particles; and
- produce screenshots from overview, curve, downstream, and low river angles.

If the official PBD solver cannot meet these gates after a bounded sweep of
documented PhysX settings and particle spacing, the spike ends with a negative
recommendation. Matter does not respond by modifying the solver mathematics.

### P4 — cache and lifecycle

- second load is an artifact cache hit and performs no PhysX or GPU meshing;
- terrain/network/PBD/mesher version changes each invalidate the key;
- cancellation leaves no publishable partial artifact;
- world reload cannot publish an old generation; and
- editor shutdown releases CUDA, PhysX, and DLL resources without device loss.

## 15. Expected source layout

Implementation planning may refine filenames, but responsibilities remain:

- `integrations/physx_bridge/` — MSVC/CUDA C-ABI bridge and its C header;
- `tools/deps/physx.lock` — pinned external dependency identity;
- `MatterEngine3/src/hydrology/physx_bridge_loader.*` — Windows dynamic loading
  and ABI negotiation;
- `MatterEngine3/src/hydrology/physx_fluid_bake.*` — Matter orchestration,
  collision input, emitters, sensor, validation, and result conversion;
- `MatterEngine3/src/hydrology/hydrology_artifact.*` — versioned persistence;
- `MatterEngine3/include/matter/hydrology.h` — status visible to engine/editor
  consumers;
- `MatterEngine3/tests/physx_bridge_contract_tests.cpp` — fake bridge and ABI
  tests that run without PhysX;
- `MatterEngine3/tests/physx_fluid_integration_tests.cpp` — GPU-tagged control,
  chute, and ravine gates;
- `MatterEditor/Makefile` and a dedicated build script — opt-in bridge build and
  staging into `build/windows`; and
- `projects/world_demo/scenes/RiverHydrology/RiverHydrology.js` — the authored
  acceptance world, with solver-quality parameters added through the imperative
  build DSL rather than hidden environment values.

## 16. Non-goals for this specification

- runtime-changing water;
- two-way boat-to-water interaction;
- replacing Box3D rigid gameplay physics;
- modifying PhysX PBD kernels or constraints;
- engineering-grade discharge or flood prediction;
- full sequential multi-section river baking;
- tributary acceptance beyond preserving a multi-emitter interface;
- PhysX isosurface extraction;
- CUDA/Vulkan zero-copy interop; and
- GPU terrain meshing, which is Phase 2 of specification 1.

## 17. References

- PhysX PBD particle-system guide and `SnippetPBF` references:
  <https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/ParticleSystem.html>
- PhysX particle buffers and diffuse particle support:
  <https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/ParticleSystem.html#particle-buffers>
- PhysX repository and license:
  <https://github.com/NVIDIA-Omniverse/PhysX>
- GPU meshing prerequisite:
  `docs/superpowers/specs/2026-08-22-gpu-visual-meshing-foundation-design.md`
- River authoring contracts:
  `MatterEngine3/include/matter/river_network.h`
- Current acceptance world:
  `projects/world_demo/scenes/RiverHydrology/RiverHydrology.js`
