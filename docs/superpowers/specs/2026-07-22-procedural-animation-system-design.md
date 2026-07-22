# Procedural Animation System — Design

**Date:** 2026-07-22
**Status:** Approved design; awaiting written-spec review
**Primary runtime dependency:** ozz-animation, wrapped behind MatterEngine APIs

## Summary

MatterEngine will add a procedural-first animation system in which JavaScript remains the
only authored source of truth. Authors define a skeleton, derive geometry from its bone
segments, generate clips, declare a motion graph, and expose named graph inputs and IK
targets from the part DSL. MatterEngine compiles that source into disposable,
content-addressed `.part` and `.anim` artifacts.

ozz-animation supplies the optimized skeleton, clip sampling, blending, local-to-model,
and two-bone IK runtime primitives. MatterEngine owns the authoring model, procedural
geometry bindings, animation graph, ECS integration, root-motion policy, renderer bridge,
editor tools, and all public APIs. No JavaScript-facing API exposes ozz types.

The system supports three geometry modes within the same rig:

- continuous skinned geometry generated from the skeleton;
- rigid per-joint segments whose raster and ray-tracing instances move by transform only;
- independently instanced `.part` attachments placed at joints or sockets.

Generated clips and continuously evaluated native controllers coexist in a staged motion
pipeline. Runtime JavaScript may update only inputs and targets declared when the `.anim`
artifact was built. It does not write joint transforms directly or execute callbacks inside
the per-frame pose evaluator.

## Motivation

Conventional animation integrations assume imported skeletons, meshes, weights, and clips.
That is the wrong center of gravity for MatterEngine. Its durable content is JavaScript DSL
source; meshes and other binary products are reproducible caches. Procedural geometry must
therefore be a first-class participant in rig construction rather than a mesh attached after
an imported animation pipeline has already been chosen.

A skeleton-first workflow gives procedural authors a compact way to create animated forms:

1. Grow a hierarchy by walking and branching through bone segments.
2. Store shape hints such as radius on the hierarchy.
3. Generate voxel envelopes or rigid segments from those hints.
4. Add exceptional geometry as instanced part attachments.
5. Generate reusable clips or declare continuous native controllers.
6. Drive the result through a small set of named runtime inputs and target transforms.

This applies equally to creatures, plants, machinery, doors, tentacles, tools, and abstract
kinetic structures. The public terminology must remain rig-oriented rather than
character-oriented.

## Goals

- Make skeleton construction concise and stateful in the existing Processing-style DSL.
- Make geometry generation from bones the default workflow.
- Retain JavaScript as the only canonical authored data.
- Use ozz-animation for efficient native runtime pose evaluation.
- Support generated clips, blending, additive motion, native procedural controllers, IK,
  constraints, markers, and opt-in root motion.
- Let runtime JavaScript drive declared semantic inputs and targets without owning the pose
  evaluator.
- Support skinned, rigid-segmented, attached, and hybrid rigs.
- Integrate additively with the Flecs runtime and existing `PartInstance` model.
- Allow the core compiler and runtime work to proceed before the editor event-system branch
  lands.
- Provide useful debug visualization in the isolated Part Workbench preview.

## Non-goals

- Importing production DCC formats, authored meshes, skeletons, or animation clips.
- Making ozz types part of MatterEngine's public ABI or JavaScript API.
- Running arbitrary JavaScript callbacks during frame pose evaluation.
- A visual node-graph authoring system. JavaScript remains authoritative.
- Direct animation-system ownership of gameplay or physics transforms.
- A fully general inverse-kinematics constraint language in the first delivery.
- Persisting editor-authored animation state outside the JavaScript module.
- Network replication of raw poses. A later network layer can replicate semantic inputs,
  targets, clocks, or authoritative transforms as appropriate.

## Decisions of Record

| Question | Decision |
|---|---|
| Runtime library | Use ozz-animation behind a Matter-owned adapter |
| Authored source | JavaScript DSL only |
| Derived artifacts | A resolved part hash may produce sibling `.part` and `.anim` files |
| Primary workflow | Build a skeleton first, then derive or attach geometry |
| Rig DSL style | Stateful cursor and branch stack with concise defaults |
| Clip authoring | Bake-time generated clips plus explicit keyframes as an escape hatch |
| Runtime control | Declared graph inputs and declared named targets only |
| Procedural evaluation | Native compiled controllers; no per-frame JavaScript callbacks |
| Graph form | Ordered stages, not an unrestricted user node graph |
| IK selection | Public API names a start and end joint; engine derives the chain and solver |
| Target toggling | Enable/disable changes desired weight with configurable smoothing |
| Geometry modes | Skinned, rigid segments, attached parts, or any hybrid of them |
| ECS model | `PartInstance + Animator`; no separate `AnimatedPart` entity type |
| Root motion | Emit desired root motion for an authoritative consumer to apply or reject |
| Skeleton display | Part Workbench debug overlays, never baked render geometry |
| Event dependency | Core work proceeds independently; editor hub wiring follows the event branch |

## Terminology

- **RigAsset** — immutable compiled skeleton, hierarchy, bind pose, joint metadata, and
  sockets shared by animator instances.
- **AnimationAsset** — immutable compiled clips, motion program, declared input schemas,
  target schemas, geometry bindings, and renderer metadata stored in `.anim`.
- **AnimationGraph** — the compiled staged motion program. The public name remains graph
  even though evaluation order is constrained.
- **AnimatorInstance** — mutable per-entity clocks, input values, target state, graph state,
  pose buffers, and status.
- **RigTarget** — a named transform input bound to an inferred IK chain.
- **JointAttachment** — a `.part` instance bound to a joint or named socket.
- **Rigid segment** — geometry owned by one joint and moved only by its model transform.
- **Skinned region** — geometry deformed by weighted joint transforms.

Names such as `Character`, `CharacterAsset`, and `CharacterController` are deliberately not
used in public interfaces.

## System Boundaries

The integration is split into six independently testable units:

1. **Ozz adapter** converts Matter rig and clip build products into ozz offline/runtime
   objects and exposes Matter-native sampling, blending, local-to-model, and IK operations.
2. **Animation compiler** executes the DSL at bake time, validates it, generates geometry
   bindings and clips, and writes `.anim`.
3. **Animation runtime** evaluates immutable assets and mutable instances without renderer
   or editor knowledge.
4. **ECS integration** schedules fixed and frame evaluation, publishes status, markers, and
   desired root motion, and owns instance lifetime.
5. **Renderer bridge** consumes immutable pose snapshots and chooses rigid, skinned, proxy,
   or exact ray-tracing paths.
6. **Editor integration** observes artifacts and runtime state, renders debug overlays, and
   reports commands and diagnostics through the editor event architecture.

Dependencies flow in that order. In particular, the animation runtime does not call the
renderer, the editor does not own runtime state, and the ozz adapter has no JavaScript or
Flecs dependency.

## Source and Artifact Model

For a resolved part hash `<hash>`, the cache may contain:

```text
parts/<hash>.part   # bind-pose/static proxy geometry and normal part metadata
parts/<hash>.anim   # rig, motion, bindings, and animation renderer metadata
```

Static parts produce only `.part`. Animated parts normally produce both. Both files are
derived from the same resolved JavaScript module and dependency graph; neither is canonical
content. Deleting either cache file must be harmless because the host can rebuild it from
source.

The resolved hash covers:

- canonical module source and imported module hashes;
- evaluated build parameters;
- relevant part-compiler and animation-schema versions;
- the pinned ozz version and adapter serialization version;
- geometry-binding and clip-generation settings that affect output.

The `.anim` file begins with a Matter-owned versioned header and section table. Sections
contain the Matter joint table and names, compiled ozz skeleton and clip payloads, graph
program, input and target schemas, sockets, skin influence streams, rigid-segment records,
attachment records, animated bounds metadata, and optional debug metadata. Consumers must
not infer the file layout from ozz serialization internals.

## Ozz Integration

The adapter is the only code allowed to include ozz headers outside its implementation and
focused tests. Its public API uses Matter transforms, spans, handles, diagnostics, and owned
byte buffers. JavaScript and ECS code never retain ozz pointers.

At bake time the adapter:

- builds and validates the ozz runtime skeleton from the Matter joint hierarchy;
- samples procedural clip generators at the declared rate;
- builds and optimizes ozz animation clips;
- serializes pinned-version runtime payloads into `.anim` sections;
- reports failures as structured Matter diagnostics.

At runtime it:

- samples clips into local-space SoA transforms;
- blends base and additive layers;
- converts local poses to model-space joint matrices;
- applies supported IK primitives selected by MatterEngine;
- exposes no asset-loading or rendering policy.

All JavaScript coordinates use the existing MatterEngine transform convention. Any basis,
handedness, or quaternion-layout conversion required by ozz occurs exactly once in the
adapter and is verified with asymmetric reference poses. Authors never compensate for the
runtime library.

## Stateful Rig DSL

A rig session has a current joint, current radius, current default orientation, and a branch
stack. `root()` creates the first joint. `bone()` adds a child at a local endpoint and makes
that child current. `push()` and `pop()` preserve and restore the branch cursor. Radius is a
joint property: a segment tapers from its parent joint's radius to its child joint's radius.

Illustrative authored source:

```js
const rig = beginRig("walker");

radius(0.24);
root("hips", [0, 0.95, 0]);

radius(0.18);
bone("spine", [0, 0.42, 0]);
radius(0.13);
bone("chest", [0, 0.34, 0]);

push();
  radius(0.10);
  bone("leftUpperArm", [0.34, 0.05, 0]);
  radius(0.075);
  bone("leftHand", [0.32, -0.03, 0]);
pop();

mirrorBranch("leftUpperArm", "rightUpperArm", { axis: "x" });

atJoint("hips");
push();
  radius(0.14);
  bone("leftUpperLeg", [0.16, -0.44, 0]);
  radius(0.11);
  bone("leftFoot", [0, -0.43, 0.08]);
pop();

mirrorBranch("leftUpperLeg", "rightUpperLeg", { axis: "x" });
endRig();
```

The exact JavaScript binding retains these semantics:

| Verb | Effect |
|---|---|
| `beginRig(name?)` | Opens one rig session and returns its stable bake-time handle |
| `root(name, position?, rotation?)` | Creates the sole root and selects it |
| `bone(name, endpoint, rotation?)` | Adds and selects a child using a parent-local endpoint |
| `radius(value)` | Changes the cursor radius; the next joint stores it |
| `push()` / `pop()` | Saves/restores the current rig branch cursor |
| `atJoint(name)` | Selects an existing joint for a new branch or metadata |
| `mirrorBranch(fromRoot, toRoot, options)` | Reproduces one named subtree and deterministically renames its root and descendants |
| `socket(name, transform?)` | Adds a named transform relative to the current joint |
| `endRig()` | Validates and closes the rig session |

Rig-session `push()` and `pop()` operate on the rig branch stack; geometry transform-stack
verbs retain their existing behavior outside a rig session. Sessions may not overlap, and
build finalization rejects an open session or unbalanced branch stack.

Joint names are unique UTF-8 identifiers within a rig and are the stable authoring keys for
clips, targets, sockets, debug tools, and live-reload migration. Runtime evaluation uses
compact indices resolved during compilation.

## Geometry From the Rig

The compiler iterates parent-child segments and can produce three binding classes.

### Automatic skinned geometry

`skin(rig, options?)` emits a continuous voxel envelope along all selected, unclaimed
segments. With no options it uses the radii stored on joints and generates the complete
surface. The bake also creates a compact influence field, samples that field at final mesh
vertices, retains at most the configured maximum influences, and normalizes weights.

The default maximum is four joint influences per vertex. Influence computation is
deterministic and operates in bind-pose space. Geometry simplification and LOD generation
must preserve or deterministically reproject joint weights. Skinning the bind pose must
reproduce the generated source mesh within a documented numerical tolerance.

### Rigid segments

`segments(rig, options?)` emits one rigid geometry record for each selected, unclaimed bone
segment. Each record has exactly one owning joint transform. Geometry and BLAS data remain
immutable while animation updates the raster instance transform and TLAS transform.

This path is preferred for articulated machinery, block figures, hinged structures, and
other designs that do not require a welded deforming surface. It avoids vertex skinning and
per-frame BLAS reconstruction.

### Part attachments

`attach(rig, jointOrSocket, module, options?)` records a normal content-addressed `.part`
instance under a joint or socket. Attachments keep their own part identity, material data,
LOD policy, raster instances, and BLAS. The renderer composes the attachment's local
transform with the evaluated joint transform.

### Hybrid binding

A rig may use all three modes. Selection options and the `bind()`/`withBinding()` escape
hatch assign explicit joint subsets or generated regions. A segment may have only one
primary generated binding, but attachments do not claim the segment and may coexist.
Overlapping explicit selections are a compile error unless an option deliberately declares
decorative overlap.

`skin(rig)` and `segments(rig)` are intentionally the common one-line forms. Fine-grained
binding exists for exceptional rigs and must not make the default case verbose.

## Generated Clip DSL

Clip sessions author local joint motion against a rig. A clip declares duration or derives
it from key times, loop policy, sample rate, and override/additive mode. The pose cursor
selects a joint with `at(joint)`, after which translation and rotation verbs modify that
joint's authored local transform.

```js
const walk = beginClip(rig, "walk");
loop(true);
duration(1.0);
sampleRate(30);
mode("override");

generate(phase => {
  const swing = Math.sin(phase * Math.PI * 2);
  at("leftUpperLeg");  rotateX(swing * 0.55);
  at("rightUpperLeg"); rotateX(-swing * 0.55);
  at("leftHand");     rotateX(-swing * 0.32);
  at("rightHand");    rotateX(swing * 0.32);
});

marker(0.0, "left_step");
marker(0.5, "right_step");
endClip();
```

The `generate` callback runs only while compiling the part. The compiler invokes it at
deterministic sample phases, captures local transforms, and builds an optimized ozz clip.
It never survives into `.anim` and is never called by the runtime evaluator.

Explicit keyframe verbs are provided for discontinuities and exact authored timing.
`mirrorPose()` and clip mirroring use compiled joint-name mappings rather than string
replacement at runtime. Missing tracks inherit the bind pose for override clips and the
identity delta for additive clips.

Markers are immutable clip metadata. Fixed-tick clock advancement determines marker
crossings so frame-rate variation does not duplicate or omit them.

## Declared Motion Graph

The graph is declarative but deliberately not an arbitrary graph language. Compilation
places nodes into this fixed pipeline:

```text
declared inputs
    -> base clip selection and blending
    -> additive layers
    -> continuous native controllers
    -> IK targets
    -> joint constraints
    -> final local pose
    -> local-to-model palette
    -> pose snapshot + desired root motion
```

This order makes pose ownership predictable, prevents cycles, and gives MatterEngine room
to optimize storage and evaluation. Each node declares the inputs and joints it reads and
the pose channels or targets it writes. Compilation rejects ambiguous writers within a
stage unless the node explicitly defines accumulation.

Inputs are declared with a name, type, default, and optional range/units metadata. Initial
types are `number`, `boolean`, `vec3`, `quat`, `transform`, and enumerated symbol. Names and
types form the runtime contract and are reflected to the editor.

Representative declaration:

```js
const motion = beginMotion(rig);
input("speed", "number", { default: 0, min: 0, max: 8 });
input("grounded", "boolean", { default: true });
input("stride", "number", { default: 1, min: 0, max: 2 });

blend1D("locomotion", "speed", [
  [0.0, idle],
  [2.0, walk],
  [6.0, run],
]);

controller("gait", "proceduralGait", {
  speed: inputRef("speed"),
  stride: inputRef("stride"),
  leftTarget: "leftFootTarget",
  rightTarget: "rightFootTarget",
});

endMotion();
```

Continuous controllers are registered native node types with versioned parameter schemas.
They may adjust pose channels or contribute to named target transforms on every evaluation.
A procedural gait controller can therefore generate foot trajectories continuously and let
the later IK stage solve the legs. JavaScript selects the controller and connects declared
inputs; native C++ performs the frame evaluation.

The initial implementation need only provide the controller infrastructure and the built-in
nodes required by the acceptance example. New native controllers can be added without
changing the artifact model or runtime JavaScript contract.

## Named IK Targets

An IK target is declared once in the motion definition:

```js
ikTarget("leftFootTarget", {
  start: "leftUpperLeg",
  end: "leftFoot",
  positionHalfLife: 0.08,
  rotationHalfLife: 0.10,
  weightHalfLife: 0.06,
});
```

The author names only the first and last joint. The compiler walks parents from `end` to
`start`, verifies that `start` is an ancestor, reverses the result, and stores the compact
chain. Authors do not repeat intermediate joints and do not choose a solver.

For a two-segment chain the first implementation uses the ozz two-bone IK primitive. The
Matter target abstraction is not named `twoBone`, so a later longer-chain solver can support
the same declaration and runtime API. Solver selection is an artifact/compiler decision.

At runtime JavaScript obtains a validated target handle:

```js
const foot = animator.target("leftFootTarget");
foot.transform(worldFootTransform);
foot.enable();

// Later:
foot.disable();
```

Public target transforms are world-space in the first implementation. The evaluator
snapshots the entity world transform and converts the target into rig/model space before
solving. Native controllers receive that same snapshot, so a gait controller can produce a
world-space desired target without coupling the animation asset to the renderer.

Targets expose transform, desired weight, enable, disable, weight, and snap operations.
Enable and disable set desired weight to one or zero. The evaluated weight approaches that
value using the declared half-life. Position and rotation independently approach their
desired transforms using their declared half-lives. `snap()` copies desired state to
evaluated state for teleportation, initialization, and discontinuous edits.

Disabling a target retains its desired transform. Re-enabling it fades toward the current
desired transform without requiring the caller to rebuild the target. An unreachable
target produces the closest stable solution permitted by the solver and constraints; it
must never emit non-finite transforms.

Targets are declared graph inputs and therefore appear in the editor. A future extension
may bind a desired transform to an ECS entity or socket, but direct transform values are the
first runtime contract.

## Runtime Input API

The animator resolves input and target names to generational handles. A name lookup may be
cached by the caller; subsequent writes avoid string lookup. Writes with the wrong type,
unknown name, or stale generation are rejected with a recoverable diagnostic and leave the
previous value unchanged.

Input writes enter a pending buffer. Fixed-tick evaluation snapshots that buffer, providing
a coherent input set for clocks, controllers, markers, and root motion. Frame evaluation
interpolates from fixed state and may use render-only target smoothing, but it never changes
simulation outcomes or emits marker events.

Direct APIs such as `setJointRotation` are intentionally absent from runtime JavaScript.
Exceptional pose behavior is implemented as a declared native controller so its ordering,
inputs, outputs, performance, and serialization remain inspectable.

## Root Motion

Root motion is opt-in per motion definition and names the source joint and projected
channels. Fixed evaluation extracts the delta between the previous and current evaluated
root samples, removes the consumed portion from the visual pose when configured, and writes
a `DesiredRootMotion` component.

The animation system never directly mutates the entity transform. A kinematic, physics,
gameplay, or networking system is the authoritative consumer and may apply, constrain,
partially consume, or reject the request. The component records the requested translation,
rotation, fixed-tick identity, and consumption status so tools can explain discrepancies.

This keeps animation compatible with collision, authoritative simulation, and future
prediction without losing the convenience of generated locomotion clips.

## ECS Integration

Animation is additive to the existing part entity model:

```text
PartInstance + Animator
```

The `PartInstance` hash resolves both `.part` and optional `.anim` siblings. There is no
`AnimatedPart` component or parallel entity hierarchy.

Small reflected public components include:

- `Animator` — asset identity/generation, playback policy, declared-input facade, and
  enabled state;
- `AnimationStatus` — read-only load/evaluation state and latest diagnostic summary;
- `DesiredRootMotion` — fixed-tick request for an authoritative consumer.

Large mutable data remains private in an animation-instance store keyed by a generational
handle: ozz sampling contexts, local/model pose arrays, clocks, node state, smoothed target
state, marker cursors, and renderer snapshot buffers. Flecs components do not contain large
pose arrays or pointers into movable storage.

Scheduling follows the existing Matter phases:

### Fixed update

1. Snapshot pending declared inputs and desired targets.
2. Advance deterministic clip and controller clocks.
3. Detect marker crossings exactly once.
4. Evaluate the root-motion source channels and any simulation-relevant controller state at
   the fixed timestamp.
5. Extract and publish `DesiredRootMotion` from those fixed samples.
6. Enqueue entity-local marker/completion events through Flecs.

### Frame update

1. Interpolate fixed clocks and input state.
2. Sample and blend base clips.
3. Apply additive layers.
4. Evaluate continuous native controllers.
5. Smooth and solve IK targets.
6. Apply constraints and run local-to-model conversion.
7. Publish an immutable pose snapshot to the renderer bridge.

ECS mutation remains on the session's main thread. The first implementation may evaluate
poses on that thread. Later job parallelism is permitted if workers consume immutable
snapshots and publish results only at the phase boundary.

## Event-System Relationship

The animation compiler, ozz adapter, `.anim` cache, runtime evaluator, ECS components, and
renderer bridge do not depend on the pending editor event-system architecture. They can be
implemented and tested on the current branch structure.

Event ownership is divided by scope:

- Flecs events carry entity-local runtime facts such as clip markers and completion.
- The session event hub carries asynchronous load, bake, and runtime failures.
- The application/editor hub carries commands, selection, rebuild requests, and inspector
  synchronization.

Only the latter two hub bindings wait for the event branch to land. Until then, core code
returns structured diagnostics through direct result/status APIs. The later integration
adapts those results to hubs rather than changing the compiler or runtime contracts.

## Renderer Bridge

The renderer consumes an immutable animation snapshot containing asset generation, instance
identity, joint model matrices, bounds, rigid records, attachment records, and skinned
palette location. It does not query mutable animator state during command construction.

### Rigid path

Rigid segments and attachments compose their bind offsets with current joint matrices. The
underlying geometry and BLAS remain unchanged. Raster instance transforms and TLAS instance
transforms are updated. This is the first rendering milestone because it exercises the full
rig/runtime path without requiring vertex deformation.

### Skinned raster path

The renderer uploads the joint palette and dispatches compute skinning once per required
pose/output buffer. Raster passes reuse the deformed buffer. Animated bounds come from
compiled per-joint bounds transformed by the current palette, with conservative fallback
bounds while data is unavailable.

### Ray-tracing path

Rigid segments remain exact through TLAS transform updates. For deforming geometry the
renderer chooses among:

- an exact deformed BLAS update/refit for selected high-value instances within budget;
- a generated rigid or coarse animated proxy;
- the normal static bind-pose proxy as a conservative fallback.

Exact deformation is a quality tier, not a semantic requirement of every animated entity.
The budget decision belongs to the renderer and does not alter animation evaluation.

### Failure degradation

If a skinned GPU allocation or exact ray-tracing update cannot be satisfied, the instance
falls back to its compiled proxy or bind pose and records a diagnostic. Simulation, targets,
markers, and root motion continue.

## Part Workbench and Editor

Animation tools live as tabs in the Part Workbench because the part JavaScript module is the
authoritative definition. The initial tool surface is observational and interactive, not a
second authoring format:

- **Rig** — hierarchy, selected joint, bind/evaluated transforms, and sockets;
- **Skin** — influence display, binding mode, generated bounds, and weight diagnostics;
- **Clips** — playback, scrub, loop, speed, markers, and local/model pose inspection;
- **Graph** — active base clips, blend weights, controller stages, and declared inputs;
- **Targets** — target transforms, weights, smoothing state, chain, and gizmos;
- **Render** — chosen geometry and ray-tracing tiers, buffer sizes, and update cost.

The existing debug-rendering path in the isolated part preview draws bones, joint axes,
radius envelopes, sockets, target transforms, IK chains, and optional skin weights. These
are transient overlays derived from `.anim`; they are never emitted into `.part` geometry or
included in production bounds.

Runtime entity properties expose a smaller reflected subset: animator enabled state,
declared inputs, targets, current clip/state, status, and root-motion request.

Live rebuild is transactional. The editor continues using the previous valid `.anim` until
a replacement has been fully compiled, validated, loaded, and generation-swapped. Inputs
and targets migrate only when name and type match. Changed or removed declarations reset to
new defaults, and stale cached handles fail generation checks.

## Validation and Failure Behavior

Compilation is fail-closed. A candidate `.anim` is built in temporary storage or memory,
validated and checksummed, then atomically published. Failure preserves the previous valid
artifact.

Validation covers at least:

- nested, overlapping, or unclosed DSL sessions;
- unbalanced rig branch stacks;
- missing, duplicate, or invalid joint names;
- more than one root, cycles, invalid parentage, and non-finite transforms;
- invalid radii, degenerate segments, and deterministic mirror-name collisions;
- invalid geometry selections, duplicate primary bindings, or unresolved attachments;
- invalid clip duration, rate, marker, track, loop, or additive metadata;
- generated callbacks producing missing/non-finite pose values or attempting structural
  session mutation while samples are being captured;
- duplicate input names, unsupported types, invalid defaults, and ambiguous graph writers;
- target start/end ancestry failures, degenerate chains, and invalid smoothing values;
- missing controller types or incompatible controller schema versions;
- skin vertices with no valid influence, excess influences after packing, or weights outside
  normalization tolerance;
- ozz builder, optimizer, serialization, and runtime-payload validation failures.

Diagnostics carry module identity, resolved hash, stage, severity, source location when
available, and relevant rig/joint/clip/input/target names. Native exceptions do not cross
QuickJS, Flecs, worker, or Vulkan boundaries.

At runtime, a missing or invalid `.anim` sets `AnimationStatus` to a recoverable failed state
while leaving the entity alive. A static bind-pose `.part` may remain visible. Invalid input
writes preserve the previous value. Unreachable IK targets clamp to stable finite results.
Corrupt payloads are rejected before creating runtime ozz objects.

## Testing Strategy

### Headless compiler and adapter tests

- Deterministic rig hierarchy, branch stack, mirroring, sockets, and ancestry.
- Matter-to-ozz skeleton construction with asymmetric transform fixtures.
- Generated and explicit clip sampling, optimization, looping, and additive behavior.
- Marker ordering and boundary crossing, including loops and large fixed steps.
- `.anim` deterministic byte output, round trip, version rejection, and hash invalidation.
- Transactional failure preserving the previous artifact.

### Geometry-binding tests

- Radius interpolation and segment-envelope construction.
- Deterministic influence fields and stable tie-breaking.
- At most four packed influences and normalized weights within tolerance.
- Bind-pose skinning reproducing source positions and normals.
- Simplification/LOD weight reprojection.
- Rigid segment ownership and attachment transform composition.
- Hybrid selection conflict validation.

### Runtime and ECS tests

- Base, additive, and staged-controller ordering.
- Declared input type checking, snapshotting, and generational handle invalidation.
- Frame-rate-independent half-life smoothing.
- Target enable/disable fades, snap, chain inference, and unreachable solutions.
- Fixed-tick marker determinism under varied render frame rates.
- Root-motion extraction without direct entity-transform mutation.
- Asset sharing with independent per-instance clocks and targets.
- Live-reload migration by matching name and type.

### Renderer tests

- Rigid animation changes instance/TLAS transforms without changing BLAS geometry.
- GPU skinning output compared with a CPU reference pose.
- Conservative animated bounds over representative clips and controller extremes.
- Shared compute-skinned buffers across raster passes.
- Proxy selection and exact BLAS update/refit budget behavior.
- Clean degradation after simulated allocation/update failure.

### Integration and manual acceptance

An `AnimatedRigGallery` JavaScript part is the release acceptance asset. It contains:

- a soft voxel creature generated with `skin(rig)`;
- a segmented machine generated with `segments(rig)`;
- at least one independently instanced joint attachment;
- generated idle, walk, and run clips blended by speed;
- one continuously evaluated native gait/controller path;
- foot and look targets with enable/disable smoothing;
- opt-in desired root motion consumed by a test authority system;
- Part Workbench skeleton, envelope, target, chain, and weight overlays;
- a successful transactional live edit and an intentionally failed edit that retains the
  previous valid animation.

The acceptance asset is authored entirely in JavaScript and remains functional after both
`.part` and `.anim` caches are deleted and rebuilt.

## Delivery Sequence

### Phase A — event-independent foundation

1. Vendor and pin ozz-animation with license/version metadata.
2. Build the narrow ozz adapter and headless asymmetric-pose smoke tests.
3. Add Matter rig/clip/graph intermediate representations and validators.
4. Add the stateful rig and generated-clip DSL.
5. Compile and transactionally cache `.anim` siblings.
6. Add skin influence, rigid-segment, and attachment binding products.

### Phase B — runtime and rendering

1. Add shared asset storage, private instance storage, and `PartInstance + Animator` ECS
   integration.
2. Add fixed/frame staged evaluation, declared input handles, targets, markers, and desired
   root motion.
3. Deliver rigid raster/TLAS animation as the first visible end-to-end renderer path.
4. Add compute-skinned raster deformation and conservative animated bounds.
5. Add deforming ray-tracing proxies and budgeted exact BLAS updates/refits.

### Phase C — event and editor integration

1. Adapt structured compiler/runtime diagnostics to the session event hub.
2. Connect editor commands and inspector synchronization to the application hub.
3. Add Part Workbench animation tabs and debug overlays.
4. Add transactional live-reload presentation and migration diagnostics.

Phase C begins after the editor event-system architecture lands. Phases A and most of B can
proceed in parallel with that branch because they communicate through stable result,
snapshot, and component boundaries rather than editor events.

## Implementation Decomposition

This document is the architectural contract. Detailed implementation plans should be split
into bounded projects so each can be reviewed and verified independently:

1. **Ozz foundation and `.anim` container**
2. **Stateful rig, clip, and motion DSL/compiler**
3. **Skin, rigid-segment, and attachment binding**
4. **Runtime graph, targets, ECS, markers, and root motion**
5. **Raster and ray-tracing renderer bridge**
6. **Part Workbench and event-system integration**

The first implementation plan should cover only item 1. Later plans must preserve the
interfaces and invariants established here while refining private representations.

## Deferred Extensions

- Longer-chain, spline, full-body, and multi-effector IK solvers.
- ECS entity/socket following for target desired transforms.
- Motion matching or large generated clip databases.
- Retargeting between independently authored rigs.
- Network synchronization policies.
- Visual authoring and non-JavaScript persistence.
- Imported animation data.

These extensions fit behind the named-target, native-controller, asset, and snapshot
boundaries. None is required to validate the procedural-first core.
