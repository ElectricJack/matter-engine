# Procedural Animation Phases A-C Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a deterministic, rig-first procedural animation pipeline to MatterEngine: JavaScript authors rigs, generated geometry, clips, graphs, and declared controls; native C++ evaluates animation and IK; the production Vulkan renderer supports rigid-segment and skinned output.

**Architecture:** The bake host compiles one JavaScript source into a coherently published `.part` + optional `.anim` pair. `.anim` contains Matter-owned schemas plus ozz-animation runtime blobs, but every public/runtime boundary uses Matter types. Native animation instances evaluate fixed-rate controller state and frame-rate presentation state into immutable pose snapshots. The ECS consumes root-motion intent and physics-backed world queries at explicit phases. The Vulkan bridge consumes snapshots without accessing Flecs, first by expanding rigid bindings into existing dynamic instances and then by compute-skinning visible meshes. Ray tracing intentionally retains bind-pose BLAS in v1.

**Tech Stack:** C++17, JavaScript/QuickJS DSL, ozz-animation 0.16.0, Flecs, Box3D, Vulkan compute/raster/ray tracing, GNU Make, CMake (ozz only), existing MatterEngine `.part` cache/provider pipeline.

**Upstream basis:** Pin the official [ozz-animation 0.16.0 release](https://github.com/guillaumeblanc/ozz-animation/releases/tag/0.16.0). Its documented split between offline builders and the `ozz_animation`/`ozz_base` runtime libraries matches the bake/runtime boundary in this plan; see the official [build](https://guillaumeblanc.github.io/ozz-animation/documentation/build/), [offline](https://guillaumeblanc.github.io/ozz-animation/documentation/animation_offline/), [runtime](https://guillaumeblanc.github.io/ozz-animation/documentation/animation_runtime/), and [IK](https://guillaumeblanc.github.io/ozz-animation/documentation/ik/) documentation.

## Global Constraints

- JavaScript remains bake/load-time only in Phases A-C. Do not add per-tick QuickJS execution or make gameplay scripting a dependency.
- The pending editor event-system branch is not a dependency. Phases A-C return structured diagnostics directly, use Flecs only for entity-local marker/completion events, and leave editor/application hub adapters to Phase D.
- Vendor ozz-animation tag `0.16.0`, upstream commit `6cbdc790123aa4731d82e255df187b3a8a808256`, under `Libraries/ozz-animation` with `git subtree`; do not add a submodule. Record both identifiers in `Libraries/ozz-animation/MATTER_VERSION`.
- All engine-facing APIs use Matter types. Ozz headers may appear only under `MatterEngine3/src/animation/ozz_*` and their tests.
- Production support targets the Vulkan renderer. Legacy raylib/OpenGL displays may render bind pose and must not acquire a parallel animation implementation.
- A source hash identifies authored input; a commit manifest identifies the exact `.part`/`.anim` pair. A reader must never accept one animated sibling without the matching manifest and checksums.
- Static `.part` behavior and cache compatibility must remain unchanged.
- Fixed-state writes are sampled at the next fixed tick. Frame-state writes are sampled once at frame evaluation. A declared target has exactly one driver: external API or one native controller.
- Root motion is clip-only, extracted from the authored root track, and published as `DesiredRootMotion`; animation never mutates the entity transform directly.
- Frame evaluation interpolates immutable previous/current fixed snapshots. It never advances fixed controllers or performs physics queries.
- V1 hard limits are 256 joints, 4 influences per vertex, 64 IK targets, and 128 graph/controller nodes per animated asset.
- Default runtime budgets are 4096 animator instances, 64 MiB mutable CPU animation state, 2048 world queries per fixed tick, 65,536 evaluated joints per frame, 256 visible skin work items per frame, 2,000,000 skinned output vertices per frame, and zero deforming-BLAS updates.
- Invalid authored data fails the bake with source-oriented diagnostics. Invalid/corrupt runtime data fails closed and preserves the last good asset during reload.
- New tests follow red-green-refactor. Every task starts with the failing assertion, implements the smallest complete contract, then runs its focused test plus affected regression tests.
- Every task that adds a `.cpp` also adds it to the relevant explicit `ME3_CPP`, `WIN_ME3_CPP`, `APP_SRC`, or focused test source list in `MatterEngine3/Makefile`, `MatterViewer/Makefile`, and/or `MatterEngine3/tests/Makefile`; this repository does not discover sources by glob.
- Run Windows builds from MSYS2 UCRT64 with:

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
export TMP="C:/Users/webde/AppData/Local/Temp"
export TEMP="$TMP"
```

## Canonical Runtime Pipeline

```text
JavaScript source
  -> ScriptHost + stateful DSL
  -> AnimationBuild IR + existing BuildOps
  -> validation + ozz compilation + final-LOD binding bake
  -> candidate .part + candidate .anim
  -> pair validation
  -> atomic sibling replacement + .anim.commit published last

Committed asset
  -> AnimationStore / AnimatorInstanceStore
  -> fixed controllers + physics-backed queries
  -> previous/current fixed snapshots
  -> frame interpolation + clip blend + local-to-model
  -> model-space IK + affected-subtree local-to-model
  -> immutable AnimationPoseSnapshot
  -> DynamicSceneBridge (rigid) or Vulkan skin queue (skinned)
```

## File Ownership Map

| Area | Primary files | Rule |
|---|---|---|
| Ozz integration | `Libraries/ozz-animation/**`, `MatterEngine3/src/animation/ozz_*` | Only adapter files include ozz headers. |
| Authored animation IR | `MatterEngine3/src/animation/animation_ir.*`, `animation_validate.*` | No renderer, Flecs, QuickJS, or ozz types. |
| DSL compiler | `MatterEngine3/src/script/dsl_animation.*`, `dsl_state.*`, `dsl_bindings.cpp` | Produces IR and existing geometry operations only. |
| Artifacts/cache | `MatterEngine3/src/animation/anim_asset.*`, `anim_bundle.*`, provider/ScriptHost files | Manifest is the sole animated-pair commit point. |
| Runtime | `MatterEngine3/src/animation/animation_store.*`, `animation_evaluator.*`, `animation_systems.*` | Native C++; no JavaScript calls. |
| World interaction | `animation_world_queries.*`, `animation_controllers.*`, `animation_targets.*` | Interface in animation; Box3D adapter in ECS/physics integration. |
| Rigid rendering | `dynamic_scene_bridge.*`, `dynamic_instance_slots.*`, `animation_rigid_bridge.*` | One shared generational slot namespace. |
| Skinned rendering | `vk_animation_*`, `vk_scene_renderer.*`, `shaders_vk/animation_skin.comp` | Production Vulkan only; bind-pose RT in v1. |
| Public API | `MatterEngine3/include/matter/animation.h` | Stable Matter handles/types; no implementation headers. |
| Tests | `MatterEngine3/tests/animation_*_tests.cpp` | One focused binary per contract plus phase gates. |

## Dependency and Parallelization Boundaries

- The editor event-system branch may land before, during, or after this work; none of A-C imports its APIs.
- A1 (ozz/build) and A2 (Matter IR) can proceed in parallel in isolated worktrees. A3 joins them.
- A4 (artifact container) may proceed beside A5/A6 (DSL) once A2's IR names are frozen. A7 requires the final rig/binding IR and shared indexed-geometry extraction. A8 is the Phase A integration join.
- After the Phase A gate, B1 (stores/API) and the non-ECS portion of B2 (evaluator) may proceed together. B3 freezes phase/snapshot ownership; B4 and B5 then build on that ordering. B6 requires the published pose store but is independent of gameplay/editor events.
- After the Phase B gate, C1 (GPU ABI/arenas) and the CPU portion of C3 (bounds) may proceed together. C2 requires C1; the culling integration in C3 requires C2's renderer plumbing. C4 is the final integration join.
- Do not parallel-edit the explicit source lists in the same Makefile. The coordinating implementer owns those small merge points.

---

# Phase A — Deterministic Authoring and Artifact Foundation

## Task A1: Vendor and Build the Ozz Runtime

**Files:**

- Create: `Libraries/ozz-animation/MATTER_VERSION`
- Create: `MatterEngine3/tools/build_ozz.sh`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Modify: `MatterEngine3/tests/Makefile`
- Test: `MatterEngine3/tests/animation_ozz_smoke_tests.cpp`

- [ ] Add `animation_ozz_smoke_tests.cpp` that includes only `animation/ozz_adapter.h`, constructs a two-joint Matter rig through the adapter, and asserts joint count, parent order, bind-pose model transforms, and deterministic serialization bytes. Add `run-animation-ozz`; confirm it fails because the adapter/library is absent.
- [ ] Import tag `0.16.0` with:

```bash
git subtree add --prefix Libraries/ozz-animation https://github.com/guillaumeblanc/ozz-animation.git 0.16.0 --squash
```

Write `tag=0.16.0` and `commit=6cbdc790123aa4731d82e255df187b3a8a808256` to `MATTER_VERSION`; verify the imported `CHANGES.md` identifies 0.16.0 before continuing.
- [ ] Implement `tools/build_ozz.sh` to configure a static Release build at `Libraries/ozz-animation/build/matter` with tests, samples, tools, glTF, FBX, and shared libraries disabled. Build `ozz_base`, `ozz_animation`, and `ozz_animation_offline` only.
- [ ] Add common include/library variables to all three Makefiles. Offline code links only into bake/test executables; MatterViewer and runtime binaries link only `ozz_animation` and `ozz_base`. Add the manually listed animation sources and `vpath` entries without globbing.
- [ ] Create the minimal `ozz_adapter.h/.cpp` shell needed by the smoke test. Run:

```bash
./MatterEngine3/tools/build_ozz.sh
make -C MatterEngine3/tests run-animation-ozz GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: `animation_ozz_smoke_tests: all tests passed`.
- [ ] Commit:

```bash
git add Libraries/ozz-animation MatterEngine3/tools/build_ozz.sh MatterEngine3/Makefile MatterViewer/Makefile MatterEngine3/tests
git commit -m "build: vendor ozz animation runtime"
```

## Task A2: Define the Matter Animation IR and Validation Contract

**Files:**

- Create: `MatterEngine3/src/animation/animation_ir.h`
- Create: `MatterEngine3/src/animation/animation_ir.cpp`
- Create: `MatterEngine3/include/matter/animation_types.h`
- Create: `MatterEngine3/src/animation/animation_validate.h`
- Create: `MatterEngine3/src/animation/animation_validate.cpp`
- Modify: `MatterEngine3/Makefile`
- Test: `MatterEngine3/tests/animation_ir_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

- [ ] Write failing tests for duplicate names, missing/forward parents, cycles, non-finite transforms, zero/negative radii, hard-limit overflow, invalid clip times, duplicate inputs/targets, invalid cadence, multiple target drivers, target chains not lying on one parent path, overlapping IK chains, graph cycles, bad graph references, and stable diagnostic order.
- [ ] Define shared POD `matter::AnimationTransform { Float3 translation; Quaternion rotation; Float3 scale; }` and `matter::AnimationValueType` in `animation_types.h`. Define the exact IR in `namespace matter::animation`: `JointIndex = uint16_t`, `kInvalidJoint = UINT16_MAX`, `JointRange`, `JointDef`, `SocketDef`, `RigDefinition`, `ClipDefinition`, `ClipTrack`, `ClipKey`, `ClipMarker`, `InputSchema`, `TargetSchema`, `ControllerDef`, `GraphNode`, `MotionDefinition`, `SkinBindingDef`, `RigidBindingDef`, `AttachmentDef`, and aggregate `AnimationBuild`.
- [ ] Use `enum class AnimationValueType { Bool, Number, Float3, Quaternion, Transform, Symbol }`, `enum class EvaluationCadence { Fixed, Frame }`, and `enum class TargetDriverKind { External, Controller }`. Store source spans on every authored declaration for deterministic diagnostics.
- [ ] Implement `validate_animation_build(const AnimationBuild&, Diagnostics&)`. Canonicalize joints in pre-order depth-first traversal, retaining declaration order among siblings, so parents precede children and every subtree is one contiguous index interval. Graph nodes topologically sort with declaration order as the tie-breaker.
- [ ] Make chain selection an exact inclusive `start -> ... -> end` descendant path inferred from parents. Reject a missing path, fewer than two joints, any v1 chain other than exactly three joints/two segments, and any two target chains that write a common joint.
- [ ] Run:

```bash
make -C MatterEngine3/tests run-animation-ir GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: all invalid cases fail with their named diagnostic; the canonical sample passes and serializes identically across two builds.
- [ ] Commit: `git add MatterEngine3 && git commit -m "feat: define procedural animation IR"`.

## Task A3: Complete the Ozz Adapter

**Files:**

- Modify: `MatterEngine3/src/animation/ozz_adapter.h`
- Modify: `MatterEngine3/src/animation/ozz_adapter.cpp`
- Modify: `MatterEngine3/Makefile`
- Test: `MatterEngine3/tests/animation_ozz_adapter_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

- [ ] Write failing tests for skeleton build, raw-clip optimization/build, archive round-trip, sampling at endpoints, normal/additive blend, local-to-model conversion, and two-bone IK followed by affected-subtree model recomputation.
- [ ] Expose only opaque adapter owners and Matter values:

```cpp
class OzzSkeleton;
class OzzAnimation;
struct OzzSampleContext;

bool build_skeleton(const RigDefinition&, OzzSkeleton&, Diagnostics&);
bool build_clip(const RigDefinition&, const ClipDefinition&, OzzAnimation&, Diagnostics&);
bool serialize_skeleton(const OzzSkeleton&, std::vector<uint8_t>&);
bool serialize_animation(const OzzAnimation&, std::vector<uint8_t>&);
bool deserialize_skeleton(const uint8_t* data, size_t size, OzzSkeleton&, Diagnostics&);
bool deserialize_animation(const uint8_t* data, size_t size, OzzAnimation&, Diagnostics&);
bool sample(const OzzAnimation&, float ratio, OzzSampleContext&,
            std::vector<AnimationTransform>& locals);
bool blend(const std::vector<BlendLayer>&, const std::vector<AdditiveLayer>&,
           std::vector<AnimationTransform>& locals);
bool local_to_model(const OzzSkeleton&, const std::vector<AnimationTransform>& locals,
                    std::vector<Mat4f>& models,
                    JointRange affected = JointRange::all());
bool solve_two_bone(const TwoBoneSolve&, const std::vector<Mat4f>& models,
                    std::vector<AnimationTransform>& locals,
                    std::vector<Mat4f>& updated_models);
```

- [ ] Convert all ozz allocation/job failure states into diagnostics. Do not expose archive streams, SoA transforms, or ozz spans outside the adapter.
- [ ] In `solve_two_bone`, feed model-space joint matrices to ozz, apply returned local-space corrections, then recompute the compiled contiguous affected-subtree range before any later solver runs. Keep explicit Matter-to-ozz joint mappings inside the adapter.
- [ ] Run `run-animation-ozz` and `run-animation-ozz-adapter`; expect exact round-trip equality at Matter boundaries and epsilon-based transform assertions.
- [ ] Commit: `git add MatterEngine3 && git commit -m "feat: add Matter ozz adapter"`.

## Task A4: Add `.anim` and Coherent Bundle Publication

**Files:**

- Create: `MatterEngine3/src/animation/anim_asset.h`
- Create: `MatterEngine3/src/animation/anim_asset.cpp`
- Create: `MatterEngine3/src/animation/anim_bundle.h`
- Create: `MatterEngine3/src/animation/anim_bundle.cpp`
- Modify: `MatterEngine3/src/part_asset_v2.h`
- Modify: `MatterEngine3/Makefile`
- Test: `MatterEngine3/tests/animation_asset_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

- [ ] Write failing round-trip and corruption tests for header magic/version, schema/bake epochs, resolved hash, target ABI, pinned ozz tag, body checksum, count/offset overflow, truncated blobs, manifest mismatch, missing sibling, stale sibling, interrupted publish before manifest, animated-intent detection with no manifest, and last-good fallback.
- [ ] Add an optional tagged `ANLK` trailer to animated `.part` payloads only: version, `bundle_required = 1`, resolved hash, and 128-bit build nonce. Existing static `.part` bytes and overload behavior remain unchanged; a new load overload returns `std::optional<PartAnimationLink>`. This trailer lets a fresh provider distinguish a static part from an interrupted animated publish even when `.anim.commit` is absent.
- [ ] Define a little-endian `.anim` v1 header with ASCII magic `MANM`, format version `1`, animation schema version `1`, animation bake epoch `1`, resolved source hash, the same build nonce, target ABI tag, ozz tag hash, section table, and body checksum. Sections are rig schema, input/target schemas, graph/controller bytecode, geometry bindings per LOD, inverse bind matrices, conservative cluster bounds, ozz skeleton, and ozz clips.
- [ ] Define `.anim.commit` v1 with ASCII magic `MACM`, resolved hash, random 128-bit build nonce, `.part` body checksum, `.anim` body checksum, canonical indexed-vertex signature per LOD, vertex/influence counts per LOD, format/compiler/schema/ozz identifiers, and commit checksum.
- [ ] Add:

```cpp
std::filesystem::path cache_path_anim(const std::filesystem::path&, uint64_t resolved_hash);
std::filesystem::path cache_path_anim_commit(const std::filesystem::path&, uint64_t resolved_hash);
bool save_anim_candidate(const AnimAsset&, const std::filesystem::path&, Diagnostics&);
bool load_anim(const std::filesystem::path&, AnimAsset&, Diagnostics&);
bool publish_animation_bundle(const BundleCandidates&, const BundleIdentity&, Diagnostics&);
bool load_committed_animation_bundle(const std::filesystem::path&, uint64_t,
                                     BLASManager&, AnimAsset&, Diagnostics&);
```

- [ ] Generate the nonce before serializing either candidate. `publish_animation_bundle` validates candidate nonce agreement, fully reloads both candidates, fsyncs/flushes them, atomically replaces `.part`, then `.anim`, then writes and atomically replaces `.anim.commit` last. Readers first inspect the `.part` link trailer and manifest, then validate all three nonces/checksums before exposing the pair. Orphan candidates and sibling generations are invisible; an `ANLK` part with no valid manifest is an animated cache miss, never a static hit.
- [ ] Keep `compute_resolved_hash` source-oriented. Put compiler/schema/ozz/ABI invalidation in artifact compatibility and the manifest, so an ozz upgrade rebuilds animated artifacts without changing unrelated static `.part` identities.
- [ ] Run `run-animation-asset` and existing `run-partv2`; expect every torn/corrupt case rejected and static fixtures unchanged.
- [ ] Commit: `git add MatterEngine3 && git commit -m "feat: add coherent animation artifacts"`.

## Task A5: Add the Stateful Rig and Mirroring DSL

**Files:**

- Create: `MatterEngine3/src/script/dsl_animation.h`
- Create: `MatterEngine3/src/script/dsl_animation.cpp`
- Modify: `MatterEngine3/src/script/dsl_state.h`
- Modify: `MatterEngine3/src/script/dsl_state.cpp`
- Modify: `MatterEngine3/src/script/dsl_bindings.cpp`
- Modify: `MatterEngine3/src/script/part_base.js.h`
- Modify: `MatterEngine3/Makefile`
- Test: `MatterEngine3/tests/animation_dsl_rig_tests.cpp`

- [ ] Write failing ScriptHost tests for the approved stateful form: `beginRig()`, `root()`, `bone()`, `push()/pop()`, `atJoint()`, `radius()`, `socket()`, `mirrorBranch()`, and `endRig()`, plus illegal nesting, unbalanced stack, duplicate names, invalid parents, and calls outside a rig session.
- [ ] Add one optional `AnimationBuildBuffer` to `DslState`. Animation state includes current parent, transform cursor, radius, material, mirror plane, and a stack snapshot. Do not duplicate the existing geometry transform/material implementation; adapt it through shared helpers.
- [ ] Make `root(name, position?, rotation?)` create and select the sole root. `bone(name, endpoint, rotation?)` creates a child at a parent-local endpoint and selects it. `push()`/`pop()` save and restore the branch cursor; `radius()` changes the radius captured by the next joint; `atJoint()` selects an existing joint.
- [ ] Specify `mirrorBranch(fromRoot, toRoot, { axis, rename })` exactly: clone the complete descendant subtree; map the source root explicitly to `toRoot`; require every descendant name to contain `rename.from` exactly once and replace it with `rename.to`, or accept an explicit complete name map; reflect local translation as `S*t` and rotation as `S*R*S`; normalize and canonicalize quaternion sign; copy positive radius/scale magnitudes; mirror/rename sockets; reject missing/repeated tokens and collisions. The rig is mirrored before geometry generation, so generated triangle winding is not reversed.
- [ ] `endRig()` canonicalizes the rig and runs validation but does not serialize or publish. ScriptHost owns final compilation after all geometry and LOD work completes.
- [ ] Run `run-animation-dsl-rig` and `run-script`; expected: mirrored local matrices match `S*T*S`, rotations remain proper/canonical, generated surface normals face outward without a winding flip, and existing non-animation DSL tests remain unchanged.
- [ ] Commit: `git add MatterEngine3 && git commit -m "feat: add stateful procedural rig DSL"`.

## Task A6: Add Generated Clips, Declared Controls, and Motion Graphs

**Files:**

- Modify: `MatterEngine3/src/script/dsl_animation.h`
- Modify: `MatterEngine3/src/script/dsl_animation.cpp`
- Modify: `MatterEngine3/src/script/dsl_bindings.cpp`
- Test: `MatterEngine3/tests/animation_dsl_motion_tests.cpp`

- [ ] Write failing tests for keyframed and generated clips, loop closure, markers, declared typed inputs, declared targets, target enable/disable, external/controller ownership, fixed/frame cadence, chain inference, blend/additive nodes, controller nodes, output selection, forward references, graph cycles, and solver overlap/order.
- [ ] Implement generated clip sampling exactly: validate `duration > 0` and `sampleRate > 0`; set `segments = max(1, ceil(duration * sampleRate))`. A non-looping generator runs at normalized phase `i/segments` for `i = 0..segments`. A looping generator runs for `i = 0..segments-1`, then the compiler appends an exact byte-level copy of sample zero at `duration`. Reset the pose builder to bind pose before every callback; `at()` only selects; local-axis rotation deltas compose in call order as `bindRotation * delta`; translations are offsets from bind translation; untouched joints stay at bind pose.
- [ ] Markers use normalized source time in `[0,1)` and canonicalize by `(time, declaration_order)`. Clip names are unique; key times must be finite, monotonic after stable sort, and within duration.
- [ ] Implement graph declarations with these v1 node kinds only: `clip`, `blend1D`, `additive`, `nativeController`, and `output`. Graph evaluation order is canonical topological order. One graph has exactly one output; unused nodes are rejected.
- [ ] Implement declared control schemas only—no JS runtime object:

```javascript
const speed = anim.input("speed", { type: "float", cadence: "fixed", default: 0 });
const hand = anim.target("hand", {
  start: "upperArm.R", end: "hand.R",
  cadence: "frame", driver: "external", enabled: true,
  pole: [0, 0, 1], soften: 0.95, twist: 0,
  positionHalfLife: 0.08, rotationHalfLife: 0.06, weightHalfLife: 0.05
});
```

Controller-driven targets name exactly one declared native controller. External targets cannot be written by controllers. Both kinds resolve to public handles, but transform writes to controller-driven targets are rejected; enable, desired weight, and snap remain runtime-owned controls.
- [ ] Store the compiled start/middle/end indices, start-joint-local bend axis, animator-root-relative pole direction, `soften` in `[0,1]`, and finite twist angle. If `pole` is omitted, derive it from the non-collinear bind-pose chain plane; require an explicit pole for a collinear bind chain.
- [ ] Compile validated clips through the Ozz adapter and retain Matter marker/root metadata separately from ozz blobs.
- [ ] Run `run-animation-dsl-motion`, `run-animation-ir`, and `run-animation-ozz-adapter`.
- [ ] Commit: `git add MatterEngine3 && git commit -m "feat: compile procedural clips and motion graphs"`.

## Task A7: Bind Final Geometry to the Rig

**Files:**

- Create: `MatterEngine3/src/render/indexed_part_geometry.h`
- Create: `MatterEngine3/src/render/indexed_part_geometry.cpp`
- Modify: `MatterEngine3/src/render/raster_mesh.h`
- Modify: `MatterEngine3/src/render/raster_mesh.cpp`
- Create: `MatterEngine3/src/animation/animation_binding_bake.h`
- Create: `MatterEngine3/src/animation/animation_binding_bake.cpp`
- Modify: `MatterEngine3/src/script/dsl_animation.*`
- Modify: `MatterEngine3/Makefile`
- Test: `MatterEngine3/tests/animation_binding_tests.cpp`

- [ ] Write failing tests for implicit skin generation, explicit geometry inside a skin scope, rigid bone segments, socket attachments, per-LOD influences, mirrored winding, stable vertex signatures, four-weight normalization, deterministic ties, and rejection of nested animated attachments.
- [ ] Extract one deterministic `build_indexed_part_geometry(...)` implementation from `raster_mesh`. Both the Vulkan mesh builder and animation binder must consume this exact indexed vertex order. Compute a 64-bit signature over position, normal, material/surface identity, indices, and LOD boundaries.
- [ ] Implement `skin(rig, { joints, radiusScale, falloffScale, voxelSize })` by iterating selected unclaimed bind-pose parent-child segments, emitting one unioned implicit field of tapered capsules plus branch/end joint spheres through the existing voxel/SurfaceLib path. Geometry authored through `bind()`/`withBinding()` joins the selected deformable binding set.
- [ ] Compute final-LOD weights only after all simplification/remeshing. For each parent-child segment, find closest parameter `t`, interpolated radius `r`, and `q = clamp(1 - distance / (falloffScale * r), 0, 1)`. Use `q*q*(3-2*q)` and accumulate `(1-t)` to the parent and `t` to the child. Include root/end spheres. Keep the four highest totals, breaking ties by lower joint index; normalize to UNORM16 with the residual assigned to the largest weight. If all totals are zero, bind 100% to the nearest joint, tie-breaking by joint index.
- [ ] Implement `segments(rig, options?)` as one rigid binding record for every selected unclaimed segment, with its owning joint and bind offset. Implement `attach(rig, jointOrSocket, module, options?)` as a child part hash plus local transform; reject a child with a committed `.anim` bundle in v1. `bind()`/`withBinding()` may select exceptional regions, but overlapping primary bindings fail unless explicitly decorative.
- [ ] Precompute inverse bind matrices and per-LOD, per-cluster, per-influencing-joint conservative AABBs in joint bind-local space for Phase C culling.
- [ ] Run `run-animation-binding`, `run-animation-dsl-rig`, and the existing raster-mesh tests. Expected: signatures match between bake and render paths for every LOD.
- [ ] Commit: `git add MatterEngine3 && git commit -m "feat: bind generated geometry to rigs"`.

## Task A8: Integrate Animated Bundle Baking and Loading

**Files:**

- Modify: `MatterEngine3/src/script/script_host.h`
- Modify: `MatterEngine3/src/script/script_host.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.*`
- Modify: `MatterEngine3/src/provider/resolve_cache.*`
- Modify: `MatterEngine3/src/render/part_store.*`
- Create: `MatterEngine3/src/animation/animation_asset_store.h`
- Create: `MatterEngine3/src/animation/animation_asset_store.cpp`
- Modify: `MatterEngine3/Makefile`
- Test: `MatterEngine3/tests/animation_bake_integration_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

- [ ] Add a failing end-to-end test that bakes one static script and one animated script; restarts the provider; loads both; corrupts each sibling/manifest in turn; and verifies static cache reuse plus last-good animated reload behavior.
- [ ] Extend `BakeResult` with optional `written_anim_path` and `written_commit_path`, plus one diagnostic stream. Do not change `resolved_hash` semantics.
- [ ] For an animated build, ScriptHost writes uniquely named candidates, builds the canonical indexed geometry, computes bindings, compiles ozz blobs, validates both complete candidates, and invokes `publish_animation_bundle`. For a static build, preserve the existing `.part` atomic path exactly.
- [ ] Provider cache-hit logic is: a header-compatible `.part` without `ANLK` is static; a `.part` with `ANLK` succeeds only through `load_committed_animation_bundle`. Missing/stale/incompatible animated siblings trigger one rebuild and never downgrade silently to bind-pose static content. If rebuilding fails during live reload, the already loaded in-memory pair remains active; a fresh load may expose the `.part` only as an explicit failed-status bind-pose preview, not as a successful static asset.
- [ ] `PartStore` owns the loaded `.part`; `AnimationAssetStore` owns immutable `.anim` assets and deduplicates them by `(resolved_hash, commit_nonce)`. The same pair identity is checked when creating an animator later.
- [ ] Add fault injection immediately before each of the three replacements and prove that only the final manifest makes a generation visible.
- [ ] Run:

```bash
make -C MatterEngine3/tests run-animation-bake run-script run-partv2 GRAPHICS=GRAPHICS_API_OPENGL_43
```

Expected: animated bake/load succeeds, all injected torn publishes preserve the prior generation, and static cache tests are byte-for-byte unchanged.
- [ ] Commit: `git add MatterEngine3 && git commit -m "feat: publish procedural animation bundles"`.

## Phase A Gate

- [ ] Run `run-animation-ozz`, `run-animation-ozz-adapter`, `run-animation-ir`, `run-animation-asset`, `run-animation-dsl-rig`, `run-animation-dsl-motion`, `run-animation-binding`, `run-animation-bake`, `run-script`, and `run-partv2` from a clean build.
- [ ] Inspect one generated `.anim.commit` and verify its `.part` checksum, `.anim` checksum, ozz tag, ABI tag, compiler/schema epochs, nonce, and every LOD vertex signature.
- [ ] Stop Phase B if the DSL can emit declarations that the serialized IR cannot represent losslessly.

---

# Phase B — Native Runtime, Controllers, IK, and Rigid Rendering

## Task B1: Add Public Handles and Generational Runtime Stores

**Files:**

- Create: `MatterEngine3/include/matter/animation.h`
- Create: `MatterEngine3/src/animation/animation_store.h`
- Create: `MatterEngine3/src/animation/animation_store.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/animation_store_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

- [ ] Write failing tests for asset deduplication, instance allocation/removal/reuse, stale handles, wrong-type writes, wrong-cadence writes, default values, missing names, disabled targets, controller-owned transform rejection, hard-cap failures, exact mutable-memory accounting, and live-reload migration/reset rules.
- [ ] Define public POD types `AnimationInputHandle`, `AnimationTargetHandle`, `AnimatorInstanceHandle`, `Animator`, `AnimationStatus`, `DesiredRootMotion`, `AnimationMarkerEvent`, and `AnimationRuntimeStats`. Every handle contains slot index, generation, schema index, value type, and cadence; invalid is all-ones index.
- [ ] Define `AnimationService` lookup plus typed writes only:

```cpp
AnimationInputHandle input(AnimatorInstanceHandle, std::string_view name) const;
AnimationTargetHandle target(AnimatorInstanceHandle, std::string_view name) const;
bool set(AnimationInputHandle, bool);
bool set(AnimationInputHandle, float);
bool set(AnimationInputHandle, const Float3&);
bool set(AnimationInputHandle, const Quaternion&);
bool set(AnimationInputHandle, const AnimationTransform&);
bool set_symbol(AnimationInputHandle, uint32_t declared_symbol);
bool set_enabled(AnimationTargetHandle, bool);
bool set_weight(AnimationTargetHandle, float);
bool set_transform(AnimationTargetHandle, const AnimationTransform&);
bool snap(AnimationTargetHandle);
```

- [ ] Use a `DynamicInstanceSlots`-style free list and generation counters. Asset data is immutable/deduplicated by `(resolved_hash, commit_nonce)`; instance data owns only graph state, controller state, previous/current fixed controls, frame controls, smoothed targets, sample contexts, and pose scratch.
- [ ] Generation-swap an asset only after the new bundle has loaded fully. Migrate an input only when name, type, and cadence match. Migrate a target only when name, driver kind, cadence, and compiled joint chain match. Reset changed/new declarations to new defaults, invalidate all old handles, and leave the previous generation active on any load/migration failure.
- [ ] Enforce 4096 instances and 64 MiB mutable state before allocation. Allocation failure sets `AnimationStatus::BudgetExceeded` and leaves the entity at bind pose.
- [ ] Run `run-animation-store`; expected: stale handles never affect a reused slot and all rejected writes leave bytes unchanged.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: add native animation handles and stores"`.

## Task B2: Implement Graph and Pose Evaluation

**Files:**

- Create: `MatterEngine3/src/animation/animation_evaluator.h`
- Create: `MatterEngine3/src/animation/animation_evaluator.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/animation_evaluator_tests.cpp`

- [ ] Write failing tests for time advance, wrap/clamp, blend1D endpoints/interior, additive reference pose, deterministic node order, paused instances, disabled instances, fixed snapshot interpolation, previous/current model poses, and graph/controller node budget overflow.
- [ ] Define immutable output:

```cpp
struct AnimationPoseSnapshot {
  AnimatorInstanceHandle instance;
  uint64_t fixed_tick;
  uint64_t frame_serial;
  ArrayView<AnimationTransform> local_pose;
  ArrayView<Mat4f> model_pose;
  ArrayView<Mat4f> previous_model_pose;
  ArrayView<Mat4f> skin_palette;
  ArrayView<Mat4f> previous_skin_palette;
};
```

`ArrayView<T>` is an internal `{ const T* data; uint32_t count; }` read-only view. The store owns backing memory until the next completed frame publish; renderer readers see a stable front buffer.
- [ ] Evaluate graph nodes in serialized topological order. Sample ozz clips into adapter-owned scratch, blend normal layers, apply additive layers, then run one local-to-model pass. Compute skin matrices as `model_pose * inverse_bind_model`; retain current and previous palettes for motion vectors.
- [ ] Fixed controls interpolate by accumulator alpha: numbers/vectors linearly, quaternions by shortest-path normalized slerp, transforms with linear translation/scale plus quaternion slerp, and booleans/symbols retain the previous value until alpha reaches one. Frame controls use the once-per-frame sampled value with no fixed interpolation.
- [ ] Budget evaluation in stable priority order `(visibility class, explicit priority descending, instance slot ascending)`. Over-budget instances reuse their last complete snapshot; never publish a partial pose.
- [ ] Run `run-animation-evaluator` and `run-animation-store`.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: evaluate native animation graphs"`.

## Task B3: Install Explicit ECS Scheduling and Frame Interpolation

**Files:**

- Create: `MatterEngine3/src/animation/animation_systems.h`
- Create: `MatterEngine3/src/animation/animation_systems.cpp`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/include/matter/ecs.h`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/animation_systems_tests.cpp`

- [ ] Write a phase-trace test that records input sampling, controller evaluation, root-motion publication, physics, world queries, fixed snapshot publish, frame sampling, interpolation, IK, and pose publish. First confirm current runtime cannot produce interpolation alpha.
- [ ] Add `double interpolation_alpha` to `Runtime::TickResult`, defined as `clamp(accumulator / fixed_dt, 0, 1)` after fixed stepping. It is presentation-only and does not alter simulation time.
- [ ] Register systems with exact order:

```text
FixedPreUpdate: rotate previous/current fixed state; sample fixed API writes; advance clip/controller clocks
FixedUpdate:    sample root channels; publish DesiredRootMotion; emit fixed marker/completion events
PrePhysics:      authority system consumes DesiredRootMotion (outside animation)
Physics:         Box3D step
PostPhysics:     pull physics transforms and finish hierarchy propagation
FixedPostUpdate: run fixed controllers and their world queries once; smooth fixed targets;
                 publish fixed controller/target snapshot
FrameUpdate:     sample frame API writes once; interpolate fixed state; evaluate presentation graph;
                 solve frame targets/IK; publish pose snapshot
```

- [ ] Fixed target smoothing uses fixed `dt`; frame target smoothing uses render `dt`; neither is applied twice. Writes after the relevant sampling boundary wait for the next matching boundary.
- [ ] Add a double-buffered `AnimationPoseSnapshotStore` independent of Flecs so render adapters consume snapshots by instance handle and frame serial.
- [ ] Run `run-animation-systems` plus existing `run-ecs` and `run-physics`. Expected phase trace matches exactly for zero, one, and multiple fixed steps per frame.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: schedule fixed and frame animation evaluation"`.

## Task B4: Add Markers, Root Motion, Checkpoints, and World Queries

**Files:**

- Create: `MatterEngine3/src/animation/animation_world_queries.h`
- Create: `MatterEngine3/src/animation/animation_world_queries.cpp`
- Modify: `MatterEngine3/src/animation/animation_evaluator.*`
- Modify: `MatterEngine3/src/animation/animation_systems.*`
- Modify: `MatterEngine3/src/ecs/simulation_control.*`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/animation_simulation_tests.cpp`

- [ ] Write failing tests for marker crossing forward/wrap/reverse, root-motion extraction across loop boundaries, exactly-once consumption, moving-root world targets, checkpoint restore/replay, query caps/priorities, masks, misses, and query execution after physics.
- [ ] Emit a marker event once for each crossed half-open interval; use `(previous_time, current_time]` forward and `[current_time, previous_time)` reverse, splitting loop wraps. Sort simultaneous emissions by instance slot, clip/node order, marker time, then declaration order.
- [ ] Extract root translation/rotation delta from clip root tracks before in-place removal. Blend clip deltas with the same normalized clip weights; additive clips add delta from their reference pose. Native controllers cannot author root motion in v1. Publish one `DesiredRootMotion` per fixed tick; authority marks it consumed.
- [ ] Define `AnimatorCheckpoint` with asset identity, graph times/states, fixed and frame input values, target desired/evaluated transform and weight state, controller serializable bytes, marker cursors, and fixed snapshots. Extend SimulationControl's whitelist, size accounting, capture, restore, and deterministic replay tests. Reconstruct transient ozz contexts, pose scratch, GPU state, and cached handles after restore.
- [ ] Define renderer-agnostic interface:

```cpp
class AnimationWorldQueries {
 public:
  virtual bool ray_cast(const Float3& origin, const Float3& direction,
                        float max_distance, uint64_t mask, WorldRayHit& out) const = 0;
};
```

Provide a Box3D adapter that calls `physics_ray_cast`. `FixedPostUpdate` evaluates fixed controllers in deterministic `(declared query priority, animator handle, controller order)` order against the post-step physics world. Admit at most 2048 calls; every excess request receives an explicit no-hit result and increments the overflow counter before fixed snapshot publication. Frame controllers cannot query the world.
- [ ] Define public desired target transforms as world-space. Convert them to animator-root-relative/model space only at the target's evaluation boundary using the current post-authority/post-physics `WorldTransform`, never the transform that existed when the API write occurred.
- [ ] Run `run-animation-simulation`, `run-simulation-control`, and `run-physics`.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: integrate animation with simulation state"`.

## Task B5: Add Native Controllers, Target Smoothing, and IK

**Files:**

- Create: `MatterEngine3/src/animation/animation_controllers.h`
- Create: `MatterEngine3/src/animation/animation_controllers.cpp`
- Create: `MatterEngine3/src/animation/animation_targets.h`
- Create: `MatterEngine3/src/animation/animation_targets.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/animation_controller_tests.cpp`
- Test: `MatterEngine3/tests/animation_ik_tests.cpp`

- [ ] Write failing tests for controller registration/schema mismatch, gait contacts over flat/stepped/missing ground, deterministic query order, target enable/disable, positional and rotational convergence across frame rates, one-driver arbitration, inferred two-bone chain, unreachable target clamping, pole vector, solver ordering, and downstream model-space freshness.
- [ ] Define a native-controller registry keyed by serialized stable type ID. Each factory validates its parameter blob and reports fixed/frame state byte counts before instance allocation. Controllers access declared inputs/owned targets and `AnimationWorldQueries`; they cannot access renderer or Flecs.
- [ ] Implement v1 `GaitController`: fixed cadence; alternate normalized leg phases; cast downward from predicted foot positions; lock a planted foot while its ground remains within configured step height/slope; drive swing height by a deterministic cubic curve; release on miss; emit owned target transforms. All decisions derive from fixed inputs, fixed `dt`, checkpointed controller state, and query results.
- [ ] Smooth translation/scale with `a = 1 - exp2(-dt / halfLife)` and rotation by shortest-path normalized slerp; smooth desired weight independently and clamp it to `[0,1]`. A zero half-life snaps. Disable sets desired weight to zero but preserves desired transform; the weighted solver continues during fade-out. Reenable sets desired weight to one and approaches the latest desired transform. `snap()` copies desired transform/weight to evaluated state at the next declared cadence boundary.
- [ ] Resolve chain indices once from inclusive start/end ancestry. V1 accepts exactly three joints/two segments, feeds target position, compiled mid-axis, current root-relative pole, soften, twist, and evaluated weight to ozz in model space, applies returned local corrections, and reruns local-to-model over the affected subtree. Then match the end-joint target orientation at evaluated weight in local space and reconvert that subtree once more. The target abstraction exposes no solver choice; longer-chain support may be added behind the same declaration later.
- [ ] Fixed controllers and fixed targets complete in `FixedPostUpdate`; frame external targets solve in `FrameUpdate`. Reject overlapping writable target chains in v1 rather than attempting arbitrary ordering.
- [ ] Run `run-animation-controller`, `run-animation-ik`, `run-animation-systems`, and checkpoint replay tests.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: add procedural controllers and IK targets"`.

## Task B6: Expand Rigid Bindings Through the Dynamic Instance Lane

**Files:**

- Modify: `MatterEngine3/src/render/dynamic_instance_slots.h`
- Modify: `MatterEngine3/src/render/dynamic_instance_slots.cpp`
- Modify: `MatterEngine3/src/render/dynamic_scene_bridge.h`
- Modify: `MatterEngine3/src/render/dynamic_scene_bridge.cpp`
- Create: `MatterEngine3/src/render/animation_rigid_bridge.h`
- Create: `MatterEngine3/src/render/animation_rigid_bridge.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/animation_rigid_bridge_tests.cpp`

- [ ] Write failing tests for one ordinary entity, many rigid segments, attachment sockets, slot stability, removal/reuse, entity-generation reuse, missing snapshots, stale asset generations, and per-frame raster/TLAS transform updates.
- [ ] Replace entity-only dynamic identity with `DynamicInstanceKey { uint64_t entity_id; uint32_t entity_generation; uint32_t binding_index; }`. Ordinary instances use binding index `0`; animated rigid/attachment bindings use indices `1..N` in serialized order. Hash/equality must use all fields.
- [ ] `AnimationRigidBridge` reads only `AnimationPoseSnapshotStore` and immutable asset bindings. It emits desired `(key, part_hash, world_transform, previous_world_transform, flags)` records; it never queries Flecs or calls the animation evaluator.
- [ ] `DynamicSceneBridge` remains the sole owner of `DynamicInstanceSlots`. It gathers ordinary ECS records, asks `AnimationRigidBridge` to expand animator records, reconciles the combined sorted key set, and forwards existing add/change/remove batches.
- [ ] Segment world transforms are `entity_world * joint_model * rigid_bind_offset`; attachments use `entity_world * joint_model * socket_local * attachment_local`. Previous transforms use the matching previous pose and previous entity transform.
- [ ] Prove the existing Vulkan dynamic lane updates raster instances and rebuilt TLAS instances without BLAS changes. Do not add an RT proxy abstraction or BLAS refit flags.
- [ ] Run `run-animation-rigid`, `run-dynamic-slots`, `run-dynamic-bridge`, and `run-vk-scene-renderer`.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: render articulated rigid animation"`.

## Phase B Gate

- [ ] Run all Phase A tests plus `run-animation-store`, `run-animation-evaluator`, `run-animation-systems`, `run-animation-simulation`, `run-animation-controller`, `run-animation-ik`, `run-animation-rigid`, `run-ecs`, `run-physics`, `run-simulation-control`, `run-dynamic-slots`, and `run-dynamic-bridge` from a clean build.
- [ ] Run a headless deterministic replay for 10,000 fixed ticks, checkpoint at tick 4,000, restore twice, and compare marker sequence, root-motion deltas, controller bytes, target transforms, and fixed pose checksums.
- [ ] In MatterViewer, display a multi-segment machine driven by a generated clip, a socket attachment, and an externally positioned target. Confirm raster and TLAS transforms update while BLAS handles remain unchanged.

---

# Phase C — Vulkan Compute Skinning, Animated Bounds, and Budgets

## Task C1: Define GPU Influence, Palette, and Work-Queue Contracts

**Files:**

- Create: `MatterEngine3/src/render/vk_animation_types.h`
- Create: `MatterEngine3/src/render/vk_animation_skinning.h`
- Create: `MatterEngine3/src/render/vk_animation_skinning.cpp`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vk_animation_skinning_tests.cpp`

- [ ] Write failing CPU-side layout tests for struct sizes/offsets, UNORM16 decode sum, palette offsets, queue sorting, visible-only submission, per-frame arena wrap protection, current/previous allocation pairing, and cap rejection.
- [ ] Define shader-shared POD contracts with explicit 32-bit fields and static assertions:

```cpp
struct VkSkinInfluence { uint16_t joint[4]; uint16_t weight[4]; }; // 16 bytes
struct VkSkinJoint { Matrix4 position; Matrix4 normal; };          // 128 bytes
struct VkSkinWorkItem {
  uint32_t source_vertex, influence, vertex_count, palette;
  uint32_t output_current, output_previous, instance_slot, flags;
};                                                               // 32 bytes
```

`normal` is the inverse-transpose joint skin matrix, so nonuniform authored scale does not corrupt normals.
- [ ] Add immutable per-asset influence buffers and per-frame-in-flight palette, work-item, current-output, and previous-output arenas. Arena slices remain alive until their frame fence signals; never overwrite in-flight storage.
- [ ] Use the previous completed visibility snapshot only for pose-rate budgeting. Build the skin work queue from the current frame's animated-bound culling result, so newly visible instances cannot become permanently starved. Sort accepted work by `(render priority descending, distance bucket ascending, instance slot ascending, LOD ascending)`. Enforce 256 work items and 2,000,000 output vertices before allocating any slice.
- [ ] Upload current and previous palettes from the same immutable pose snapshot. A newly visible/no-history instance uses current palette for both outputs and sets a history-invalid flag.
- [ ] Run `run-vk-animation-skinning`; expected: every offset matches shader ABI and overflow produces a deterministic bind-pose/last-pose fallback without partial queue entries.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: add Vulkan skin work queues"`.

## Task C2: Implement Compute Skinning and Raster Consumption

**Files:**

- Create: `MatterEngine3/shaders_vk/animation_skin.comp`
- Modify: `MatterEngine3/shaders_vk/raster.vert`
- Modify: `MatterEngine3/src/render/vk_animation_skinning.*`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.*`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vk_animation_skin_compute_tests.cpp`

- [ ] Write a CPU reference skinner test covering identity bind pose, one-joint rotation, four-weight blend, normalized normal, nonuniform scale, previous-pose output, invalid joint rejection, zero-weight rejection, and dispatch rounding at 1/63/64/65 vertices.
- [ ] Add `animation_skin.comp` with local size 64. For each accepted work item and local vertex, decode four UNORM16 weights, blend current position matrices, blend inverse-transpose normal matrices, normalize the result, copy tint/surface/material fields, and repeat position skinning with the previous palette into the previous-output arena.
- [ ] Add the shader SPIR-V to both explicit Makefile lists and embedded-shader generation. Add descriptor layouts for source vertices, influences, palettes, work items, and both output buffers.
- [ ] Dispatch skinning after palette/work upload and before depth/gbuffer raster reads. Insert explicit compute-write to vertex-input/shader-read barriers. Static meshes retain their existing vertex path.
- [ ] Extend raster vertex selection with a per-instance skinned flag and output offsets. Current clip-space position reads the current arena; motion-vector previous position reads the previous arena with the previous view/projection path. Do not approximate deformation velocity with root transform alone.
- [ ] Compare GPU output readback against the CPU reference within `1e-5` for positions and `1e-4` for normals; run under Vulkan validation with zero errors.
- [ ] Run `run-vk-animation-skin-compute`, `run-vk-animation-skinning`, and `run-vk-scene-renderer`; build MatterViewer.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: compute skin visible Vulkan meshes"`.

## Task C3: Add Conservative Animated Bounds to Culling

**Files:**

- Create: `MatterEngine3/src/render/vk_animation_bounds.h`
- Create: `MatterEngine3/src/render/vk_animation_bounds.cpp`
- Modify: `MatterEngine3/src/render/vk_animation_skinning.*`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.*`
- Modify: `MatterEngine3/shaders_vk/cull.comp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Test: `MatterEngine3/tests/vk_animation_bounds_tests.cpp`

- [ ] Write failing tests that animate vertices beyond bind bounds, compare the conservative bound against brute-force skinned vertices for random deterministic poses, switch LODs, lose history, and verify ordinary/static clusters retain existing bounds.
- [ ] For each visible skinned cluster, transform the eight corners of every serialized joint-local cluster AABB by the current joint model matrix and union them in object space. Union current and previous results for temporal stability. Empty influence sets are invalid assets.
- [ ] Upload dynamic cluster AABBs keyed by `(instance_slot, cluster_index, lod)`. Add a cluster flag/offset causing `cull.comp` to select the dynamic AABB; static clusters continue using embedded immutable bounds.
- [ ] Compute dynamic bounds from each accepted pose before cluster culling; do not first reject an animated instance using its smaller bind-pose bounds. Use the unioned current dynamic cluster bounds as the instance-level coarse bound. Budget-frozen instances retain the matching last-complete dynamic bounds.
- [ ] On pose-budget fallback, use the bound matching the last complete pose. On missing/corrupt bound data, disable occlusion culling for that instance and use its conservative asset-level bound; never use a known-stale smaller bound.
- [ ] Run `run-vk-animation-bounds`, culling regressions, and Vulkan validation. Expected: every brute-force skinned point lies inside the chosen AABB.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: cull skinned animation with dynamic bounds"`.

## Task C4: Enforce Budgets, Pose LOD, RT Fallback, and Acceptance Scenes

**Files:**

- Create: `MatterEngine3/src/animation/animation_budget.h`
- Create: `MatterEngine3/src/animation/animation_budget.cpp`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/src/animation/animation_evaluator.*`
- Modify: `MatterEngine3/src/render/vk_animation_skinning.*`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.*`
- Create: `MatterViewer/animation_debug_overlay.h`
- Create: `MatterViewer/animation_debug_overlay.cpp`
- Modify: `MatterViewer/main.cpp`
- Modify: `MatterViewer/ui.h`
- Modify: `MatterViewer/ui.cpp`
- Modify: `MatterViewer/Makefile`
- Create: `MatterEngine3/examples/world_demo/objects/AnimatedRigGallery.js`
- Test: `MatterEngine3/tests/animation_budget_tests.cpp`
- Test: `MatterEngine3/tests/animation_phase_c_acceptance_tests.cpp`

- [ ] Write failing tests for every hard/default limit, stable priority selection, visibility hysteresis, pose-rate LOD transitions, skipped-frame interpolation, work/vertex overflow, last-complete-pose fallback, bind-pose fallback before first evaluation, and zero deforming-BLAS update calls.
- [ ] Centralize all limits in `AnimationBudgetConfig`; validate hard asset limits at bake/load and runtime limits at allocation/evaluation/submission. Expose counters and fallback reasons through `AnimationRuntimeStats` and renderer diagnostics.
- [ ] Implement cosmetic pose LOD tiers with configurable target rates `Hz60`, `Hz30`, `Hz15`, and `Frozen`, scheduled from accumulated presentation time rather than render-frame divisors. Tier selection uses the prior completed visibility snapshot, distance bands, explicit priority, and two-boundary hysteresis. Newly visible instances receive `Hz60` for two frames. Returning to a faster tier resamples current graph time rather than replaying skipped poses. Simulation-relevant fixed clocks, markers, root motion, and fixed controllers continue every fixed tick.
- [ ] Keep skinned RT geometry on the immutable bind-pose `.part` BLAS. Assert acceleration-structure builds use build-once mode and never set update/refit flags for skinned deformation. Raster is exact animated pose; RT is documented bind-pose fallback until a separately specified later phase.
- [ ] Author `AnimatedRigGallery.js` containing: a soft voxel creature built from its rig with generated idle/walk clips, blend1D speed control, physics-grounded gait, external hand/look targets, and clip root motion; a segmented rigid machine using TLAS-transform articulation; a socket attachment; mirrored limbs; and at least two LODs with post-LOD weights. Keep it below 128 joints, 50,000 LOD0 skinned vertices, 32 rigid/attachment instances, eight targets, and 32 graph nodes.
- [ ] Add an observational debug overlay fed only by the committed `.anim` plus immutable pose snapshots. In the isolated part/gallery preview it draws bones, joint axes, radius envelopes, sockets, target transforms, IK chains, conservative bounds, and optional skin weights. Overlay primitives are transient, never enter `.part`, BLAS, culling bounds, or animation checkpoints, and require no editor event-hub integration.
- [ ] Add a headless acceptance test that bakes/reloads the gallery, evaluates 1,000 fixed ticks at two render frame-rate patterns, compares fixed checksums/markers/root motion, submits rigid and skinned outputs, exercises all pose LODs, and remains within all default budgets.
- [ ] Run all animation tests, affected engine regressions, a clean MatterViewer build, Vulkan validation, and the manual gallery. Record CPU animation time, GPU skin time, work items, skinned vertices, evaluated joints, query count, and fallback count in the test output; acceptance requires zero validation errors and zero fallback at gallery defaults.
- [ ] Commit: `git add MatterEngine3 MatterViewer && git commit -m "feat: complete Vulkan procedural animation runtime"`.

## Phase C / Release Gate

- [ ] Delete build outputs, rebuild ozz, MatterEngine3 tests, and MatterViewer from clean state.
- [ ] Run every `run-animation-*` target plus `run-ecs`, `run-physics`, `run-simulation-control`, `run-script`, `run-partv2`, `run-dynamic-slots`, `run-dynamic-bridge`, and `run-vk-scene-renderer`.
- [ ] Bake the gallery twice from identical source/params and compare `.part`, `.anim`, and manifest semantic fields. The nonce may differ; all payload bytes, checksums, signatures, and identifiers must match.
- [ ] Repeat interrupted-publish fault injection and last-good reload with a real gallery bundle.
- [ ] Inspect a GPU capture: skin dispatch precedes raster consumption, barriers are present, only visible work is dispatched, dynamic bounds feed culling, and BLAS resources remain bind-pose/build-once.
- [ ] Confirm public headers contain no ozz, QuickJS, Flecs implementation, or Vulkan types and that runtime animation performs no file I/O or JavaScript execution.
- [ ] Update the approved design document only if implementation discovers a contract change; do not silently diverge.

## Explicitly Deferred Beyond Phase C

- Runtime/gameplay JavaScript input and target writes.
- Editor event-system bindings, graph UI, gizmos, undo/redo, and live authoring UX.
- Exact deforming ray-tracing geometry, BLAS refit/rebuild, or animation RT proxies.
- Nested animated attachments.
- Animation-driven physics shape deformation or ragdolls.
- Persisted save-game serialization beyond SimulationControl checkpoints.
- General CCD/FABRIK or arbitrary-length IK chains.
- A general joint-constraint authoring language beyond the v1 two-bone solver's pole, softening, weight, and stable unreachable-target behavior.

## Definition of Done

Phases A-C are complete only when an authored JavaScript file can deterministically generate a rig, final voxel/mesh geometry, per-LOD bindings, generated clips, a native motion graph, declared inputs and targets; publish and reload a coherent `.part`/`.anim` pair; run fixed controllers, root motion, Box3D-grounded gait, interpolation, and IK through native handles; render both articulated rigid parts and compute-skinned meshes in Vulkan with conservative animated culling and correct previous-pose motion data; remain inside the stated budgets; preserve static parts and last-good reload; and make no claim of exact animated ray tracing.
