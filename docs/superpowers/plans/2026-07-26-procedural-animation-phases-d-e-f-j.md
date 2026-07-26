# Procedural Animation Phases D, E, F, J — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps
> use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the shipped procedural animation system usable without C++ (D, E), produce
believable limbs (F), and make ray-traced reflections and shadows match the animated pose (J).

**Basis:** [system design](../specs/2026-07-22-procedural-animation-system-design.md) as
revised 2026-07-26, and [sequencing](2026-07-26-procedural-animation-remaining-work.md).
Phases A–C are delivered and merged. G/H/I are planned but deprioritized and are NOT in
this document.

**Order:** D+E and J0 run concurrently → F → J1..n. Each phase is its own branch off `main`.

## Global Constraints

These are the invariants the delivered system enforces. Breaking one is a design change,
not an implementation detail.

- JavaScript is the only authored source of truth. No imported skeletons, meshes, or clips.
- No ozz type appears in a public header or the JavaScript API. `check-animation-runtime-boundary`
  and `check-animation-production-sources` enforce the bake/runtime split; keep them passing.
- **No JavaScript executes inside pose evaluation.** Phase E is write-only into staged
  buffers plus read-only accessors. A script must not be able to observe or interpose on
  an evaluation phase.
- Animation publishes intent (`DesiredRootMotion`, pose snapshots); it never writes an
  entity transform.
- A declared target has exactly one driver: one external writer or one controller.
- Writes are staged and sampled at the declared cadence boundary. `sample_fixed_controls`
  and `sample_frame_controls` are runtime phase hooks and are not part of any public
  control surface.
- Overlay primitives are transient: never into `.part`, BLAS, culling bounds, or checkpoints.
- New tests follow red-green-refactor. **Assert on resulting state, not on return codes** —
  the three IK defects found in review all passed a suite that only checked `== true`.
- Every new `.cpp` goes into the explicit source lists (`ME3_CPP`/`ME3_OBJ`,
  `WIN_ME3_CPP`/`APP_SRC`, and the focused test lists). Nothing is discovered by glob. If
  a test target compiles `script_host.cpp` or `part_graph.cpp`, it needs
  `$(ANIMATION_BAKE_HOST_CPP)` and `$(OZZ_OFFLINE_LIBS)`.
- Windows builds run from MSYS2 UCRT64 with `TMP`/`TEMP` passed as make variables:

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make -C MatterEngine3 TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp"
```

  The ozz CMake build is the exception — it must be driven from PowerShell (see
  `MatterEngine3/tools/build_ozz.sh` header).

## Known-red baseline

`run-graph-integration`, `run-asyncbake`, `run-viewer-logic`, `run-terrainverb`,
`run-sectorbake`, `run-example`, `run-gallery`, `run-treebake`, `run-grasslod`, and
`run-stressforest` fail on Windows for `main` itself — they hardcode POSIX `/tmp` sandbox
paths. Verified against a clean `main` worktree. **Do not treat these as regressions**, and
do not use them as a gate. Fixing them is a worthwhile separate task.

---

# Phase D — Editor and event integration

The editor event architecture (`matter::evt::Hub`, `SceneService`, `Property<T>`,
`CommandRegistry`) has landed. A–C deliberately returned diagnostics directly instead of
depending on it. D is a translation and presentation layer over finished contracts.

## Task D1: Adapt animation diagnostics onto the session hub

**Files:**

- Create: `MatterEngine3/include/matter/events/animation_events.h`
- Modify: `MatterEngine3/src/animation/animation_validate.cpp` (emit path only)
- Modify: `MatterEngine3/src/script_host.cpp` (bake diagnostics fan-out)
- Test: `MatterEngine3/tests/animation_events_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

- [ ] Write a failing test that subscribes a typed subscriber to the session hub, bakes a
      part with a deliberately invalid rig, and asserts the diagnostic arrives as a typed
      event carrying module identity, resolved hash, stage, severity, source span, and the
      offending rig/joint/clip/input/target name.
- [ ] Define the typed events mirroring the existing `animation::Diagnostics` item fields.
      Do **not** invent new diagnostic content at the editor — this is translation only, so
      a headless bake and an editor bake report identical text.
- [ ] Emit on the session hub at the same points that currently populate `Diagnostics`.
      Structured direct return stays; the hub is additive, so headless callers are unchanged.
- [ ] Run `make -C MatterEngine3/tests run-animation-events GRAPHICS=GRAPHICS_API_OPENGL_43`.
- [ ] Commit: `git commit -m "feat(animation): report compiler diagnostics as typed events"`.

## Task D2: Part Workbench animation tabs

**Files:**

- Create: `MatterEditor/src/animation_panel.cpp` / `.h`
- Modify: `MatterEditor/src/ui.cpp`, `MatterEditor/src/ui.h`, `MatterEditor/Makefile`
- Test: `MatterEditor/tests/test_animation_panel_model.cpp`

- [ ] Write a failing test over the panel's *model* (selection, tab state, derived rows) with
      no ImGui dependency, so the logic is testable headlessly like `editor_model`.
- [ ] Add the six tabs from the design: Rig, Skin, Clips, Graph, Targets, Render. Every one
      is observational over committed `.anim` plus immutable pose snapshots. None writes.
- [ ] Reuse `animation_debug_overlay.*` for viewport drawing. Do not add a second overlay
      implementation; it already draws bones, joint axes, radius envelopes, sockets, target
      transforms, IK chains, conservative bounds, and optional skin weights.
- [ ] Targets tab gizmos write through `AnimationService::set_transform`, so they are
      ordinary external drivers and inherit one-driver arbitration. A gizmo on a
      controller-driven target must be visibly disabled, not silently ignored.
- [ ] Run the panel model test and build `make -C MatterEditor windows`.
- [ ] Commit: `git commit -m "feat(editor): add Part Workbench animation tabs"`.

## Task D3: Assert the overlay isolation invariant

**Files:**

- Test: `MatterEngine3/tests/animation_overlay_isolation_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

- [ ] This invariant is currently documentation only. Write a test that enables every
      overlay option, bakes and reloads a part, and asserts: `.part` bytes are identical to
      a bake with overlays disabled; no BLAS handle changed; culling bounds are unchanged;
      and a captured checkpoint contains no overlay state.
- [ ] Run it, and confirm it fails if you deliberately route an overlay primitive into the
      bake — a test that cannot fail is not a test.
- [ ] Commit: `git commit -m "test(animation): pin the overlay isolation invariant"`.

## Task D4: Transactional live-reload presentation

**Files:**

- Modify: `MatterEditor/src/animation_panel.cpp`, `MatterEditor/src/session_binding.cpp`
- Test: extend `MatterEditor/tests/test_animation_panel_model.cpp`

- [ ] The transaction already works: the editor keeps the previous valid `.anim` until a
      replacement compiles, validates, loads, and generation-swaps. What is missing is
      *legibility* — an author cannot currently tell why a declaration reset.
- [ ] Show, per declaration: migrated / reset-because-type-changed /
      reset-because-cadence-changed / reset-because-chain-changed / removed. Inputs migrate
      on name+type+cadence; targets additionally on driver kind and joint chain.
- [ ] Show the failed-edit case explicitly: the previous animation is still live, and the
      diagnostic says why the candidate was rejected.
- [ ] Commit: `git commit -m "feat(editor): explain live-reload declaration migration"`.

## Phase D Gate

- [ ] All `run-animation-*` suites green; `MatterEditor windows` links; overlay isolation
      test passes; a manual pass over the gallery shows all six tabs populated, a working
      target gizmo, and a legible failed live edit.

---

# Phase E — Gameplay JavaScript writes to animation inputs

The C++ control surface is complete. This is an adapter. The whole risk is letting script
reach something it must not.

**Blocked on:** the gameplay-scripting host existing. Nothing else in this plan waits on E.

## Task E1: Bind handle resolution and the write surface

**Files:**

- Create: `MatterEngine3/src/script/animation_script_binding.cpp` / `.h`
- Modify: `MatterEngine3/Makefile`, `MatterEditor/Makefile`
- Test: `MatterEngine3/tests/animation_script_binding_tests.cpp`

- [ ] Write failing tests for: resolving an input and a target by name; writing each declared
      type; a type mismatch rejected with a script-visible diagnostic; a cadence mismatch
      rejected; a stale handle after a live-reload generation bump rejected; and a write to a
      controller-driven target rejected by one-driver arbitration.
- [ ] Bind `input(name)` / `target(name)` and the `set*` / `snap` writers. The JS-side object
      holds the **generational handle**, not the name, so a reload invalidates it exactly as
      it does for a C++ caller.
- [ ] Bind read-only `number_value`, `bool_value`, `status`.
- [ ] Do **not** bind `sample_fixed_controls`, `sample_frame_controls`,
      `attach_runtime_systems`, `insert_asset`, `release_asset`, `create`, `replace_asset`,
      `remove`, or any checkpoint method. Add a test asserting these names are absent from
      the binding table — the negative surface is the security property here.
- [ ] Rejections must be script-visible diagnostics, not silent `false`. A silently dropped
      write is the most common authoring mistake and the hardest to debug.
- [ ] Run `make -C MatterEngine3/tests run-animation-script-binding GRAPHICS=GRAPHICS_API_OPENGL_43`.
- [ ] Commit: `git commit -m "feat(animation): bind declared inputs and targets to gameplay script"`.

## Task E2: Prove script cannot enter pose evaluation

**Files:**

- Test: extend `MatterEngine3/tests/animation_script_binding_tests.cpp`

- [ ] Write a test that installs a script writing an input every tick, runs 100 fixed ticks
      across varied render frame rates, and asserts on the **phase trace** that no QuickJS
      frame is active during any evaluation phase.
- [ ] Assert a script write to a `fixed` input lands at the next fixed boundary and a `frame`
      write at the next frame sampling — identical timing to the equivalent C++ write.
- [ ] Assert determinism: the same script and the same tick sequence produce identical fixed
      pose checksums across two runs and across two render frame-rate patterns.
- [ ] Commit: `git commit -m "test(animation): pin the script/evaluation boundary"`.

## Phase E Gate

- [ ] Binding tests green; negative-surface test green; determinism unchanged from the Phase
      B/C acceptance baselines.

---

# Phase F — Long-chain IK and joint constraints

The authored surface does not change: the public API still names a start and an end joint
and the engine derives the chain and solver. What changes is everything behind that.

**Read first:** the three defects fixed in `animation_targets.cpp` on
`claude/procedural-animation-completion-b4651e`, and the equivariance test in
`MatterEngine3/tests/animation_ik_tests.cpp`. That test is the template for validating any
new solver.

## Task F1: Generalize chain resolution

**Files:**

- Modify: `MatterEngine3/src/animation/animation_targets.cpp` / `.h`
- Test: `MatterEngine3/tests/animation_ik_tests.cpp`

- [ ] Write failing tests for inclusive chains of length 2, 3, 4, 8, and the hard limit;
      a start that is not an ancestor of the end; and a chain crossing a branch point.
- [ ] Replace `resolve_two_bone_chain`'s fixed parent-of-parent walk with an inclusive
      ancestry walk of arbitrary length, returning the ordered joint list and the affected
      subtree range. Keep the existing function as the length-3 fast path.
- [ ] Lift the `chain.size() != 3` gates in `validate_exclusive_target_chains` and
      `solve_animation_target`. Add a maximum chain length to `AnimationBudgetConfig` and
      validate it at bake, so an authored 200-joint chain fails with a diagnostic rather
      than at runtime.
- [ ] Commit: `git commit -m "feat(animation): resolve inclusive IK chains of arbitrary length"`.

## Task F2: Add the long-chain solver

**Files:**

- Create: `MatterEngine3/src/animation/animation_ik_solver.cpp` / `.h`
- Modify: `MatterEngine3/src/animation/animation_targets.cpp`
- Test: `MatterEngine3/tests/animation_ik_tests.cpp`

- [ ] Write failing tests: reachable target converges within tolerance; unreachable target
      clamps to a stable finite result (and to the *same* result across repeated solves);
      convergence is frame-rate independent; and the **equivariance property** holds — rotate
      the rig and the model-space target by R, hold the root-relative pole fixed, and require
      the whole solution to rotate by R.
- [ ] Implement FABRIK for chains longer than three. Keep ozz's two-bone job for exactly
      three: it is cheaper and already validated. Selection is internal; the target
      abstraction exposes no solver choice.
- [ ] Iteration count and tolerance belong in `AnimationBudgetConfig`, counted in
      `AnimationRuntimeStats`, with a documented behavior on hitting the cap (accept the
      current best, never publish a partial pose).
- [ ] **Reuse the shared quaternion helper** (see Task F0 in Cross-cutting) rather than
      writing a fifth copy of the Hamilton product.
- [ ] Commit: `git commit -m "feat(animation): add long-chain IK solving"`.

## Task F3: Per-joint constraints

**Files:**

- Modify: `MatterEngine3/src/animation/animation_ir.*`, `animation_validate.cpp`,
  `MatterEngine3/src/dsl_animation.cpp`, `animation_ik_solver.cpp`
- Test: `MatterEngine3/tests/animation_ik_tests.cpp`, `animation_dsl_rig_tests.cpp`

- [ ] Long chains without limits produce visibly wrong elbows and knees. This is part of the
      same delivery, not a follow-on.
- [ ] Write failing tests for: a hinge joint that refuses to bend off-axis; a cone limit that
      clamps at its boundary; a twist limit; limits that compose with the pole; and an
      authored limit that is degenerate (zero cone, inverted range) rejected at bake.
- [ ] Add hinge (axis + min/max), cone (axis + half-angle), and twist (min/max) to the joint
      IR and the rig DSL. Validate ranges at bake with source-oriented diagnostics.
- [ ] Apply limits inside the solver iteration, not as a post-pass — clamping after
      convergence produces a pose that no longer reaches the target and looks worse than not
      solving.
- [ ] Commit: `git commit -m "feat(animation): add hinge, cone, and twist joint limits"`.

## Task F4: Deterministic ordering for shared joints

**Files:**

- Modify: `MatterEngine3/src/animation/animation_targets.cpp`, `animation_validate.cpp`
- Test: `MatterEngine3/tests/animation_ik_tests.cpp`

- [ ] **This is the real blocker, not chain length.** v1 rejects chains with overlapping
      writable joint sets, which is tolerable only because two-bone chains rarely overlap. A
      spine and an arm sharing a clavicle is ordinary.
- [ ] Write failing tests: two chains sharing one joint solve deterministically in declared
      order; the result is identical across runs and across render frame rates; a later chain
      sees the earlier chain's model-space result, not a stale one; and a cycle in the
      declared order is rejected at bake.
- [ ] Replace the blanket rejection with an ordered solve: sort by declaration order, solve
      sequentially, reconvert the affected subtree after each, and let a later chain observe
      the earlier result. Document that authors control precedence by declaration order.
- [ ] Keep rejecting the genuinely ambiguous case: two chains whose *end effectors* write the
      same joint. Ordering cannot resolve that; it needs the multi-effector solver that
      remains deferred.
- [ ] Run `run-animation-ik`, `run-animation-controller`, `run-animation-systems`, and both
      acceptance suites.
- [ ] Commit: `git commit -m "feat(animation): order IK chains that share joints"`.

## Phase F Gate

- [ ] Every IK test green including equivariance at chains of length 2, 3, 4, and 8.
- [ ] Phase B/C acceptance determinism unchanged: a rig using only two-bone chains produces
      byte-identical fixed pose checksums to the pre-F build. Long-chain support must not
      perturb existing content.
- [ ] Gallery extended with one long-chain limb (tail or spine) exercising limits and one
      pair of overlapping chains.

---

# Phase J — Deforming ray tracing

Today skinned geometry ray-traces in bind pose, so a running character's shadow and
reflection are wrong. The blocker is **buffer lifetime**, not geometry: the skinned vertices
already exist on the GPU, but their arena slices are freed when their frame fence signals,
and an acceleration-structure build extends that lifetime to a longer fence.

## Task J0: Spike — measure before committing (run concurrently with D/E)

**Files:**

- Create: `MatterEngine3/tests/vk_animation_rt_spike_tests.cpp` (throwaway; may be deleted
  after the decision is recorded)

- [ ] **Do not pick the tier from the armchair.** Prototype two paths against the existing
      C2 skinned vertex arena and measure both:
      (a) per-instance BLAS refit from the current arena;
      (b) full rebuild at a reduced rate (every N frames).
- [ ] Measure, for a gallery-scale scene: GPU time per frame, peak additional memory, and —
      the decisive number — **how long an arena slice must stay alive** for each path, versus
      the current frame-fence lifetime.
- [ ] Answer explicitly: can a refit consume the arena slice in the same frame it was
      written, or does the acceleration-structure build force a second arena generation?
      If the latter, memory cost roughly doubles and that changes the tier decision.
- [ ] Record the measurements and the chosen tier as a design amendment in the spec's
      Deforming Ray Tracing section. **The spike output is a decision, not code.**
- [ ] Commit: `git commit -m "docs(spec): record deforming RT tier decision and measurements"`.

## Task J1: Extend arena lifetime to acceleration-structure completion

**Files:**

- Modify: `MatterEngine3/src/render/vk_animation_skinning.cpp` / `.h`
- Test: `MatterEngine3/tests/vk_animation_skinning_tests.cpp`

- [ ] Write failing tests for: a slice retained past its frame fence until its AS build
      fence signals; wrap protection when retained slices exhaust the arena; and no slice
      freed while any pending build references it.
- [ ] Introduce the second fence and the retention bookkeeping the spike identified. This is
      the load-bearing change; everything after it is comparatively mechanical.
- [ ] Commit: `git commit -m "feat(render): retain skinned arenas through acceleration builds"`.

## Task J2: Build deforming acceleration structures

**Files:**

- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp` / `.h`
- Test: `MatterEngine3/tests/vk_scene_renderer_tests.cpp`

- [ ] Implement the chosen tier. Dispatch after compute skinning and before any ray-traced
      pass reads the structure; insert explicit barriers.
- [ ] Static geometry keeps its existing build-once path untouched.
- [ ] Test that a skinned instance's traced geometry matches its raster pose within
      tolerance, and that a bind-pose instance is bit-identical to the pre-J build.
- [ ] Commit: `git commit -m "feat(render): ray-trace deforming skinned geometry"`.

## Task J3: Replace the zero-budget assertion with a real budget

**Files:**

- Modify: `MatterEngine3/src/animation/animation_budget.cpp` / `.h`,
  `MatterEngine3/tests/animation_phase_c_acceptance_tests.cpp`
- Test: `MatterEngine3/tests/animation_budget_tests.cpp`

- [ ] The Phase C acceptance test currently asserts **zero** deforming-BLAS updates. Do not
      delete that assertion — deleting it silently removes the guarantee that nothing is
      quietly refitting acceleration structures.
- [ ] Replace it with a configured limit, a fallback, and a counter: over-budget instances
      fall back to their **bind-pose** structure (never a stale deformed one), the fallback
      reason is recorded in `AnimationRuntimeStats`, and the acceptance test asserts the
      gallery stays within the new budget with zero fallbacks at defaults.
- [ ] Test the over-budget path explicitly: force the limit low, assert bind-pose fallback
      and a non-zero counter rather than a dropped frame or a partial structure.
- [ ] Commit: `git commit -m "feat(animation): budget deforming acceleration updates"`.

## Phase J Gate

- [ ] GPU capture shows: skin dispatch precedes structure build, barriers present, only
      visible work dispatched, bind-pose structures still build-once for static geometry.
- [ ] Vulkan validation clean.
- [ ] A moving character's ray-traced shadow and reflection track its raster pose.
- [ ] Over-budget falls back to bind pose with a counted reason.

---

# Cross-cutting — do these first

Small, and they reduce risk across D/E/F/J. F2 in particular should not start before F0.

## Task F0: One quaternion helper

- [ ] Four independent copies of the Hamilton product exist (`animation_evaluator`,
      `animation_systems`, `animation_binding_bake`, `animation_targets`). One of them was
      wrong for the entire life of the branch. Consolidate to a single shared helper with a
      test that pins the convention, and delete the copies.
- [ ] A background task for this may already be running — check before duplicating it.
- [ ] Commit: `git commit -m "refactor(animation): consolidate the quaternion helpers"`.

## Task X1: Reformat the dense math

- [ ] Several animation sources put whole functions on one line. That style concealed all
      three IK defects. Reformat the math-bearing helpers to one statement per line, with no
      behavior change, and re-run the animation suites to prove it.

## Task X2: Fix the `/tmp` test suites

- [ ] Ten suites hardcode POSIX `/tmp` and are red on Windows for `main` itself. They
      currently hide real Windows regressions. Route them through the same
      `local_fixture_root` helper the animation A8 fixtures already use.

---

## Definition of Done

- [ ] Phases D and E: an author can build, drive, and debug an animated rig without writing
      or reading C++.
- [ ] Phase F: a limb of arbitrary chain length reaches its target with believable joint
      behavior, and two chains sharing a joint resolve deterministically.
- [ ] Phase J: ray-traced reflections and shadows track the animated pose, with a budgeted
      bind-pose fallback.
- [ ] Every invariant in Global Constraints still holds, each pinned by a test that fails
      when the invariant is broken.
- [ ] Phase B/C determinism baselines unchanged for content that does not use the new features.
