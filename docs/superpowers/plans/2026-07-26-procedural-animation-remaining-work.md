# Procedural Animation — Remaining Product Work (post Phase A–C)

**Date:** 2026-07-26
**Status:** Proposed; sequencing and scope for review
**Predecessors:** [system design](../specs/2026-07-22-procedural-animation-system-design.md),
[Phase A–C plan](2026-07-22-procedural-animation-phase-abc.md)

Phases A–C are merged onto current `main`. This document plans the seven items the
A–C plan listed under "Explicitly Deferred Beyond Phase C", against the code that
actually exists now rather than against the original design's projection of it.

## What already exists (the seams these build on)

Anything below that says "extend X" means X is present and tested today:

| Seam | Where | What it already guarantees |
|---|---|---|
| Named input/target resolution | `AnimationService::input(handle,name)` / `target(handle,name)` | Generational handles carrying declared type + cadence; invalid writes rejected, not coerced |
| Staged write surface | `set(...)`, `set_enabled/weight/transform`, `snap` | Writes are staged and sampled at the declared cadence boundary, never applied mid-evaluation |
| Cadence sampling hooks | `sample_fixed_controls()` / `sample_frame_controls()` | Explicitly NOT part of the gameplay-facing surface; the runtime owns when writes land |
| Immutable pose publication | `AnimationPoseSnapshotStore` | Renderer reads by instance handle + frame serial; no Flecs access |
| Checkpoint transaction | `capture/validate/restore_runtime_checkpoints` | Transactional; a rejected restore leaves service and runtime bridge untouched |
| Rigid instancing lane | `AnimationRigidBridge` + `DynamicInstanceKey{entity,generation,binding_index}` | One generational slot namespace already keyed for multiple bindings per entity |
| Budget + fallback accounting | `AnimationBudgetConfig`, `AnimationRuntimeStats` | Central limits, fallback reasons, pose-LOD tiers |
| Bake/runtime boundary | `MATTER_RUNTIME_ANIMATION_ONLY`, `check-animation-production-sources` | Ozz offline compiler cannot reach a runtime link line; enforced by the build |

Two properties below are load-bearing for almost every item and should not be
renegotiated casually: **a declared target has exactly one driver**, and
**animation never mutates an entity transform directly** (it publishes
`DesiredRootMotion` for an authority to apply or reject).

---

## Sequencing (decided 2026-07-26)

The spec now names these Phases D–J; the W-numbers below map as: W1→E, W2→D, W3→J,
W4→G, W5→I, W6→H, W7→F.

Priorities were set against **what you want to be able to do that you can't today**,
and three were chosen: author rigs without C++, get believable limbs, and get correct
reflections/shadows on animated geometry. Durable save and ragdoll were *not* chosen,
so H and I sit below G.

```text
      ┌─ Phase D  editor integration ─┐
      │  Phase E  gameplay-JS writes  ├─> authoring is usable without C++
      └───────────────────────────────┘
         Phase F  long-chain IK + joint limits   -> believable limbs
         Phase J  deforming ray tracing          -> correct RT
            └─ J0 spike runs CONCURRENTLY with D/E

   later, unprioritized: G (nested attachments) -> H (durable save) -> I (ragdoll)
```

**Order: D+E and J0 in parallel → F → J1..n → G → H → I.**

Rationale for the one non-obvious choice: J is the largest and riskiest item, and its
central problem (buffer lifetime, below) is a *measurement*, not a build. Running the
J0 spike beside the cheap authoring work answers the question that gates the whole
investment before anyone commits schedule to it. If J0 says refit is impractical, the
tier decision changes and no build work was wasted.

Decisions taken:

- **RT tier is deliberately undecided.** J0 prototypes BLAS refit against reduced-rate
  rebuild using the existing skinned arena, measures both, then commits. Do not pick
  from the armchair.
- **Nesting is depth-bounded at 3**, rejected at bake past that. Keeps budget
  accounting and subtree-atomic checkpointing tractable.
- **Each phase lands as its own branch off `main`.** This branch merges first. The
  107-commit divergence resolved this session is the argument.

---

## W1 — Runtime/gameplay JavaScript writes to animation inputs

**Size:** small-to-moderate. This is an adapter, not new runtime machinery.

The design already decided this is "Phase E: expose the Phase B C++ input and target
handles to that host". The C++ surface is complete; the work is binding it without
letting JavaScript into pose evaluation.

- Bind `input(name)` / `target(name)` resolution and the `set*` writers to the
  gameplay host. Handle objects on the JS side must hold the generational handle,
  not a name, so a live-reload generation bump invalidates them the same way it
  does for C++ callers.
- Do **not** expose `sample_fixed_controls`/`sample_frame_controls`. Those are the
  runtime's phase hooks; a script that could call them would break the "writes land
  at the declared cadence boundary" invariant.
- Writes from script are ordinary staged writes: a script writing a `fixed` input
  mid-frame is sampled at the next fixed tick, exactly like a C++ write.
- Reject a write whose declared type or cadence does not match, and surface it as a
  script-visible diagnostic rather than a silent no-op — this is the single most
  common authoring mistake and the existing API already returns `false`.
- Enforce the design's non-goal: no per-frame JavaScript callback inside evaluation.
  The binding is write-only into the staged buffers plus read-only status/value
  accessors (`number_value`, `bool_value`, `status`).

**Blocked on:** the gameplay-scripting host existing. Nothing else here waits for it.

**Tests:** type/cadence mismatch rejection; generation invalidation after reload;
a script write and a C++ write to the same input arbitrating identically; proof that
no QuickJS frame runs inside the evaluation phase (assert on the phase trace).

---

## W2 — Phase D editor-event integration and UX

**Size:** moderate; mostly presentation over contracts that already exist.

The editor event architecture has landed on `main` (`matter::evt::Hub`,
`SceneService`/`SceneChangeTracker`, `Property<T>`, `CommandRegistry`). A–C
deliberately returned structured diagnostics directly instead of depending on it.

- Adapt compiler diagnostics (`animation::Diagnostics`, already carrying module
  identity, stage, severity, source span, and rig/joint/clip/input/target names) onto
  the session hub as typed events. This is a translation layer; do not re-derive
  diagnostics at the editor.
- Part Workbench tabs per the design: Rig, Skin, Clips, Graph, Targets, Render. All
  six are observational over `.anim` + immutable pose snapshots.
- Reuse the existing debug overlay (`animation_debug_overlay.*`, already drawing
  bones, joint axes, radius envelopes, sockets, target transforms, IK chains,
  conservative bounds, optional skin weights) rather than writing a second one.
  It already reads only committed `.anim` plus immutable snapshots.
- Transactional live reload is specified and partly implemented: the editor keeps the
  previous valid `.anim` until a replacement compiles, validates, loads, and
  generation-swaps. Inputs migrate on matching name/type/cadence; targets
  additionally on driver kind and joint chain. The UX work is showing *why* a
  declaration reset, which is currently only knowable from diagnostics.
- Target gizmos write through `set_transform`, so they are ordinary external drivers
  and inherit one-driver arbitration for free.

**Watch for:** overlay primitives must stay transient — never into `.part`, BLAS,
culling bounds, or checkpoints. That invariant is currently only documented.

---

## W3 — Deforming ray-tracing support

**Size:** large. This is the biggest single item and the only one that changes the
acceleration-structure contract.

v1 deliberately keeps skinned RT geometry on the immutable bind-pose `.part` BLAS and
asserts builds are build-once with no update/refit flags. Raster is exact animated
pose; RT is a documented bind-pose fallback. Undoing that is a renderer project.

- Decide the tier first, because it determines everything else: (a) per-instance BLAS
  refit from the compute-skinned vertex arena, (b) periodic full rebuild at reduced
  rate, or (c) a decoupled lower-detail RT proxy mesh. The design explicitly defers
  "generated RT animation proxies", so (c) is a spec change, not just an implementation.
- The skinned vertex data already exists on the GPU: C2 writes current and previous
  deformed positions into per-frame-in-flight arenas. A refit path consumes the
  current arena rather than introducing a new skinning pass.
- The hard part is lifetime, not geometry. Arena slices are currently freed when their
  frame fence signals; a BLAS built from a slice extends that lifetime into
  acceleration-structure build completion, which is a different (and longer) fence.
- Budget this explicitly. The current default is *zero* deforming-BLAS updates, and
  `AnimationRuntimeStats` asserts it. Any tier above zero needs its own limit,
  its own fallback (bind pose) and its own counter, or the acceptance test that
  asserts zero simply gets deleted — which would lose the guarantee.
- Keep the bind-pose path as the fallback for over-budget instances rather than
  degrading to a stale deformed BLAS.

**Prerequisite:** a design amendment. Do not start implementation against the current
spec, which names this a non-goal.

---

## W4 — Nested animated attachments

**Size:** moderate. Mostly a validation and ordering problem.

v1 requires attachments to resolve to *static* parts and rejects an animated
attachment at bake. Lifting that means an animator can own a child animator.

- Evaluation order becomes a dependency graph rather than a flat list. Parent poses
  must publish before child animators consume their socket transform, within the same
  frame, or attachments lag by one frame.
- Cycle detection at bake: an animated attachment chain that reaches itself must fail
  with a source-oriented diagnostic, like every other v1 structural violation.
- Depth limit and budget interaction: nested animators multiply instance count against
  the 4096-animator budget, and the 65,536-evaluated-joints budget is per frame across
  all of them. Decide whether a nested subtree is budgeted as one unit or N.
- `DynamicInstanceKey` already carries `binding_index`, so the rigid lane can express
  nested bindings without a schema change — but the *world transform composition*
  (`entity_world * joint_model * socket_local * attachment_local`) gains a level and
  needs the parent's published pose, not its bind pose.
- Checkpoint/restore must capture the whole subtree atomically, or a restore can pair
  a parent's tick-N pose with a child's tick-N+1 state.

**Recommendation:** cap nesting depth (2 or 3) in the first delivery. Unbounded
recursion turns budget accounting and checkpoint atomicity into open problems.

---

## W5 — Animation-driven physics and ragdolls

**Size:** large, and the most likely to violate an existing invariant.

The design's non-goals currently include pose-following physics shapes, skinned-surface
collision, and articulated rigid-body dynamics; root/entity collision is authoritative.

- This is the one item that pushes against "animation never mutates the entity
  transform". A ragdoll is authority flowing the other way: physics drives the pose.
  Model it as an explicit authority handover with a defined blend, not as animation
  gaining write access to transforms.
- The existing phase order already puts physics between `FixedUpdate` and
  `FixedPostUpdate`, and controller world queries already run post-step against the
  settled world. A pose-following shape updates in `PostPhysics`; a ragdoll reads
  physics transforms there and publishes them as the fixed pose.
- Blending in and out of ragdoll is the actual product feature and the hard part:
  entering needs the current pose as initial rigid-body state, exiting needs a
  convergence back to animated pose over a defined interval.
- Checkpoint scope grows to include rigid-body state for ragdolled animators, which is
  physics-engine state rather than animation state — a new serialization surface.
- Keep it opt-in per animator. Every existing rig must be unaffected.

**Prerequisite:** a design amendment, same as W3.

---

## W6 — Durable save-game support

**Size:** small-to-moderate, and the highest value per unit effort.

`AnimatorCheckpoint` already exists and already round-trips through
`SimulationControl` for editor Play/Stop, with deterministic replay tested. The gap
between that and a save-game is **durability and versioning**, not capture.

- Today's checkpoint is an in-memory snapshot whose validity is scoped to one session:
  it references the asset by identity and assumes the same `.anim` is loaded. A durable
  save must survive an asset rebuild at the same resolved hash.
- Reuse the artifact compatibility machinery that already exists: the `.anim` header
  and commit record carry schema version, bake epoch, pinned ozz version, target ABI
  tag, and adapter serialization version. A save record should carry the same, and a
  mismatch should degrade gracefully (reset the animator to defaults, keep the entity)
  rather than fail the load.
- Declaration migration on load is the same problem live-reload already solves: inputs
  migrate on matching name/type/cadence, targets additionally on driver kind and chain.
  Reuse that path rather than writing a second migration rule set.
- Controller state is opaque serialized bytes today. Durable saves need those bytes to
  be versioned per controller type ID, or a controller change silently misinterprets
  old saves. This is the one genuinely new contract.
- Explicitly out of scope: network replication of raw poses. The design's position —
  replicate semantic inputs/targets/clocks — still holds.

---

## W7 — General long-chain IK / constraint solving

**Size:** moderate. The public API was designed for this.

v1 accepts exactly three joints / two segments and rejects anything else. The design
anticipated the extension: "the public API names a start and end joint; engine derives
the chain and solver" and "longer-chain support may be added behind the same
declaration later". So the authored surface does not change.

- `resolve_two_bone_chain` becomes chain resolution of arbitrary inclusive length; the
  `!= 3` rejection in `validate_exclusive_target_chains` and `solve_animation_target`
  is the actual v1 gate.
- Solver selection is internal: keep ozz two-bone for exactly-three chains (it is
  cheaper and already validated) and add FABRIK or CCD above that length. The target
  abstraction exposes no solver choice, so this stays an implementation detail.
- Overlapping writable chains are currently rejected outright rather than ordered.
  Longer chains make overlap far more likely (a spine and an arm sharing a clavicle),
  so this is the point at which deterministic solver ordering has to be designed
  rather than avoided.
- Per-joint constraints (hinge limits, twist limits, cone limits) are the "general
  joint-constraint authoring language" the plan defers. Long-chain solving without
  limits produces visibly wrong elbows and knees, so scope at least hinge + cone.
- **Carry the fixes from this branch forward.** The three defects found in
  `animation_targets.cpp` (quaternion product, matrix-to-quaternion branch coverage,
  pole frame direction) were all in code a longer-chain solver will reuse or imitate.
  The equivariance test added in `animation_ik_tests.cpp` is the right shape for
  validating any new solver: rotate the rig and the model-space target, hold the
  root-relative pole fixed, and require the solution to rotate with it.

---

## Cross-cutting items worth doing first

These are small and reduce risk across several workstreams:

1. **Consolidate the quaternion helpers.** Four independent copies of the Hamilton
   product exist (`animation_evaluator`, `animation_systems`, `animation_binding_bake`,
   `animation_targets`). One of them was wrong. One shared helper makes that class of
   defect impossible.
2. **Reformat the dense one-liner math.** Several animation sources put whole functions
   on a single line; that style is what concealed all three IK defects.
3. **Assert the overlay invariant.** "Overlay primitives never enter `.part`, BLAS,
   culling bounds, or checkpoints" is currently documentation only.
4. **Fix the pre-existing Windows test failures** (`run-graph-integration`,
   `run-asyncbake`, `run-viewer-logic` hardcode POSIX `/tmp` sandbox paths). They fail
   identically on clean `main`, so they currently hide real regressions on Windows.
