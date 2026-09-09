# RiverFloatLab character-controller integration

Date: 2026-08-30. Status: completed implementation record; all four tasks individually reviewed through `a8166d78`, with [bounded RiverFloatLab acceptance](../../../findings/river-character-controller-integration-acceptance-2026-08-30.md). This is not acceptance of a full river playtest or the unfinished water optics.

Final cross-task review is closed after the independently approved numerical
safety fix `0440d8b9`; post-fix native integration tests and the PhysX-enabled
editor build passed. The linked acceptance record distinguishes those checks
from the original two-process screenshot evidence.

## Goal and decision

Integrate the completed character-controller feature into the fluid branch by a selective semantic port, then demonstrate fixed-step walking, grounding, jumping, Pause/Step/Resume, and Stop/restore in RiverFloatLab. The player is an authored root entity whose transform is owned by a query-driven capsule mover. It queries the existing Box3D world, including its installed engine-wide terrain collision. It is not a rigid body and does not own a separate collider.

This is a four-task integration, not a redesign of terrain collision or river physics. The implementation plan is [2026-08-30-river-character-controller-integration.md](../plans/2026-08-30-river-character-controller-integration.md).

## Provenance and integration boundary

The inspected fluid HEAD was `bf300565cbf64877306950cfde61fe985149f537` on `codex/dualsphysics-fluid-spike`. The controller source is local `feature/character-controller` and tag `premerge-main`, both at `9974457732adc8a24641fcca72678ae564a1cca0`. Local `main` at `cc6adc812db8db1f67d55aa050a2b10aa7745bbd` contains that tip as its first parent. `origin/main` at `84408b3c01b41d834c86d8b1330d920e43c968f9` is not the source. Common ancestor: `1110de164f4cf85179118772b77c2a269208a025`.

All eight feature-side commits were absent by ancestry and patch equivalence (`git cherry`); the branch comparison was 190 fluid-side commits and eight feature-side commits. Use these as read-only source material:

| Commit | Original contribution | Integration decision |
| --- | --- | --- |
| `6c866c2dd471e2a22e89983936ae0ff812e6f188` | Controller design | Historical intent only; this design governs the port. |
| `8976640c9a8f8add177f6f5e17111a224a8bbd06` | M0 components and fixed-step system | Port with Runtime-owned registration and validation. |
| `e9ffc3775ac8b9e808382546e20430f1b869a6fa` | M1 world ray and old terrain collider | Do not port old terrain installation. Implement only private mover queries against the current world. |
| `e1f06e01df4dfec2ca0807c5c40af81e872a3565` | Stream attach/evict hooks | Exclude. |
| `3b6c082a4c7abc79e72b80ee06ca18eabea0c25e` | M2 collide/solve/cast mover and slope behavior | Port behavior onto installed triangle meshes. |
| `4c57430b030b844867f66ccd0206aae455ef1d26` | Old volumetric streaming colliders | Exclude. |
| `051d3ed95c3dabb0b347811bf7ffdea3219b67ed` | M3 editor walking | Port input/follow concepts; exclude anonymous player spawning and render-ray safety floor. |
| `9974457732adc8a24641fcca72678ae564a1cca0` | M4 authoring, snapshots, independent intents | Port while preserving the fluid branch's snapshot fields. |

The current branch already has static Box3D terrain installation (`1b32db0f2c0a6bea46ef926a34bbc8a9ec46972f`), the Ready/install gate (`9c9194664041335c98a54b2f41d3d69e3d35b510`), RiverFloatLab ravine authoring (`416649a254b18443d0fa5994be65a9f090e537cc`), and overlay identity validation (`75b0851e42bd1f21097d6f3dae8238f3a3dd35d5`). Verify each hash before execution; preserve the installed terrain architecture described in `docs/completed/superpowers/specs/2026-08-24-engine-wide-terrain-collision-design.md`.

Read-only three-way inspection found textual conflicts in engine/test Makefiles, `physics_context.cpp`, `scene_registry.cpp/.h`, and `simulation_control.cpp/.h`. Automatically mergeable editor/runtime changes also have semantic conflicts. Consequently, do not cherry-pick this range or copy historical files wholesale. Do not introduce `terrain_collider_build.*`, streamed collider attachment, old heightfield test fixtures, or a render/analytic floor.

## Runtime ownership and interfaces

New `MatterEngine3/include/matter/character.h` owns `matter::character::CharacterController`, `MoveIntent`, and `CharacterModule`. `MatterEngine3/src/ecs/character_systems.cpp` owns the fixed-step system. `ecs_runtime.cpp` imports the module once after physics registration and before building the pipelines. It is available in every Runtime, not registered by the editor after a bake.

Authored controller fields are radius, total height, move speed, slope limit, ground-snap/step-height allowance, and jump speed. Runtime fields are velocity, grounded, fixed-tick count, consumed jump-latch count, and actually launched jump count. `MoveIntent` contains a horizontal direction, jump latch, and sprint flag; it is runtime-only and is added automatically when a controller is added. Removal of a controller also removes its intent.

The public mover boundary in `matter/physics.h` is `bool physics_move_character(flecs::world&, const CharacterMoveInput&, CharacterMoveOutput&)`. Its implementation delegates to `PhysicsContext::move_character`; no Box3D identifiers escape this boundary. Inputs contain position, velocity, desired horizontal velocity, gravity, capsule radius/half-segment, fixed dt, slope cosine, step height, and category mask. Output contains position, velocity, ground normal, and grounded. The call is owner-thread-only, outside Box3D stepping. Invalid calls return false without mutating the supplied output or any physics body.

All mover casts, contact-plane collection, and support rays apply the same filter: non-sensor static bodies allowed by the category mask. Installed terrain tiles and authored static boulders participate. Dynamic/kinematic crates, rafts, characters, sensors, and water do not provide support or blocking in this slice. This explicit restriction avoids implying moving-platform riding or one-sided push dynamics. It does not change those bodies' physics filters or motion.

The character system runs in `ecs::PrePhysics` on valid fixed pipeline ticks. Existing WorldSession tick gating supplies Ready-after-configured-collision-installation semantics; do not add a second hidden Ready singleton requirement to the controller or standalone Runtime tests. A world intentionally configured without terrain can be Ready and the mover then falls normally. The system reads `PhysicsSettings.gravity`. It never applies forces, creates a body, or writes `PhysicsVelocity`. `LocalTransform.translation` and controller runtime state are its only movement outputs. The existing order `PrePhysics -> PhysicsReconcile -> RiverFloatForces -> PhysicsPush -> Physics -> PhysicsPull -> PostPhysics` remains intact.

## Movement contract

1. Validate finite position, velocity, gravity, intent, dt, and configuration. Require dt > 0, radius > 0, height >= 2 * radius, nonnegative speed/step height/jump speed, and slope cosine in [0, 1]. Reject parented or non-unit-scale controllers and any controller that also carries `RigidBody`, a physics collider, `PhysicsVelocity`, or `RiverFloatBody`. An invalid fixed update changes neither transform, runtime counters, nor jump latch.
2. Discard finite intent Y, clamp XZ direction magnitude to at most one, and multiply by move speed. Sprint multiplies speed by exactly 1.5. This makes Shift functional; the source feature recorded sprint but did not use it.
3. On a successful fixed update, consume the jump latch once. Increment `jumps_consumed` for every consumed press, including airborne presses. Launch only if previously grounded, increment `jumps_started`, and set upward velocity to jump speed. An airborne press is not buffered until landing. A frame executing zero fixed steps cannot consume a latch; a frame executing multiple fixed steps cannot consume it twice.
4. Determine standability using a downward support ray from the lower capsule sphere center and `normal.y >= max_slope_cos`; positive rising velocity prevents snapping. Desired horizontal velocity is applied on standable ground only. Airborne/steep-slope motion retains momentum and receives gravity. There is no new air-control model.
5. Port the source's bounded collide/solve/cast iteration: at most 32 contact planes and five iterations, capsule points at plus/minus half-segment, `b3World_CollideMover`, `b3SolvePlanes`, and `b3World_CastMover`, followed by velocity clipping. Keep the feature's 0.02 m contact skin and document its support-ray rest-height calculation.
6. Port the bounded post-move ground snap for supported non-rising motion. `stepHeight` is the existing limited rise/snap allowance, not a claim of a four-phase stair solver. Landing must establish grounded without requiring an additional artificial floor. Flat, slope, edge, wall, and tile-seam tests govern the adapted triangle-mesh behavior.
7. Commit transform, velocity, grounded, counters, and cleared latch only after a successful move. `fixed_ticks` counts successful controller fixed updates. A Ready controller over empty collision space still integrates gravity and increments ticks; absence of terrain is not a fabricated support plane.

The feature's four-phase steps, hysteresis, push dynamics, spring grounding, and moving-platform velocity transfer are not implemented by this work and must not be advertised as present.

## Authoring, editing, and Stop

`CharacterController` is added to the component registry and strict recipe parser. Author keys are `radius`, `height`, `moveSpeed`, `maxSlopeAngleDeg`, `stepHeight`, and `jumpSpeed`. Defaults are 0.4 m, 1.8 m, 4.5 m/s, 45 degrees, 0.45 m, and 5 m/s. Store the slope internally as its cosine. Authored slope angles must be finite and within [0, 90]. Unknown controller keys, wrong types, invalid dimensions, conflicting ownership components, parenting, and non-unit scale fail validation before scene replacement. Runtime-only fields are not authorable.

Expose authored fields through the existing descriptors, component fetch/store/add/remove dispatch, SceneService duplication, and SceneChangeTracker component reporting. Mutable editor copies are validated before writing them back. Keep the registry's current RiverFloatBody entry and count; the resulting count is 11. `MoveIntent` and runtime counters are not editable components.

Use one registry-level `scene::validate_character_component` policy for service controller-add and editor controller-store. The editor walking helper reuses it rather than introducing a second edit validator or a generic scene-store framework.

Expose the existing authored FNV-1a hash helper from `scene_registry.h` instead of duplicating its algorithm. Editor player lookup uses the stable authored hash and the current `SceneEntityId.generation`, never a retained Flecs handle or display name. Re-resolve after Stop, reload, deletion, and world changes. Missing or invalid players disable walking cleanly; no replacement player is silently spawned.

Extend `EntitySnapshot` to copy the entire controller, including runtime fields. Preserve the existing `RiverFloatBody`, `RiverFloatState`, animation checkpoints, and other snapshot members. Stop restores the captured controller and transform, restores/removes/recreates components consistently with snapshot presence, and resets `MoveIntent` to all-zero. It also clears editor input/automation overrides and walk mode. A deleted authored player is recreated under its stable scene identity.

Two source-level transport issues are in scope because acceptance depends on them: `SimulationControl::play()` currently rejects Pause; add Pause -> Play resume without taking a replacement snapshot. Editor single-Step currently sets only `max_fixed_steps = 1`; supply exactly one fixed delta on that branch so fast frames or slow time scales cannot consume a Step with zero fixed ticks. Preserve the runtime accumulator semantics; do not change the global accumulator or add another stepping engine. Regressions must first demonstrate both behaviors.

## Editor and observable control surface

Extract testable policy into `MatterEditor/src/character_walk_controller.h/.cpp`; keep GLFW sampling and camera application in `main.cpp`.

- G enables/disables walking for authored id `river-player`. Enabling requires a Ready session, completion of its configured collision installation, and a valid player; a nonzero terrain tile count is not required. It captures the cursor and enters/resumes Play through SimulationControl. Disabling clears input and returns free-fly control but does not Stop the simulation.
- WASD is camera-yaw-relative and normalized. Space uses an explicit testable press-arming rule: focus/UI keyboard-capture loss disarms jumping; only an observed key release while keyboard input is accepted rearms it; the next press emits one latch and disarms again. Holding Space through refocus therefore cannot synthesize a jump. Shift uses the 1.5 sprint multiplier. Focus loss clears live directional input; explicit FIFO input remains independently usable for unattended acceptance.
- Input is sampled before the session tick; camera position follows the resulting player transform after the tick. Eye position is capsule center plus `(0, height * 0.5 - 0.1, 0)`. Yaw/pitch remain camera-owned. Free-fly movement cannot also run in walk mode.
- Retain the existing single camera-follow streaming anchor and its existing update cadence. Do not add `SectorStreaming` to the player or create another streaming owner.
- The walking helper retains only the bound `SceneEntityId` value/generation token, never a Flecs entity handle. Each operation resolves the unique authored identity again; a changed generation or missing/ambiguous identity fails closed. Stop and session replacement clear the binding, including when Stop recreates the original identity. Do not invent a second global scene-generation authority.

Add one typed `viewer::FifoCharacter` command through the existing parser/command registry with ActiveSession lifetime, not an environment-only test mode:

| Grammar | Behavior |
| --- | --- |
| `character walk on` / `character walk off` | Same walking transition as G. |
| `character intent <world_x> <world_z> <0-or-1>` | Requires walking enabled; finite world-space persistent intent and sprint flag suppress live directional sampling while active. |
| `character intent clear` | Clear override, direction, sprint, and pending jump. |
| `character jump` | Requires walking enabled; latch one press in Play/Pause, reject Edit. |
| `character status <label>` | Emit one machine-readable `character_status ` JSON line; no state mutation. |

Labels are 1-64 ASCII letters/digits/underscore/hyphen. Reject malformed grammar and nonfinite numeric input. Status includes label, authored id, numeric scene identity and generation, simulation mode, walk-enabled, position/velocity, grounded, fixed_ticks, jumps_consumed, jumps_started, jump_pending, direction, and sprint. Use deliberate machine-readable stdout like existing FIFO telemetry. Document commands in `docs/agent/control-surface.md`.

Status remains available for a valid player while walking is disabled or the simulation is in Edit, and clear/reset remain callable anytime. FIFO overrides are independent of keyboard focus, not of walking ownership. The helper's intent-setting method takes the current world so it can validate the bound identity directly before storing an override.

The acceptance harness uses existing `pause`, `step`, `wait_frames 1`, `play`, `sim stop`, `shot_now`, and `quit`. Repeat `step` followed by `wait_frames 1` for exact tick counts; multiple adjacent `step` commands would otherwise collapse into one pending boolean. Status after the wait observes completed movement. No hidden controller state, render ray, teleport command, or test-only fixed-step loop is introduced.

## RiverFloatLab slice

Add exactly one entity with id `river-player`, name `River Player`, root/unit-scale identity transform, and `CharacterController` defaults. Its initial capsule center is `[48, 126, 31]`: a deliberately elevated finite drop spawn inside the existing authored collision union, near the current camera's XZ position. Do not snap this authored spawn using an analytic or render-derived height. The first acceptance segment must prove settlement onto installed walkable terrain; if it does not, acceptance fails and the authored spawn must be corrected using actual collision evidence before completion.

The player has no `PartInstance` for this first-person slice, no rigid body/collider/physics velocity, no `RiverFloatBody`, and no streaming component. JS does not author `MoveIntent`; the component lifecycle adds it. Preserve all existing roots, 24 dynamic body definitions, reference crate/raft identities, colliders, buoyancy probes, force caps, collision settings, hydrology network, materials, and water authoring.

The collision region remains `river-gameplay`, min `[-64,-64,-64]`, max `[384,128,64]`, cell size 0.5. Collision is available only where the installed authored union contains terrain. Crossing its boundary or entering water does not create more ground. Water is neither a rigid support surface nor controller buoyancy. Static boulders remain obstacles; crates and rafts keep exclusive rigid-body and RiverFloat system ownership.

## Verification and evidence

Executed commands, captures, tolerances, and limitations are retained in the [acceptance record](../../../findings/river-character-controller-integration-acceptance-2026-08-30.md). The implementation was required to satisfy:

1. Native MSVC tests for Runtime module registration/idempotency; mover flat rest/no drift, walking/normalization/sprint, 30-degree climb, 60-degree slide, walls, tile seams, falling outside finite terrain, replaced terrain generations, static filtering, invalid values/ownership/thread/step timing, two independent characters, and zero/one/multiple-step jump consumption.
2. Registry/bootstrap/editor mutation tests and snapshots covering missing/present/deleted controllers, MoveIntent reset, preserved river runtime state, and Pause -> Play retaining the original snapshot. A transport test drives the production tick-description helper with fast wall time and slow motion and proves exactly one paused fixed step.
3. Existing terrain definition/artifact/physics/session, physics, river-float, ECS, and relevant viewer tests remain green. Node RiverFloatLab and shared river-hydrology scene tests remain green.
4. A freshly built PhysX-enabled MSVC editor. Every build-wrapper invocation explicitly passes `-EnablePhysx`; otherwise the wrapper resets the shared configuration to OFF. Verify `MATTER_ENABLE_PHYSX:BOOL=ON`, copy the pinned `PhysXGpu_64.dll` into the isolated acceptance binary directory, and record matching hashes.
5. Two fresh-process RiverFloatLab runs in a new isolated fixture using a copied project/cache and runtime binaries. Never delete, invalidate, or overwrite the working project's cache. Clear unrelated inherited `MATTER_*` overrides in the child environment and restore caller state. `MATTER_CACHE_ROOT` alone is insufficient isolation because project-derived per-world caches remain authoritative.
6. Machine-checked status sequences and current PNG + `.done` captures for elevated spawn/grounding, exact walking ticks, sprint, a retained paused jump, one-step jump consumption, multi-step no-double-jump, Pause stability, Play resume, and Stop restoration. Compare deterministic paused-step segments across both fresh runs within 0.001 m; Play/resume timing is checked relationally, not given an invented exact tick count.
7. Visually inspect walk/ground/jump captures and reference crate/raft river views. Record limitations honestly: CPU tests prove rigid-body noninterference; screenshots alone do not prove whole-river traversal or buoyancy stability. Reject timeouts, missing status/captures, PhysX fallback, terrain-not-ready, fatal errors, or Vulkan validation errors.

Completion evidence belongs in `docs/findings/river-character-controller-integration-acceptance-2026-08-30.md`, with exact HEAD/dirty-file scope, commands, binary/DLL hashes, logs, captures, measured tolerances, and remaining limitations. Do not mark the roadmap item complete before all required evidence exists.

## Explicit non-goals

Swimming, water buoyancy for characters, moving-platform riding, dynamic-body pushing, raft/craft controls, animation locomotion, NPC AI, third-person character rendering, four-phase step climbing, hysteresis/spring grounding, unbounded terrain collision, legacy per-sector collision, river/waterfall changes, new collision cache formats, and a second streaming anchor are outside this integration.
