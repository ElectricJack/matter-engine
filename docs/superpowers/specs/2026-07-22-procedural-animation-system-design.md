# Procedural Animation System — Design

**Date:** 2026-07-22
**Status:** Revised after adversarial codebase review; awaiting final sign-off
**Primary runtime dependency:** ozz-animation, wrapped behind MatterEngine APIs

## Adversarial Review Resolution (2026-07-22)

The final pass checked this design against the current provider/cache, ECS, script-host,
physics, and Vulkan renderer implementations. The review confirmed the core boundaries and
the low-risk rigid-segment milestone, but found several contracts that were aspirational or
ambiguous. This revision resolves them as follows:

- the primary runtime control surface is C++; a gameplay-JavaScript binding is a later
  adapter and is not a prerequisite for the animation runtime;
- fixed-to-frame interpolation, controller world queries, skinning buffers, and animated
  bounds are named greenfield work rather than assumed engine facilities;
- resolved part hashes retain their current source/params/children identity, while artifact
  compatibility epochs invalidate derived animation bundles independently;
- `.part` and `.anim` are consumed through a checksummed commit manifest, preventing torn
  sibling pairs and stale vertex-order coupling;
- target ownership, fixed-versus-frame cadence, root-motion ordering, ozz IK conversion,
  mirror semantics, and generated-clip sampling are explicit below;
- v1 targets the production Vulkan renderer, uses one existing dynamic instance per rigid
  segment, and deliberately defers deforming BLAS updates and new RT proxy infrastructure;
- physics pose-following, persistent save/load, and nested animated attachments are
  explicitly scoped, and hard/configurable resource budgets are recorded.

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
pipeline. Runtime C++ systems and editor tools may update only inputs and targets declared
when the `.anim` artifact was built. A future gameplay-JavaScript host may expose the same
handles, but it will not write joints directly or execute callbacks inside pose evaluation.

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
- Let runtime systems drive declared semantic inputs and targets through a stable C++ API;
  permit a later gameplay-JavaScript host to bind that same API.
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
- Building the gameplay-JavaScript host. This design supplies the C++ API that host can
  expose later.
- Pose-following physics shapes, skinned-surface collision, and articulated rigid-body
  dynamics in v1; root/entity collision remains authoritative.
- Nested animated `.part` attachments in v1; attachments must resolve to static parts.
- Exact ray tracing of deforming skinned vertices in v1. Rigid animation remains exact.
- General persistent world save/load in v1. Editor Play/Stop checkpoint behavior is in
  scope because it already snapshots runtime entities.
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
| Primary control API | C++ generational handles; gameplay-JS binding is a later adapter |
| Procedural evaluation | Native compiled controllers; no per-frame JavaScript callbacks |
| Graph form | Ordered stages, not an unrestricted user node graph |
| IK selection | Public API names a start and end joint; engine derives the chain and solver |
| Target toggling | Enable/disable changes desired weight with configurable smoothing |
| Target ownership | Transform driver is exactly one external writer or one controller |
| Runtime cadence | Every input, target, and controller is declared `fixed` or `frame` |
| Geometry modes | Skinned, rigid segments, attached parts, or any hybrid of them |
| ECS model | `PartInstance + Animator`; no separate `AnimatedPart` entity type |
| Root motion | Emit desired root motion for an authoritative consumer to apply or reject |
| Resolved hash | Preserve source + canonical params + sorted child hashes |
| Artifact invalidation | Header/manifest epochs, not ozz pins in every resolved part hash |
| Animated publication | Validate siblings, then commit them through one atomic manifest |
| Renderer | Production Vulkan only; legacy renderer displays bind pose |
| Deforming ray tracing | Bind-pose fallback in v1; exact BLAS updates require a later design |
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

The integration is split into seven independently testable units:

1. **Ozz adapter** converts Matter rig and clip build products into ozz offline/runtime
   objects and exposes Matter-native sampling, blending, local-to-model, and IK operations.
2. **Animation compiler** executes the DSL at bake time, validates it, generates geometry
   bindings and clips, and writes `.anim`.
3. **Animation runtime** evaluates immutable assets and mutable instances without renderer
   or editor knowledge.
4. **ECS integration** schedules fixed and frame evaluation, publishes status, markers, and
   desired root motion, and owns instance lifetime.
5. **Renderer bridge** consumes immutable pose snapshots and chooses rigid, skinned, or
   bind-pose ray-tracing paths in the production Vulkan renderer.
6. **Editor integration** observes artifacts and runtime state, renders debug overlays, and
   reports commands and diagnostics through the editor event architecture.
7. **Optional runtime-language bindings** adapt C++ input/target handles to a future
   gameplay-scripting host. They consume the runtime and do not participate in evaluation.

Dependencies flow in that order. In particular, the animation runtime does not call the
renderer, the editor does not own runtime state, and the ozz adapter has no JavaScript or
Flecs dependency. ECS integration may inject the narrow world-query interface described
below; the core runtime never calls Box3D or Flecs directly.

## Source and Artifact Model

For a resolved part hash `<hash>`, the cache may contain:

```text
parts/<hash>.part          # bind-pose geometry and normal part metadata
parts/<hash>.anim          # rig, motion, bindings, and animation renderer metadata
parts/<hash>.anim.commit   # checksummed commit record for the animated sibling pair
```

Static parts produce only `.part`. Animated parts normally produce both. Both files are
derived from the same resolved JavaScript module and dependency graph; neither is canonical
content. A missing or mismatched sibling invalidates the animated bundle and rebuilds both
payloads. A retained `.part` may still serve a static/bind-pose preview, but it is never
paired with stale `.anim` vertex streams.

The resolved part hash deliberately keeps the current engine contract: transitively folded
module source, canonical evaluated parameters, and sorted child hashes. It does not add ozz,
animation-schema, compiler, or engine versions. This avoids invalidating every static part
when the animation runtime changes and avoids making hash identity depend on discovering an
animation session during evaluation.

Derived compatibility is enforced separately. The `.anim` header and commit record contain
an animation schema version, animation bake epoch, pinned ozz version, target ABI tag, and
adapter serialization version. An incompatible value is a cache miss that rebuilds the
animated bundle at the same resolved hash. Changes to generated `.part` geometry continue
to use the part artifact's existing format/bake-version invalidation mechanisms.

Publication is coherent across siblings even though a filesystem cannot atomically rename
two files. The compiler writes temporary `.part` and `.anim` payloads, loads and validates
both completely, replaces the payload files, and atomically replaces `.anim.commit` last.
The commit record contains both payload checksums, vertex-layout signature, per-LOD vertex
counts, schema/epoch values, and one build nonce. Loaders read the commit record first and
accept the pair only when both payloads match it. A concurrent or interrupted publish can
therefore cause a retry/cache miss, but cannot expose a mixed pair. Live reload retains the
previous in-memory pair until the new committed pair loads successfully.

The `.anim` file begins with a Matter-owned versioned header and section table. Sections
contain the Matter joint table and names, compiled ozz skeleton and clip payloads, graph
program, input and target schemas, sockets, skin influence streams, rigid-segment records,
attachment records, animated bounds metadata, and optional debug metadata. Consumers must
not infer the file layout from ozz serialization internals. Skin influence streams identify
the committed `.part` body checksum, vertex-layout signature, and LOD vertex counts they
index; vertex order is never assumed solely from the resolved hash.

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

Generated clip callbacks inherit the existing deterministic bake sandbox: no `Date`, no
ambient I/O, and params-seeded `Math.random`. Native geometry and ozz offline processing are
required to reproduce bytes only for the same supported toolchain, target ABI, dependency
versions, and bake settings. Cross-toolchain byte identity is not promised; target ABI and
compatibility tags prevent sharing an uncertified native payload between such caches.

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
  bone("neck", [0, 0.16, 0]);
  radius(0.13);
  bone("head", [0, 0.18, 0]);
pop();

push();
  radius(0.10);
  bone("leftUpperArm", [0.34, 0.05, 0]);
  radius(0.085);
  bone("leftLowerArm", [0.26, -0.02, 0]);
  radius(0.075);
  bone("leftHand", [0.12, -0.01, 0]);
pop();

mirrorBranch("leftUpperArm", "rightUpperArm", {
  axis: "x",
  rename: { from: "left", to: "right" },
});

atJoint("hips");
push();
  radius(0.14);
  bone("leftUpperLeg", [0.16, -0.44, 0]);
  radius(0.11);
  bone("leftLowerLeg", [0, -0.34, 0.02]);
  radius(0.09);
  bone("leftFoot", [0, -0.12, 0.18]);
pop();

mirrorBranch("leftUpperLeg", "rightUpperLeg", {
  axis: "x",
  rename: { from: "left", to: "right" },
});
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
| `mirrorBranch(fromRoot, toRoot, options)` | Reproduces one named subtree using explicit axis and rename rules |
| `socket(name, transform?)` | Adds a named transform relative to the current joint |
| `endRig()` | Validates and closes the rig session |

Rig-session `push()` and `pop()` operate on the rig branch stack; geometry transform-stack
verbs retain their existing behavior outside a rig session. Sessions may not overlap, and
build finalization rejects an open session or unbalanced branch stack.

`mirrorBranch` requires an exact source root, exact target root, axis, and rename token pair.
The source root maps to `toRoot`; every descendant name must contain the `rename.from` token
exactly once and maps by replacing it with `rename.to`. Missing or repeated tokens and name
collisions are compile errors. An explicit per-name map may replace the token rule for
irregular naming.

For reflection matrix `S` on the selected axis, each source local translation maps to
`S * t` and each local rotation matrix maps to `S * R * S`; the result is converted back to
a normalized, canonical-sign quaternion. Radius and positive scale magnitudes are copied.
Sockets already present in the subtree are mirrored and renamed by the same rule. Geometry,
bindings, clips, and attachments declared later are not copied; they may address the emitted
target joints or use their own mirror helpers. Because `mirrorBranch` mirrors rig data before
geometry generation rather than reflecting finished triangles, generated winding does not
flip.

Joint names are unique UTF-8 identifiers within a rig and are the stable authoring keys for
clips, targets, sockets, debug tools, and live-reload migration. Runtime evaluation uses
compact indices resolved during compilation.

## Geometry From the Rig

The compiler iterates parent-child segments and can produce three binding classes.

### Automatic skinned geometry

`skin(rig, options?)` emits a continuous voxel envelope along all selected, unclaimed
segments. It lowers tapered bone capsules plus joint spheres into one unioned implicit field
and sends that field through MatterEngine's voxel/SurfaceLib meshing path. Branch junctions
are therefore a single unioned surface rather than stitched capsule meshes. `voxelSize` is
an explicit option; if omitted it inherits the part's active voxel bake resolution. Authors
can use normal field modifiers or custom binding geometry when the automatic junction shape
is insufficient.

The bake also creates a compact influence field. After modifiers, simplification, and every
LOD mesh have finalized their own vertex streams, the compiler samples that field at each
final vertex, retains the strongest influences, and normalizes weights. The existing
`lod_bake` simplifier therefore does not need to preserve new attributes or vertex order.

The default maximum is four joint influences per vertex. Influence computation is
deterministic and operates in bind-pose space. Skinning the bind pose must reproduce each
final LOD mesh within the documented numerical tolerance.

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
transform with the evaluated joint transform. In v1 the resolved child must not contain an
animation bundle; an animated attachment is a compile error rather than an implicitly nested
animator.

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

Sampling is exact. Let `segments = ceil(duration * sampleRate)`, with a minimum of one. A
non-looping generator is invoked at `i / segments` for every `i` from zero through
`segments`. A looping generator is invoked for zero through `segments - 1`; the compiler
appends a sample at `duration` copied exactly from sample zero, guaranteeing loop closure.

Every invocation begins from a fresh bind pose. `at()` only selects a joint. Rotation verbs
build a local-axis delta in call order and apply `localRotation = bindRotation * delta`;
translation verbs are offsets from bind translation. A joint untouched during an invocation
is captured at bind pose for that sample, even if other phases touch it. Constant bind tracks
may be removed only by the deterministic optimizer. No pose state accumulates between
callback invocations.

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
declared fixed/frame inputs
    -> base clip selection and blending
    -> additive layers
    -> native controllers at their declared cadence
    -> preliminary local-to-model conversion
    -> IK target solve
       [model-space query -> local correction -> affected-subtree reconversion]
    -> joint constraints
    -> final local pose and model palette
    -> pose snapshot
```

This order makes pose ownership predictable, prevents cycles, and gives MatterEngine room
to optimize storage and evaluation. Each node declares the inputs and joints it reads and
the pose channels or targets it writes. Compilation rejects ambiguous writers within a
stage unless the node explicitly defines accumulation.

Inputs are declared with a name, type, cadence, default, and optional range/units metadata.
Initial types are `number`, `boolean`, `vec3`, `quat`, `transform`, and enumerated symbol.
Cadence is exactly `fixed` or `frame`; it is never inferred. Fixed inputs may affect clocks,
root motion, fixed controllers, and markers. Frame inputs are cosmetic and compilation
rejects any path from them to fixed state. Names, types, and cadence form the runtime
contract and are reflected to the editor.

Representative declaration:

```js
const motion = beginMotion(rig);
input("speed", "number", { cadence: "fixed", default: 0, min: 0, max: 8 });
input("grounded", "boolean", { cadence: "fixed", default: true });
input("stride", "number", { cadence: "fixed", default: 1, min: 0, max: 2 });

blend1D("locomotion", "speed", [
  [0.0, idle],
  [2.0, walk],
  [6.0, run],
]);

controller("gait", "proceduralGait", {
  cadence: "fixed",
  speed: inputRef("speed"),
  stride: inputRef("stride"),
  leftTarget: "leftFootTarget",
  rightTarget: "rightFootTarget",
});

// Target declarations shown below complete this motion session.
```

Continuous controllers are registered native node types with versioned parameter schemas.
Each declaration has exactly one cadence. Fixed controllers execute once per fixed tick and
publish previous/current state that frame evaluation interpolates; they are never executed a
second time at frame rate. Frame controllers execute once in `FrameUpdate`, are cosmetic,
and cannot write root motion, markers, or fixed targets.

Controllers may adjust pose channels or drive named target transforms. A procedural gait
controller generates foot trajectories at fixed cadence and lets the later IK stage solve
the legs. The DSL selects the native controller and connects declared inputs; native C++
performs evaluation.

### Controller world queries

The ECS integration injects an `AnimationWorldQueries` interface into fixed controller
evaluation. Its v1 operation is a masked segment/ray cast returning hit position, normal,
fraction, and entity identity. The default world-session implementation delegates to the
existing Box3D `physics_ray_cast`; headless tests provide a deterministic fake. The
animation runtime and ozz adapter do not depend on Box3D, Flecs, streaming, or the renderer.

World queries run only for fixed controllers in `FixedPostUpdate`, after root-motion
consumption, physics, transform pull, and hierarchy propagation for that tick. Results are
stored in the controller's current fixed state and interpolated for rendering; frame
controllers cannot issue world queries. A missing provider or miss returns an explicit
no-hit result, after which the gait controller uses its authored airborne trajectory rather
than stale ground data. Determinism is relative to the fixed world snapshot presented by
the authoritative session. Cross-machine/network determinism is deferred to the future
network authority design.

The initial implementation need only provide the controller infrastructure and the built-in
nodes required by the acceptance example. New native controllers can be added without
changing the artifact model or runtime C++ contract. Controller and target name references
are resolved together at `endMotion()`, so declarations may use forward references.

## Named IK Targets

An IK target is declared once in the motion definition:

```js
ikTarget("leftFootTarget", {
  start: "leftUpperLeg",
  end: "leftFoot",
  driver: { controller: "gait" },
  cadence: "fixed",
  positionHalfLife: 0.08,
  rotationHalfLife: 0.10,
  weightHalfLife: 0.06,
});

ikTarget("rightFootTarget", {
  start: "rightUpperLeg",
  end: "rightFoot",
  driver: { controller: "gait" },
  cadence: "fixed",
  positionHalfLife: 0.08,
  rotationHalfLife: 0.10,
  weightHalfLife: 0.06,
});

ikTarget("leftHandTarget", {
  start: "leftUpperArm",
  end: "leftHand",
  driver: "external",
  cadence: "frame",
  positionHalfLife: 0.05,
  rotationHalfLife: 0.05,
  weightHalfLife: 0.05,
});

ikTarget("lookTarget", {
  start: "chest",
  end: "head",
  driver: "external",
  cadence: "frame",
  positionHalfLife: 0.04,
  rotationHalfLife: 0.04,
  weightHalfLife: 0.05,
});

endMotion();
```

The author names only the first and last joint. The compiler walks parents from `end` to
`start`, verifies that `start` is an ancestor, reverses the result, and stores the compact
chain. Authors do not repeat intermediate joints and do not choose a solver.

For a two-segment chain the first implementation uses the ozz two-bone IK primitive. The
Matter target abstraction is not named `twoBone`, so a later longer-chain solver can support
the same declaration and runtime API. Solver selection is an artifact/compiler decision.

An externally driven target instead declares `driver: "external"` and either fixed or frame
cadence. The primary runtime C++ API obtains a validated target handle:

```cpp
AnimationTargetHandle look = animations.target(entity, "lookTarget");
animations.set_transform(look, world_look_transform);
animations.enable(look);

// Later:
animations.disable(look);
```

The future gameplay-JavaScript host may bind the same operations as
`animator.target(name).transform(...).enable()`, preserving the previously selected DSL
shape without making that host a prerequisite.

Each target has exactly one transform driver: external or one named controller. An external
transform write to a controller-driven target is rejected; a controller connection to an
externally driven target is a compile error. Runtime systems retain control of desired
enable/weight for either kind, allowing gameplay to fade a controller-owned foot target
without competing for its transform.

Public target transforms are world-space in v1. Fixed controller queries and targets are
computed after physics against the post-root-motion world transform from the same fixed
tick. Frame targets are consumed after all completed fixed ticks. Frame pose evaluation
always converts desired world transforms using the entity's current post-physics
`WorldTransform`, not the transform captured when the write occurred.

The ozz two-bone implementation first converts the working local pose to model space, asks
the solver for local correction quaternions, patches those locals, and re-runs
local-to-model for the affected subtree before the next ordered target. Constraints run
after all target reconversions and the final palette is rebuilt if they modify locals. The
compiler establishes deterministic target order and, in v1, rejects declared target chains
with overlapping writable joint sets. A future multi-effector solver may explicitly own and
solve an overlapping set as one node.

Targets expose transform, desired weight, enable, disable, weight, and snap operations.
Enable and disable set desired weight to one or zero. The evaluated weight approaches that
value using the declared half-life. Position and rotation independently approach their
desired transforms using their declared half-lives. Fixed targets smooth once per fixed tick
and publish previous/current evaluated state; frame evaluation interpolates those states.
Frame targets smooth once per rendered frame and cannot affect fixed state. `snap()` copies
desired state to evaluated state at the target's next cadence boundary for teleportation,
initialization, and discontinuous edits.

All half-life smoothing uses `alpha = 1 - exp2(-dt / halfLife)`. A zero half-life snaps.
Position and scale use linear interpolation; rotation uses normalized shortest-arc slerp;
weight is clamped to `[0, 1]` and interpolated linearly with its alpha. `weight(x)` changes
the desired weight and follows the same smoothing rule.

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

This is a C++ service API owned by MatterEngine. Its handle shape follows the existing
index-plus-generation precedent in `DynamicInstanceSlots`, although animation storage has
its own lifetime and does not reuse renderer slots. Runtime JavaScript is not present in the
current engine and is not assumed here. A later gameplay host may wrap these C++ calls.

Fixed-cadence writes enter a pending buffer and are snapshotted at `FixedPreUpdate`. The
instance store retains previous/current fixed clocks, inputs, controller outputs, and target
state. `Runtime::tick` must expose the accumulator-derived interpolation alpha to the frame
pipeline; this previous/current storage and alpha plumbing are explicit greenfield tasks.
Frame evaluation resamples clips at the interpolated clock and interpolates fixed controller
and target outputs. It does not re-run fixed controllers.

Fixed number/vector values interpolate linearly, quaternions use normalized shortest-arc
slerp, transforms use linear translation/scale plus quaternion slerp, and booleans/enums step
to the current fixed value at alpha one. Every native fixed-controller output field declares
one of these interpolation modes in its versioned schema.

Frame-cadence writes use a separate pending buffer consumed once at the beginning of
`FrameUpdate`. They may drive cosmetic controllers and targets only. They do not alter
clocks, fixed controller state, markers, root motion, physics queries, or simulation. Thus a
frame look target is responsive at render cadence while a grounded foot target remains a
fixed, query-backed result.

Direct APIs such as `setJointRotation` are intentionally absent from C++ and from the future
JavaScript binding. Exceptional pose behavior is a declared native controller so its
ordering, cadence, inputs, outputs, performance, and serialization remain inspectable.

## Root Motion

Root motion is opt-in per motion definition and names the source joint and projected
channels. Fixed evaluation extracts the delta between the previous and current evaluated
root samples, writes a `DesiredRootMotion` component, and removes the full requested delta
from the visual pose to keep the rig in place. If root motion is not declared, the root track
remains visual and no request is emitted. Consumer acceptance or rejection does not
retroactively alter the in-place pose policy.

In v1 root motion is extracted from sampled base/additive clip tracks only. Controllers may
drive joints and targets but may not write root motion. This keeps extraction available in
`FixedUpdate` before physics without evaluating the full pose or issuing world queries.

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

The `PartInstance` hash resolves `.part` plus an optional committed `.anim` sibling bundle.
There is no `AnimatedPart` component or parallel entity hierarchy.

Small reflected public components include:

- `Animator` — asset identity/generation, playback policy, declared-input facade, and
  enabled state;
- `AnimationStatus` — read-only load/evaluation state and latest diagnostic summary;
- `DesiredRootMotion` — fixed-tick request for an authoritative consumer.

Large mutable data remains private in an animation-instance store keyed by a generational
handle: ozz sampling contexts, local/model pose arrays, clocks, node state, smoothed target
state, marker cursors, and renderer snapshot buffers. Flecs components do not contain large
pose arrays or pointers into movable storage.

Editor Play/Stop integrates with the existing `SimulationControl` snapshot. An
`AnimatorCheckpoint` captures asset generation, graph state, clocks, fixed/frame input
values, target desired/evaluated state, controller serializable state, and marker cursors.
Transient ozz contexts, pose arrays, GPU allocations, and cached handles are reconstructed.
Stopping restores the checkpoint taken at Play rather than leaving animation advanced.
Durable world save/load may reuse this record later but is outside v1.

Scheduling uses the existing Matter phases but adds new animation systems and interpolation
state. Required ordering is part of the contract:

### Fixed update

1. In `FixedPreUpdate`, move current fixed animation state to previous, snapshot fixed input
   writes, and advance deterministic clip/controller clocks.
2. In `FixedUpdate`, sample only the clip channels needed for root motion, extract and
   publish `DesiredRootMotion`, detect marker crossings, and enqueue marker/completion Flecs
   events exactly once.
3. In `PrePhysics`, the authoritative movement system consumes or rejects root motion and
   updates the entity/kinematic body.
4. Physics steps, pulls transforms, and completes hierarchy propagation through
   `PostPhysics`.
5. In `FixedPostUpdate`, fixed controllers run once. Query-backed controllers see the
   post-root-motion, post-physics transform and publish current target/controller state.

### Frame update

1. Consume frame-cadence writes and obtain the new runtime interpolation alpha.
2. Interpolate fixed clocks/inputs and sample/blend base clips at that render timestamp.
3. Apply additive layers and interpolated outputs from fixed controllers.
4. Execute frame controllers exactly once and smooth targets at their declared cadence.
5. Convert world targets using the current post-physics `WorldTransform`, build the
   preliminary model pose, and solve ordered IK targets with subtree reconversion.
6. Apply constraints, produce the final palette, and publish an immutable pose snapshot to
   the renderer bridge.

Fixed controllers are never re-executed in `FrameUpdate`, and frame controllers never run in
the fixed pipeline. Marker events represent simulation time and may lead the interpolated
visual pose by at most one fixed step; audio/VFX consumers that require visual alignment may
delay presentation using the tick identity included in the marker event.

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

The animation renderer bridge targets the production Vulkan path (`VkSceneRenderer`) only.
The legacy raylib/GL renderer may load and display the static bind-pose `.part`, but it does
not evaluate animated transforms or skinning and is not an acceptance target.

The Vulkan renderer consumes an immutable animation snapshot containing asset generation,
instance identity, joint model matrices, bounds, rigid records, attachment records, and
skinned palette location. It does not query mutable animator state during command
construction.

### Rigid path

Rigid segments and attachments compose their bind offsets with current joint matrices. The
underlying geometry and BLAS remain unchanged. Raster instance transforms and TLAS instance
transforms are updated. This is the first rendering milestone because it exercises the full
rig/runtime path without requiring vertex deformation.

Each rigid segment and each attachment consumes one existing `DynamicInstanceSlots` slot.
There is no hidden batched sub-instance type in v1. The current Vulkan dynamic lane already
supports per-frame raster and TLAS transform updates, and culling reprojects each static
AABB through the live transform. Compiler statistics expose the resulting instance count,
and runtime admission remains subject to the renderer's global dynamic-slot capacity.

### Skinned raster path

Compute skinning, joint-palette storage, deformed-vertex arenas, and animated cluster bounds
are greenfield Vulkan work. The bridge builds a compact visible-work queue. Each work item
references an immutable source vertex range, influence range, palette range, and a slice of
a per-frame-in-flight deformed-vertex arena. The renderer uploads palettes in bulk and uses
batched work dispatches; it does not allocate a permanent full vertex buffer or issue a
mandatory standalone dispatch for every animator.

Raster passes reuse the resulting deformed slices. Each `.anim` contains conservative
per-joint/per-cluster bind bounds; the compute or CPU preparation path transforms and unions
them into the dynamic cull record. This requires an animated-bounds extension beside the
current static `GpuCluster` AABB rather than pretending the static bounds already deform.

### Ray-tracing path

Rigid segments remain exact through TLAS transform updates. For deforming geometry the
renderer uses the existing immutable bind-pose `.part` BLAS under the animated entity's root
transform. It is a conservative visual fallback, not a new proxy system and not exact to the
deformed surface. V1 does not add `ALLOW_UPDATE`, `MODE_UPDATE`, deforming BLAS allocation,
or per-frame BLAS build scheduling. Exact skinned ray tracing and generated animation
proxies require their own later design and performance evidence.

### Failure degradation

If a skinned GPU allocation or work budget cannot be satisfied, the instance renders its
last completed deformed slice when safe, otherwise its bind pose, and records a diagnostic.
Ray tracing already uses the bind-pose fallback. Simulation, targets, markers, and root
motion continue.

## Resource Limits and Runtime Budgets

V1 hard artifact limits are 256 joints per rig, four influences per skinned vertex, 64 IK
targets, and 128 compiled graph/controller nodes. A rigid-only rig may emit at most 255 bone
segment instances plus declared attachments, still subject to the existing global dynamic
slot capacity. Compilation fails with a diagnostic instead of silently truncating these
counts.

Default configurable runtime budgets are 4,096 animator instances, 64 MiB of mutable CPU
animation state, 2,048 controller world queries per fixed tick, 65,536 evaluated joints per
frame, 256 visible skinned work items, and 2,000,000 skinned output vertices per frame.
World-query overflow produces a no-hit result for excess queries selected deterministically
by declared priority and then stable animator handle, and increments a diagnostic counter.
Exact deforming BLAS updates have a budget of zero in v1. Budget counters and overflows are
visible in the Render tab.

Simulation-relevant fixed clocks, markers, and root motion are never skipped. Cosmetic pose
evaluation may run at 60, 30, or 15 Hz, then freeze the last pose when it falls outside the
budget; returning to a higher tier resamples current graph time rather than fast-forwarding
every missed pose. At frame start the animation budget manager consumes the Vulkan bridge's
previous completed visibility/priority snapshot; the renderer never calls mutable animation
state while building commands. Pose-tier selection may not alter fixed controller results.
The acceptance asset stays below 128 joints, 50,000 LOD0 skinned
vertices, 32 rigid segment/attachment instances, eight targets, and 32 graph nodes so it
tests representative behavior without consuming the hard maxima.

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
only migrate when name, type, and cadence match. Targets additionally require matching
driver kind and joint chain. Changed or removed declarations reset to new defaults, and
stale cached handles fail generation checks.

## Validation and Failure Behavior

Compilation is fail-closed. Candidate `.part` and `.anim` payloads are built in temporary
storage, loaded through the same readers used at runtime, cross-validated, and checksummed
before replacement. The `.anim.commit` record is atomically replaced last. Failure before
commit preserves the previous valid in-memory pair; a fresh loader ignores any uncommitted
or mismatched files and requests a rebuild. Pre-publish pair validation and the commit record
are new work built on the existing single-file tmp/rename and checksum mechanisms.

Validation covers at least:

- nested, overlapping, or unclosed DSL sessions;
- unbalanced rig branch stacks;
- missing, duplicate, or invalid joint names;
- more than one root, cycles, invalid parentage, and non-finite transforms;
- invalid radii, degenerate segments, mirror rename-rule violations, and mirror-name
  collisions;
- invalid geometry selections, duplicate primary bindings, unresolved attachments, or an
  animated attachment in v1;
- invalid clip duration, rate, marker, track, loop, or additive metadata;
- generated callbacks producing missing/non-finite pose values or attempting structural
  session mutation while samples are being captured;
- duplicate input names, unsupported types, invalid cadence/defaults, and a frame-to-fixed
  dependency path;
- target start/end ancestry failures, degenerate/overlapping chains, missing or multiple
  transform drivers, and invalid smoothing values;
- missing controller types or incompatible controller schema versions;
- skin vertices with no valid influence, excess influences after packing, or weights outside
  normalization tolerance;
- artifact hard-limit violations and `.part`/`.anim` commit-signature mismatches;
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
- Fresh-bind sample initialization, local delta composition, untouched-joint sampling, and
  exact loop closure.
- Marker ordering and boundary crossing, including loops and large fixed steps.
- `.anim` deterministic byte output for the same supported toolchain/ABI, round trip,
  compatibility-epoch rejection, and rebuild without resolved-hash changes.
- Torn/missing sibling rejection, commit-manifest checksum checks, and transactional failure
  preserving the previous in-memory pair.

### Geometry-binding tests

- Radius interpolation and segment-envelope construction.
- Deterministic influence fields and stable tie-breaking.
- At most four packed influences and normalized weights within tolerance.
- Bind-pose skinning reproducing source positions and normals.
- Independent post-LOD influence sampling for every finalized vertex stream.
- Rigid segment ownership and attachment transform composition.
- Animated-attachment rejection and mirror transform/name/socket semantics.
- Hybrid selection conflict validation.

### Runtime and ECS tests

- Base, additive, and staged-controller ordering.
- Declared input type/cadence checking, fixed/frame buffers, and generational handle
  invalidation.
- Previous/current fixed state and accumulator-alpha interpolation as explicit runtime
  machinery.
- Frame-rate-independent half-life smoothing.
- Target driver arbitration, enable/disable fades, snap boundaries, chain inference,
  ordered subtree reconversion, and unreachable solutions.
- Fixed post-physics world queries using a deterministic fake plus Box3D integration tests.
- Root-motion consumption before fixed controller queries and world-target conversion using
  the current post-physics transform.
- Fixed-tick marker determinism under varied render frame rates.
- Root-motion extraction without direct entity-transform mutation.
- Asset sharing with independent per-instance clocks and targets.
- Live-reload migration by matching input name/type/cadence and target driver/chain.
- `SimulationControl` Animator checkpoint capture and exact Play/Stop restoration.

### Renderer tests

- Rigid animation changes instance/TLAS transforms without changing BLAS geometry.
- One existing dynamic slot per rigid segment/attachment and predictable capacity failure.
- GPU skinning output compared with a CPU reference pose.
- Conservative animated bounds over representative clips and controller extremes.
- Batched skin work queues and shared per-frame arenas across raster passes.
- Joint/work-item/vertex budget accounting, pose-rate downshifts, and overflow fallback.
- Bind-pose ray-tracing fallback for skinned rigs with no BLAS update flags or update builds.
- Clean degradation after simulated allocation/update failure.

### Integration and manual acceptance

An `AnimatedRigGallery` JavaScript part is the release acceptance asset. It contains:

- a soft voxel creature generated with `skin(rig)`;
- a segmented machine generated with `segments(rig)`;
- at least one independently instanced joint attachment;
- generated idle, walk, and run clips blended by speed;
- one fixed native gait controller using the injected ground ray query;
- foot and look targets with enable/disable smoothing;
- opt-in desired root motion consumed by a test authority system;
- budget counters plus at least one forced pose-rate/fallback transition;
- Part Workbench skeleton, envelope, target, chain, and weight overlays;
- a successful transactional live edit and an intentionally failed edit that retains the
  previous valid animation.

The acceptance asset definition is authored entirely in JavaScript; C++/editor test systems
drive its runtime inputs and targets. It remains functional after `.part`, `.anim`, and
`.anim.commit` are deleted and rebuilt.

## Delivery Sequence

### Phase A — event-independent foundation

1. Vendor and pin ozz-animation with license/version metadata.
2. Build the narrow ozz adapter and headless asymmetric-pose smoke tests.
3. Add the `.anim` header, compatibility epochs, sibling commit manifest, coherent loader,
   and pre-commit pair validation without changing resolved-hash identity.
4. Add Matter rig/clip/graph intermediate representations and validators.
5. Add the stateful rig, exact generated-clip sampling contract, and motion DSL.
6. Add implicit skin envelopes, post-LOD influence sampling, rigid-segment records, static
   attachment validation, and hard artifact limits.

### Phase B — C++ runtime and rigid Vulkan rendering

1. Add shared asset storage, private instance storage, and `PartInstance + Animator` ECS
   integration.
2. Add C++ generational input/target handles, fixed/frame cadence buffers, previous/current
   state, runtime interpolation alpha, staged evaluation, and Animator checkpoints.
3. Add markers, clip-only desired root motion, explicit phase ordering, injected fixed world
   queries, gait state, target arbitration, and ordered IK subtree reconversion.
4. Deliver rigid raster/TLAS animation through one existing dynamic slot per segment or
   attachment as the first visible end-to-end renderer path.

### Phase C — skinned Vulkan rendering

1. Extend animation vertex streams and add joint-palette upload/storage.
2. Add the visible skin work queue and per-frame-in-flight deformed-vertex arenas.
3. Add compute skinning and reuse deformed slices across raster passes.
4. Extend Vulkan cull records with conservative animated bounds.
5. Add joint/work-item/vertex budgets, 60/30/15 Hz cosmetic evaluation tiers, diagnostics,
   and bind-pose RT fallback. Exact deforming BLAS work is not in this phase.

### Phase D — event and editor integration

1. Adapt structured compiler/runtime diagnostics to the session event hub.
2. Connect editor commands and inspector synchronization to the application hub.
3. Add Part Workbench animation tabs and debug overlays.
4. Add transactional live-reload presentation and migration diagnostics.

### Phase E — optional gameplay-JavaScript binding

After the gameplay-scripting host exists, expose the Phase B C++ input and target handles to
that host. No compiler, asset, graph, ECS, or renderer milestone waits for this phase.

Phase D begins after the editor event-system architecture lands. Phases A, B, and C can
proceed in parallel with that branch. Phase B provides a fully testable C++ contract and
does not depend on runtime JavaScript. Phase C is intentionally its own renderer project
because compute skinning, buffer arenas, and animated culling are greenfield work.

## Implementation Decomposition

This document is the architectural contract. Detailed implementation plans should be split
into bounded projects so each can be reviewed and verified independently:

1. **Ozz foundation and `.anim` container**
2. **Stateful rig, clip, and motion DSL/compiler**
3. **Skin, rigid-segment, and attachment binding**
4. **C++ runtime graph, cadence/interpolation, targets, ECS, markers, world queries, and
   root motion**
5. **Rigid Vulkan renderer bridge**
6. **Compute-skinned Vulkan raster path, bounds, and budgets**
7. **Part Workbench and event-system integration**
8. **Optional gameplay-JavaScript binding after its host exists**

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
- Exact deforming BLAS update/refit and generated RT animation proxies.
- Pose-following Box3D shapes, skinned-surface collision, and articulated rigid-body rigs.
- Persistent world save/load beyond the editor `SimulationControl` checkpoint.
- Nested animated attachments and recursive animator ownership.

These extensions fit behind the named-target, native-controller, asset, and snapshot
boundaries. None is required to validate the procedural-first core.
