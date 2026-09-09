# RiverFloatLab Character-Controller Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the completed controller into the fluid branch and prove deterministic fixed-step walking, installed-terrain collision, jump consumption, Pause/Step/Resume, and Stop restoration in RiverFloatLab without changing river-body ownership.

**Architecture:** A Runtime-registered ECS character module drives an authored ghost capsule through the existing PhysicsContext's static Box3D queries. Strict scene authoring and snapshots preserve controller and river state. A small editor policy unit owns walking/input arbitration and exposes typed FIFO diagnostics. Acceptance reuses SimulationControl and existing FIFO single-step/capture commands in an isolated copied project.

**Tech Stack:** C++17, Flecs, bundled Box3D, current TerrainCollisionRuntime triangle meshes, native Windows MSVC v143 CMake/Ninja, GLFW editor adapter, JavaScript world DSL/Node tests, PowerShell and Python acceptance tooling, existing PhysX-enabled fluid backend.

**Spec:** [RiverFloatLab character-controller integration](../specs/2026-08-30-river-character-controller-integration-design.md).

**Completed record (2026-08-30):** All four tasks are implemented and individually
reviewed through `a8166d78`. Two native RiverFloatLab processes passed the
fixed-step/grounding/jump/Pause/Stop proof with zero measured deterministic
drift. See the [acceptance evidence](../../../findings/river-character-controller-integration-acceptance-2026-08-30.md).
Final cross-task review's numerical-safety finding was fixed in `0440d8b9`
and independently approved; the post-fix native suite passed 14/14 and the
PhysX-enabled editor rebuilt. Retained captures remain tied to their original
binary, as recorded in the acceptance evidence.
This is the bounded bank-path integration, not full playable-river acceptance.
The instructions below are retained implementation history, not active work.

## Global Constraints

- Execute only after the active waterfall editor/build work has released the shared worktree and build tree. Never build while an editor using the target binary is running; do not stop somebody else's process. Re-read `AGENTS.md`, `CLAUDE.md`, `docs/agent/control-surface.md`, and the approved spec before implementation.
- Current worktree is dirty. Record `git status --short` and `git diff --name-only` before each task; existing and concurrent changes are not this task's changes. No reset, clean, checkout-overwrite, broad `git add`, blind cherry-pick, or automatic stashing.
- Source provenance: M0 `8976640c9a8f8add177f6f5e17111a224a8bbd06`, M2 `3b6c082a4c7abc79e72b80ee06ca18eabea0c25e`, M3 `051d3ed95c3dabb0b347811bf7ffdea3219b67ed`, M4 `9974457732adc8a24641fcca72678ae564a1cca0`. Inspect with `git show`; port semantically into current files. Do not restore old Makefiles, old collider-build files, streamed terrain attach hooks, or render-ray/analytic floors.
- Use `apply_patch` for source/docs changes. Preserve current Box3D terrain generation/installation/Ready logic, river/waterfall code and data, RiverFloatBody/RiverFloatState snapshot members, and the single camera-follow streaming anchor.
- Every invocation of `tools/build-windows.ps1`, including preflight and CPU-test target builds, MUST include `-EnablePhysx`. Its default explicitly resets the shared CMake cache to OFF. Use RelWithDebInfo, native MSVC, and `MatterEditor/build/cmake/windows-msvc/relwithdebinfo`; do not substitute MinGW builds or stale binaries.
- Use the existing `MATTER_PHYSX_ROOT` and CUDA 12.8 installation. A missing SDK/runtime is a prerequisite failure, not permission to disable PhysX. Final acceptance checks `MATTER_ENABLE_PHYSX:BOOL=ON` and copies/hashes the pinned `PhysXGpu_64.dll`.
- Test-first cycles below are small checkpoints, not permission to bundle unrelated work. Tests must fail for the intended missing behavior, then pass after the implementation. Record actual command exits; no tests have been run by this plan.
- Run the four tasks in order because each consumes the previous task's contracts. Independent tests/docs within a task can be delegated after file ownership is agreed. Shared `main.cpp`/CMake/registry edits have one owner at a time.
- Each commit checkpoint requires green scoped tests and a spec/code review. Stage only this task's complete new files and reviewed hunks of existing files using the explicit commands at its checkpoint; inspect `git diff --cached --check` and `git diff --cached --stat` before committing. If an existing staged change is not yours, do not commit it; retain a reviewed patch and report the checkpoint as pending instead.
- During this docs-only preparation, do not execute any build, runtime, staging, or commit command below. They are instructions for the subsequent authorized implementation.

## Shared verification setup

Commands are run from the repository root in native PowerShell. Resolve the bundled native CTest/Python once per execution session:

```powershell
$controllerToolchain = (& ./tools/build-windows.ps1 -EnablePhysx -PreflightOnly | Out-String) | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw 'MSVC toolchain preflight failed' }
$controllerCTest = Join-Path (Split-Path $controllerToolchain.CMake) 'ctest.exe'
$controllerBuild = 'MatterEditor/build/cmake/windows-msvc/relwithdebinfo'
```

All commands shown are expected to exit 0 at green checkpoints. Do not interpret an empty CTest selection as passing; always use `--no-tests=error`.

---

## Task 1: Integrate the ghost mover and Runtime-owned module

**Files:**

- Create `MatterEngine3/include/matter/character.h`.
- Create `MatterEngine3/src/ecs/character_systems.cpp`.
- Modify `MatterEngine3/include/matter/physics.h` and `MatterEngine3/src/ecs/physics_context.h/.cpp`.
- Modify `MatterEngine3/src/ecs/ecs_runtime.cpp` and `cmake/manifests/engine-core.sources`.
- Update only the matching canonical source-count expectations in `cmake/MatterViewer.cmake` and `cmake/tests/viewer_graph_tests.cmake`; retain uniqueness checks and run `viewer_graph_tests`.
- Create `MatterEngine3/tests/character_controller_tests.cpp`.
- Modify `cmake/MatterEngine.cmake` to register the new test, existing `MatterEngine3/tests/ecs_tests.cpp`, and focused aggregate.

**Consumes:** `ecs::PrePhysics`, `ecs::FixedPipelineSystem`, `PhysicsSettings`, `PhysicsContextRef`, and existing `replace_terrain_collision(const TerrainCollisionCandidate&, std::string&)`. Tests install triangles through the same terrain candidate seam as `terrain_collision_physics_tests.cpp`. Existing WorldSession tick gating owns Ready/install semantics; standalone Runtime tests need no new Ready singleton.

**Produces:** The following exact contracts, used by tasks 2 and 3:

```cpp
namespace matter::character {
struct CharacterController {
    float radius = 0.4f;
    float height = 1.8f;
    float move_speed = 4.5f;
    float max_slope_cos = 0.70710678f;
    float step_up_height = 0.45f;
    float jump_speed = 5.0f;
    Float3 velocity{};
    bool grounded = false;
    uint64_t fixed_ticks = 0;
    uint32_t jumps_consumed = 0;
    uint32_t jumps_started = 0;
};
struct MoveIntent {
    Float3 move_dir{};
    bool jump = false;
    bool sprint = false;
};
bool valid_character_configuration(const CharacterController&) noexcept;
void register_character_systems(flecs::world&);
struct CharacterModule { explicit CharacterModule(flecs::world&); };
}

namespace matter::physics {
struct CharacterMoveInput {
    Float3 position{}, velocity{}, desired_horizontal_velocity{};
    Float3 gravity{0.0f, -9.81f, 0.0f};
    float radius = 0.4f;
    float half_segment = 0.5f;
    float dt = 1.0f / 60.0f;
    float max_slope_cos = 0.70710678f;
    float step_height = 0.45f;
    uint64_t category_mask = UINT64_MAX;
};
struct CharacterMoveOutput {
    Float3 position{}, velocity{}, ground_normal{0.0f, 1.0f, 0.0f};
    bool grounded = false;
};
bool physics_move_character(flecs::world&, const CharacterMoveInput&,
                            CharacterMoveOutput&);
}
```

- [x] **1.1 Inspect the relevant source functions and the current integration seam.** Read the complete historical `character_systems.cpp`, mover block/callbacks in `physics_context.cpp`, and final feature test file with `git show 9974457732adc8a24641fcca72678ae564a1cca0:<path>`. Compare current owner-thread/stepping guards, static terrain installation, pipeline construction, and bundled `box3d.h` callback signatures. Do not copy historical terrain functions.

- [x] **1.2 Add contracts, minimal inert implementations, and a red executable test.** An inert module registers types only; the mover returns false without touching output. Add `MatterEngine3/src/ecs/character_systems.cpp|all` to the engine manifest. Register `character_controller_tests` through `matter_add_engine_cpu_test`. Also register the existing unregistered `ecs_tests` with `matter_add_engine_cpu_test(ecs_tests MatterEngine3/tests/ecs_tests.cpp)`. Add `matter_character_integration_tests` depending initially on `character_controller_tests`, `ecs_tests`, `physics_tests`, `terrain_collision_definition_tests`, `terrain_collision_artifact_tests`, `terrain_collision_physics_tests`, `terrain_collision_session_tests`, `river_float_system_tests`, `scene_registry_tests`, `entity_recipe_tests`, and `viewer_logic_tests`.

Use a fixture containing `ecs_runtime::Runtime`, with `auto& world = runtime.world()`. Install this finite upward-facing quad through the Runtime's current `PhysicsContextRef`; no synthetic Ready component is required:

```cpp
terrain_collision::TileCandidate tile{};
tile.coordinate = {0, 0, 0};
tile.tile_key = 1;
tile.digest = 2;
tile.vertices = {{-16, 0, -16}, {16, 0, -16},
                 {16, 0, 16}, {-16, 0, 16}};
tile.indices = {0, 2, 1, 0, 3, 2};
terrain_collision::TerrainCollisionCandidate ground{};
ground.geometry_key = 1;
ground.installation_key = 2;
ground.friction = 0.72f;
ground.restitution = 0.0f;
ground.tiles.push_back(tile);
std::string error;
CHECK(world.get<physics::detail::PhysicsContextRef>().value
          ->replace_terrain_collision(ground, error), "install finite terrain");
auto player = world.entity()
    .set<ecs::LocalTransform>({{0, 3, 0}})
    .set<character::CharacterController>({});
CHECK(player.has<character::MoveIntent>(), "controller adds intent");
TickDesc tick{};
tick.frame_delta_seconds = tick.fixed_delta_seconds;
for (int i = 0; i != 240; ++i) runtime.tick(tick);
CHECK(player.get<character::CharacterController>().grounded,
      "ghost settles on installed triangle terrain");
CHECK(!player.has<physics::RigidBody>(), "ghost owns no rigid body");
```

Run red:

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -Target character_controller_tests
& $controllerCTest --test-dir $controllerBuild -C RelWithDebInfo -R '^character_controller_tests$' --output-on-failure --no-tests=error
```

Expected: compilation succeeds after the inert seam exists; the test fails on missing auto-intent or grounded behavior. A missing dependency/compiler error is not the intended red result.

- [x] **1.3 Add input/ownership rejection tests before validation.** Cover NaN/Inf in every vector class and numeric configuration, nonpositive radius/dt, negative half-segment, height smaller than diameter, invalid slope cosine, negative speed/step/jump speed, parented/non-unit entities, and conflicting body/collider/velocity/river ownership. Check failure leaves supplied mover output untouched and a rejected ECS update leaves transform/counters/latch unchanged. Add absent-context, foreign-thread, and in-step rejection using the same supported test seam as terrain collision tests. Implement shared configuration validation and PhysicsContext guards only after these assertions fail.

- [x] **1.4 Implement static query filtering and the mover.** Add `PhysicsContext::move_character(const CharacterMoveInput&, CharacterMoveOutput&)`; keep callbacks private in `physics_context.cpp`. Use `b3Shape_GetBody`, `b3Body_GetType == b3_staticBody`, and `!b3Shape_IsSensor` consistently for plane collection, mover cast, and support ray. Use `b3World_CastRay` with filtering, not unfiltered `CastRayClosest`. Build a local candidate output and assign it only on success. Port M2's five-iteration/32-plane collide/solve/cast path, velocity clipping, standable support ray, rising-motion exclusion, 0.02 m skin, and bounded final snap. Do not add an ECS/static collider or mutate installed terrain.

- [x] **1.5 Add and implement numerical movement regressions.** Extend the fixture with finite inclined quads, two adjacent tiles, a vertical wall, a replacement candidate, static boulders, dynamic boxes, and sensors. Require: rest drift <= 0.001 m over 120 ticks; expected flat rest center near 0.92 m with 0.05 m tolerance; 60 grounded walking ticks at default speed travel 4.5 m +/- 0.10 m; diagonal input is not faster; sprint travels 6.75 m +/- 0.15 m; 30-degree support climbs; 60-degree slope is not grounded and does not accept uphill steering; walls stop horizontal penetration; tile seams do not drop the capsule; changing/removing an installed generation changes support; beyond the finite quad the capsule falls without false grounding. Assert static support is used but dynamic/kinematic/sensor support is ignored and their body state is unchanged by querying. Keep slope/edge thresholds in the tests, not in test-only production branches.

- [x] **1.6 Implement fixed ECS behavior and registration.** Import `CharacterModule` in `Runtime::Runtime()` before `build_pipeline`. Register one named `MatterCharacterController` system; repeated module imports or registration calls must not duplicate it. Add/remove observers own runtime `MoveIntent` lifecycle. Use a local controller copy and local intent copy; normalize XZ, apply 1.5 sprint, consume/launch jumps under the defined counters, read PhysicsSettings gravity, call the mover, then commit successful state and increment fixed_ticks. Frozen/frame-only paths do no controller work; WorldSession continues to gate its non-Ready ticks externally. Do not gate standalone Runtime movement on a new Ready singleton.

- [x] **1.7 Add timing, input independence, and jump regressions.** From settled ground, set `intent.jump=true`; run a frozen frame and a sub-fixed-delta frame and assert no consumption. Run a frame producing two fixed ticks and assert exactly one consumption and one launch. Add airborne press consumption without launch, held-key semantics via intent latch, zero direction stop, two characters with opposing intents, configurable gravity, and no extra physics body count. Compare one two-tick Runtime update with two one-tick updates within 0.0001 m and identical counters.

- [x] **1.8 Green, review, and commit checkpoint.**

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -Target matter_character_integration_tests
& $controllerCTest --test-dir $controllerBuild -C RelWithDebInfo -R '^(character_controller_tests|ecs_tests|physics_tests|terrain_collision_(definition|artifact|physics|session)_tests|river_float_system_tests)$' --output-on-failure --no-tests=error
git diff --check
```

Review especially callback filters, bounds, owner-thread/step guards, transactional state, pipeline registration, and absence of legacy terrain code. Stage new controller/test files explicitly; stage reviewed hunks only from physics/context/runtime/manifests/CMake. Proposed checkpoint: `feat(character): integrate fixed-step ghost capsule with installed terrain`.

```powershell
git add -- MatterEngine3/include/matter/character.h MatterEngine3/src/ecs/character_systems.cpp MatterEngine3/tests/character_controller_tests.cpp
git add -p -- MatterEngine3/include/matter/physics.h MatterEngine3/src/ecs/physics_context.h MatterEngine3/src/ecs/physics_context.cpp MatterEngine3/src/ecs/ecs_runtime.cpp cmake/manifests/engine-core.sources cmake/MatterEngine.cmake
git diff --cached --check
git diff --cached --stat
git commit -m 'feat(character): integrate fixed-step ghost capsule with installed terrain'
```

---

## Task 2: Integrate schema, editor component dispatch, snapshots, and resume

**Files:**

- Modify `MatterEngine3/src/ecs/scene_registry.h/.cpp`.
- Modify `MatterEngine3/src/ecs/simulation_control.h/.cpp`.
- Modify `MatterEngine3/src/scene/scene_service.cpp` and `scene_change_tracker.cpp`.
- Modify existing component fetch/store/add/remove dispatch in `MatterEditor/src/main.cpp`.
- Modify `MatterEngine3/tests/scene_registry_tests.cpp`, `simulation_control_tests.cpp`, and `scene_tracker_tests.cpp`.
- Modify `cmake/MatterEngine.cmake` to register existing unregistered snapshot/tracker tests and add them to the focused aggregate.

**Consumes:** Task 1 `CharacterController`, `MoveIntent`, and `valid_character_configuration`; existing transactional recipe/bootstrap and SceneService/SceneChangeTracker APIs; existing RiverFloatBody/RiverFloatState/animation snapshot fields.

**Produces:** `ComponentKind::CharacterController`; six authorable controller fields; exact authored identity function `uint64_t scene::hash_authored_id(const std::string&)` declared in `scene_registry.h` by exposing the existing implementation; full controller snapshot presence/value; MoveIntent reset on Stop; Pause -> Play without replacing the original snapshot.

Expose `scene::validate_character_component(flecs::entity, const character::CharacterController&, std::string&)` in the registry layer and reuse it for service controller-add and editor controller-store before mutation. SceneService has no generic store API; Task 3's extracted store helper calls this same validator. Its scope is configuration, root/unit-scale, and conflicting ownership validity, not a new general edit framework.

- [x] **2.1 Register snapshot/tracker tests and add red authoring tests.** Use `matter_add_engine_cpu_test(simulation_control_tests MatterEngine3/tests/simulation_control_tests.cpp)` and the corresponding `scene_tracker_tests` target; both link the existing headless library through that function. Append them to `matter_character_integration_tests`. Add a valid authored recipe assertion:

```cpp
RawEntityRecipe raw{"river-player", "River Player", "", R"({
  "LocalTransform":{"translation":[48,126,31],"scale":[1,1,1]},
  "CharacterController":{"radius":0.4,"height":1.8,"moveSpeed":4.5,
    "maxSlopeAngleDeg":45,"stepHeight":0.45,"jumpSpeed":5}
})"};
EntityRecipe recipe;
RecipeError error;
CHECK(validate(raw, recipe, error), "valid authored controller recipe");
CHECK(component_count() == 11, "registry adds controller and keeps river float");
```

Add invalid-value/type/unknown-key/ownership/parent/scale cases and assert field-specific `RecipeError`, transactional bootstrap leaves prior generation intact, defaults are exact, slope converts to cosine, and MoveIntent is auto-added but not authorable. Run red:

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -Target matter_character_integration_tests
& $controllerCTest --test-dir $controllerBuild -C RelWithDebInfo -R '^(scene_registry_tests|simulation_control_tests|scene_tracker_tests)$' --output-on-failure --no-tests=error
```

Expected first failure: `CharacterController` is unknown or count is 10. Add minimal enum/descriptor plumbing only as required to expose the next intended failing assertions.

- [x] **2.2 Implement strict parsing and every scene component path.** Append, never replace, the current RiverFloatBody registry entry. Descriptor offsets use snake-case C++ storage; parser accepts the six camel-case keys from the spec. Validate finite values and cross-component ownership before mutation; enforce root/unit scale. Reject authored runtime counters/intent. Use the shared configuration validator after angle conversion. Expose the existing FNV helper without changing its hash or high-bit behavior. Add controller presence/copy/add/remove handling to SceneService, tracker, and editor dispatch. On duplicate, copy configuration/runtime controller state but reset the duplicated MoveIntent. Validate edited controller copies before setting live ECS data; unrelated existing RiverFloat descriptor gaps are not a reason to rewrite the scene system.

- [x] **2.3 Add red lifecycle/dispatch tests.** Exercise controller add/remove/duplicate through SceneService and direct ECS mutations observed by the tracker. Assert exactly one controller component name, no exposed MoveIntent, zero intent on duplicate, unchanged identity hash semantics, and no body/collider attachment. Test valid configuration round-trip and rejection of invalid edits at the registry/service layer here. The editor dispatch is static in `main.cpp`; task 3 extracts its controller store case into the specified `viewer::store_character_component` helper and tests that production path. Registry tests alone do not claim editor adapter coverage.

- [x] **2.4 Add red snapshot and resume tests.** Start with an authored controller plus a separate dynamic float entity carrying nondefault RiverFloatBody and RiverFloatState. Capture Play; mutate all controller runtime/configuration fields, transform, intent, and float state; delete/recreate variants; Stop and compare every captured field. Assert missing-at-Play controllers are removed and all restored controller intents are zero. Include this regression before changing `play()`:

```cpp
SimulationControl control;
std::string error;
CHECK(control.play(world, error), "initial Play captures snapshot");
const auto original_generation = control.snapshot().generation;
CHECK(control.pause(error), "pause");
CHECK(control.play(world, error), "Pause resumes through Play transport");
CHECK(control.snapshot().generation == original_generation,
      "resume does not recapture snapshot");
CHECK(control.stop(world, error), "Stop restores original Edit state");
```

Also compare original snapshot values and entity count, not generation alone. Confirm Play while already playing still fails, Pause/Step state errors remain explicit, and a queued paused Step is cleared when resuming.

- [x] **2.5 Implement additive snapshot/restore and resume.** Add `character::CharacterController character_controller{}; bool has_character_controller=false;` to EntitySnapshot without removing/reordering ownership meaning of river fields. Capture/restore the whole value; restore component absence, recreate deleted entities through current snapshot logic, and set zero `MoveIntent` whenever a controller is restored. On `play()` from Pause, set Play and clear `step_pending_`, retaining snapshot and animation checkpoints; only Edit -> Play captures. Keep error returns and transactional failure behavior unchanged elsewhere.

- [x] **2.6 Green, review, and commit checkpoint.**

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -Target matter_character_integration_tests
& $controllerCTest --test-dir $controllerBuild -C RelWithDebInfo -R '^(character_controller_tests|scene_registry_tests|entity_recipe_tests|simulation_control_tests|scene_tracker_tests|river_float_system_tests)$' --output-on-failure --no-tests=error
git diff --check
```

Review strict parser failure paths, stable IDs, all dispatch switches, snapshot presence semantics, original snapshot retention on resume, and unchanged float/animation fields. Surgical staging is limited to the files listed for this task. Proposed checkpoint: `feat(scene): author and restore character controller state safely`.

```powershell
git add -p -- MatterEngine3/src/ecs/scene_registry.h MatterEngine3/src/ecs/scene_registry.cpp MatterEngine3/src/ecs/simulation_control.h MatterEngine3/src/ecs/simulation_control.cpp MatterEngine3/src/scene/scene_service.cpp MatterEngine3/src/scene/scene_change_tracker.cpp MatterEditor/src/main.cpp MatterEngine3/tests/scene_registry_tests.cpp MatterEngine3/tests/simulation_control_tests.cpp MatterEngine3/tests/scene_tracker_tests.cpp cmake/MatterEngine.cmake
git diff --cached --check
git diff --cached --stat
git commit -m 'feat(scene): author and restore character controller state safely'
```

---

## Task 3: Wire editor walking and typed observable control

**Files:**

- Create `MatterEditor/src/character_walk_controller.h/.cpp`.
- Modify `MatterEditor/src/main.cpp`, `MatterEditor/src/viewer_commands.h`, and `cmake/manifests/editor.sources`.
- Update the editor's exact source count in `cmake/MatterEditor.cmake` from 40 to 41 and add the matching `matter_editor` SOURCES count/uniqueness assertion in `cmake/tests/viewer_graph_tests.cmake`, then run `viewer_graph_tests`. The engine-viewer counts in `cmake/MatterViewer.cmake` remain unchanged.
- Create `MatterEngine3/tests/character_walk_controller_tests.cpp`.
- Modify `cmake/MatterEngine.cmake` and `docs/agent/control-surface.md`.

**Consumes:** Task 1 controller/intent, task 2 authored hash and SceneEntityId semantics, SimulationControl Play/Pause/Step/Stop, existing FIFO typed registry, Ready state, camera orientation, and the existing camera-follow streaming anchor.

**Produces:** A GLFW-free editor policy unit with the following interfaces in `namespace viewer`; main owns key sampling/cursor capture and applies the eye pose:

```cpp
struct CharacterWalkInput {
    matter::Float3 world_direction{};
    bool sprint = false;
    bool jump_pressed = false;
};
class CharacterJumpEdge {
public:
    bool update(bool space_down, bool accepts_keyboard) noexcept;
    void reset() noexcept;
private:
    bool armed_ = false;
};
class CharacterWalkController {
public:
    bool set_enabled(flecs::world&, matter::scene::SimulationControl&,
                     bool enabled, bool session_ready, std::string& error);
    bool set_intent(flecs::world&, matter::Float3 direction, bool sprint,
                    std::string& error);
    void clear_intent(flecs::world&);
    bool latch_jump(flecs::world&, matter::scene::SimulationMode, std::string& error);
    void sample(flecs::world&, matter::scene::SimulationMode,
                const CharacterWalkInput&);
    void reset(flecs::world&);
    bool enabled() const noexcept;
    bool eye_position(flecs::world&, matter::Float3& out) const;
    bool status_json(flecs::world&, matter::scene::SimulationMode,
                     const std::string& label, std::string& out,
                     std::string& error) const;
};
matter::TickDesc make_editor_tick(matter::scene::SimulationControl&,
                                 float wall_dt, float time_scale);
bool store_character_component(flecs::entity,
                                const matter::character::CharacterController&);
```

The class stores mode, input override, and the bound `SceneEntityId` value/generation token while enabled, never a cross-frame Flecs entity handle. Resolve exactly one nonzero-generation authored `river-player` identity by the shared hash on every operation and compare it with that token while bound. A changed generation or ambiguous/missing identity fails closed; no new global SceneGeneration singleton is needed. Stop/session replacement explicitly resets the binding even when restoring the original identity token. `set_enabled(true, session_ready)` fails unless the caller supplies a Ready session and the player is valid; main derives that flag from existing WorldSession readiness, including completion of configured collision installation, not terrain tile count. It enters/resumes Play via SimulationControl. Missing/replaced players make the next sample fail closed to disabled/zero input. `reset` clears intent, override, binding, and walk state. `status_json` is observational. CharacterJumpEdge stores only press arming, initially disarmed.

- [x] **3.1 Create inert helper, register test target, and write red behavior tests.** Register `character_walk_controller_tests` with the helper source using `matter_add_engine_cpu_test`; no GLFW dependency is needed for policy tests. Append it to the focused aggregate and add the helper to `editor.sources`. Tests instantiate a Runtime with authored player and explicitly pass `session_ready=true` to enable; no lifecycle singleton is necessary. Exercise false-readiness rejection, a Ready-but-empty-terrain world that falls normally, enable/disable, missing player, changed generation, deletion, reset, intent arbitration, eye offset, invalid inputs, and all status fields. The first inert implementation returns failure; expect the enable/lookup assertion to fail.

- [x] **3.2 Reproduce the source-level Step edge using the production helper contract.** First extract the current tick-description construction into `make_editor_tick` without changing behavior. Add a test with a fresh Runtime, paused SimulationControl, `control.step(error)`, `wall_dt = 1/240`, and `time_scale = 0.1`. Require `runtime.tick(make_editor_tick(control, 1.0f / 240.0f, 0.1f)).fixed_steps == 1`; current behavior should produce zero. A no-Step paused frame must preserve position/counters/accumulator. Correct only the Step branch:

```cpp
matter::TickDesc tick{};
tick.presentation_delta_seconds = wall_dt;
tick.frame_delta_seconds = wall_dt * time_scale;
if (control.should_advance_fixed()) {
    return tick;
}
if (control.consume_pending_step()) {
    tick.frame_delta_seconds = tick.fixed_delta_seconds;
    tick.max_fixed_steps = 1;
    return tick;
}
tick.advance_fixed = false;
tick.frame_delta_seconds = 0.0f;
return tick;
```

Test the same helper for Edit, Pause, normal Play, slow Play, paused Step with a retained fractional accumulator, and consecutive Step calls separated by frame ticks. Do not alter Runtime's accumulator algorithm or introduce another fixed loop.

- [x] **3.3 Add typed FIFO parser tests before command implementation.** Extend `FifoParsedCommand` with `FifoCharacter`; define `Action {Walk, Intent, ClearIntent, Jump, Status}`, boolean enabled/sprint, Float3 direction, and string label. Test every grammar row in the spec; reject extra tokens, missing tokens, NaN/Inf, invalid sprint flags, empty/overlong/unsafe labels, and unknown subcommands. Test dispatch against a stale ActiveSession epoch. Register the handler through the existing registry and add the parser dispatch branch; do not hide the feature behind `MATTER_WALK_TEST`.

Run red/green for the helper and parser using:

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -Target character_walk_controller_tests
& $controllerCTest --test-dir $controllerBuild -C RelWithDebInfo -R '^character_walk_controller_tests$' --output-on-failure --no-tests=error
```

- [x] **3.4 Implement walking input and lifecycle policy.** Resolve stable identity with `scene::hash_authored_id("river-player")`; reject conflicting ownership and non-unit/parented transforms. The helper arbitrates persistent automation intent versus live input; neither path clears a pending jump merely because the next render frame has no Space edge. `sample` writes direction/sprint and ORs jump_pressed only in Play/Pause. Edit writes zero input. `clear_intent` and reset explicitly clear the latch. `status_json` emits finite numeric arrays for position/velocity/direction and integer counters, booleans, identity/generation, walk/mode, and validated label. No stringly hidden state or teleporting.

Persistent intent and jump require walking enabled; jump additionally requires Play/Pause. `set_intent` accepts the world to validate the current bound identity before changing its override. Status remains available for a valid player in Edit/disabled mode, and clear/reset remain callable anytime. FIFO overrides are independent of focus and live keys, not of walk ownership; do not use status serialization as a substitute for input validation.

- [x] **3.5 Connect the narrow main.cpp adapter.** G on Ready toggles walking using the same handler policy as FIFO. Sample WASD from yaw, normalize XZ, Shift sprint, and focus/UI capture rules before tick. Feed Space through CharacterJumpEdge: if input is not accepted, set armed_ false and return false; if Space is released while accepted, set armed_ true and return false; if pressed while armed, set armed_ false and return true; otherwise return false. Test focus loss while held, refocus still held, release, then next press; only the final press may latch. Disable free-fly translation while walking. Clear live directional input on focus loss without disabling explicit FIFO override. Call the helper's eye_position after `session->tick` so camera follows current state. Keep yaw/pitch camera-owned and the existing single camera-follow anchor path; add no player streaming component. Reset walk/input and CharacterJumpEdge on Stop and on session replacement/reload, re-resolving after snapshot recreation. Do not register CharacterModule here or add render-ray floor code.

- [x] **3.6 Cover component edit validation and diagnostics.** Extract the controller-specific editor store case into `store_character_component`; it validates the candidate configuration and root/unit-scale/no-body ownership before setting the entity, and returns false without mutation on failure. Main's CharacterController store case calls this helper. Test valid round-trip and invalid dimensions/ownership through the production helper. Verify status reads do not alter intent/counters; one paused jump survives multiple render frames and is consumed only by the next fixed step; clearing override/Stop prevents held input leaking into a new Play. Verify missing player produces an explicit command failure rather than reporting a fabricated origin.

The existing `frame_camera` snapshot precedes `session->tick`; make it mutable and refresh it from the eye-adjusted camera immediately after that tick. Render using the current pose while preserving the camera-owned view direction and the existing single streaming-anchor cadence.

- [x] **3.7 Document, green, review, and commit checkpoint.** Add grammar, input coordinate convention, status JSON schema/counter meanings, lifecycle/error behavior, and `step` + `wait_frames 1` sequencing to `docs/agent/control-surface.md`. Explain that walking does not add collision outside the authored union or float on water.

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -Target matter_character_integration_tests
& $controllerCTest --test-dir $controllerBuild -C RelWithDebInfo -R '^(character_walk_controller_tests|character_controller_tests|simulation_control_tests|viewer_logic_tests)$' --output-on-failure --no-tests=error
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -Target matter_editor
git diff --check
```

Review stale-world/Stop behavior, UI capture, fixed-step-only movement, tick-helper parity with main, strict parsing, ActiveSession dispatch, and camera ownership. Stage only listed files/new helper/test and reviewed main/CMake hunks. Proposed checkpoint: `feat(editor): add authored character walking and FIFO diagnostics`.

```powershell
git add -- MatterEditor/src/character_walk_controller.h MatterEditor/src/character_walk_controller.cpp MatterEngine3/tests/character_walk_controller_tests.cpp
git add -p -- MatterEditor/src/main.cpp MatterEditor/src/viewer_commands.h cmake/manifests/editor.sources cmake/MatterEngine.cmake docs/agent/control-surface.md
git diff --cached --check
git diff --cached --stat
git commit -m 'feat(editor): add authored character walking and FIFO diagnostics'
```

---

## Task 4: Author RiverFloatLab player and retain acceptance evidence

**Files:**

- Modify `projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js` and `projects/world_demo/tests/river_float_lab_scene_tests.mjs`.
- Create `MatterEngine3/tools/run_character_controller_acceptance.ps1`.
- Create `MatterEngine3/tools/character_controller_acceptance.py` and `MatterEngine3/tools/tests/character_controller_acceptance_tests.py`.
- Create `docs/findings/river-character-controller-integration-acceptance-2026-08-30.md` after actual runs.
- Modify `ROADMAP.md` only after acceptance succeeds; do not reorder or remove unrelated roadmap/waterfall edits.

**Consumes:** Tasks 1-3 contracts, unchanged RiverFloatLab collision/hydrology/body definitions, native editor artifact, pinned PhysX runtime, existing `MatterEngine3/tools/drive.py` protocol, and current working-project cache as read-only copy input.

**Produces:** One authored player, Node scene invariants, a reproducible isolated acceptance runner, machine-checked logs/current captures, and a concise findings record linked from the roadmap.

- [x] **4.1 Add the failing Node player invariant.** Extend the existing test's loaded `buildRiverFloatLabDefinition` result. Assert exactly one id `river-player`, name `River Player`, root/no parent, identity rotation, unit scale, and the exact controller defaults below. Assert absence of authored MoveIntent, RigidBody, PhysicsVelocity, all physics colliders, RiverFloatBody, PartInstance, and SectorStreaming. Retain the existing 24-dynamic-body, boulder, reference crate/raft, shared river, and exact collision-union assertions. Run:

```powershell
node --experimental-default-type=module projects/world_demo/tests/river_float_lab_scene_tests.mjs
```

Expected red: player lookup/count assertion fails; existing river assertions remain intact.

- [x] **4.2 Add only the player recipe.** Append it after the existing body/boulder recipes in `buildRiverFloatLabDefinition`; do not alter body placement indexing or random variation:

```js
entities.push({
  id: "river-player",
  name: "River Player",
  components: {
    LocalTransform: {
      translation: [48, 126, 31],
      rotation: [0, 0, 0, 1],
      scale: [1, 1, 1],
    },
    CharacterController: {
      radius: 0.4, height: 1.8, moveSpeed: 4.5,
      maxSlopeAngleDeg: 45, stepHeight: 0.45, jumpSpeed: 5,
    },
  },
});
```

This is an elevated authored drop spawn, not an assumed ground height. The acceptance run must prove grounding. Do not compensate for a failure with an invisible platform, collision union expansion, render ray, or analytical floor.

- [x] **4.3 Test the evidence checker before creating the runner.** The Python checker reads `character_status ` JSON lines and current PNG/`.done` artifacts. Unit tests provide a complete small synthetic valid trace and reject: missing/duplicate labels, nonfinite coordinates, wrong scene identity, unexpected fixed counts, double-consumed jump, airborne press counted as a launch, paused drift, overwritten Stop snapshot, cross-run deterministic drift > 0.001 m, missing/stale captures, timeout/error markers, and PhysX-disabled/fallback output. Use `unittest`; no third-party dependency is needed.

```powershell
& $controllerToolchain.Python -m unittest discover -s MatterEngine3/tools/tests -p character_controller_acceptance_tests.py
```

Expected red: checker rejects the valid trace or accepts a malformed trace until implemented. Then implement parsing/validation with explicit missing-field errors and strict finite numeric checks. Do not accept a screenshot-only pass.

- [x] **4.4 Implement isolated runner and provenance checks.** Parameters: mandatory `-OutputDir`, optional `-TimeoutSeconds` default 4200. Require a new or empty output directory; canonicalize it and never recursively delete it. Resolve the SDK root from the verified build's `MATTER_PHYSX_ROOT:PATH` cache entry when the environment variable is absent; if both exist, require canonical equality. Pass that root explicitly to `tools/build-windows.ps1 -EnablePhysx -PhysxRoot <resolved-root> -PreflightOnly`. Refuse to proceed unless CMakeCache has exactly `MATTER_ENABLE_PHYSX:BOOL=ON`, the source MSVC editor exists, and the pinned DLL exists at `<resolved-root>/physx/bin/win.x86_64.vc143.mt/release/PhysXGpu_64.dll`. Do not change global environment settings to satisfy this prerequisite.

Create a new fixture tree under OutputDir with `projects/world_demo`, `MatterEngine3/shared-lib`, `MatterEngine3/tools`, `MatterEditor`, and `bin`. Copy the project including its existing `.cache` read-only as input; no hardlinks or junctions. Copy engine shared-lib, `drive.py`, the newly built editor and adjacent runtime DLLs into the fixture. Copy the pinned PhysX GPU DLL into fixture/bin even if another search path contains a copy; compare SHA256 with the pinned source and record both. Use the copied drive.py so its editor working directory, preferences, command file, and cache writes stay in the fixture; pass the copied binary via `--editor`. The copied project wins executable-relative world discovery. Record source/copy editor hashes, DLL hashes, HEAD, dirty-file list, source script hashes, and CMake feature check in provenance JSON.

Child runs inherit native SDK/runtime PATH and OS TMP/TEMP, but clear unrelated inherited `MATTER_*` overrides; then set only the documented acceptance options (RiverFloatLab, command file, 1280x720, Vulkan validation, and normal rendering). Save/restore any caller environment changes in `finally`. Use the existing drive.py timeout/exit/current-shot checks; the runner must not kill unrelated editor processes. Do not reuse the waterfall agent's fixture or copy a live cache while it is being written.

- [x] **4.5 Generate two identical deterministic timelines.** Each fresh process begins in Edit. In PowerShell, generate each repeated fixed segment as literal `step` then `wait_frames 1` lines. Use absolute, unique shot paths. The sequence is:

| Segment | Commands and required observation |
| --- | --- |
| Ready/Edit baseline | `wait_event bake.finished 3600`, `wait_idle 120`, `character status edit`, `shot_now edit.png`; require Ready/install success, position `[48,126,31]`, counters zero. |
| Deterministic settle | `character walk on`, immediately `pause`, `character intent 0 0 0`, then 300 paired single steps; `character status grounded`, `shot_now grounded.png`; fixed_ticks=300, grounded true, finite position inside authored union. |
| Walk | `character intent 1 0 0`, 60 paired steps; status/capture `walk`; fixed_ticks=360, horizontal displacement > 0.5 m, no teleport/penetration, finite inside union. Exact flat speed is established by CPU tests, not assumed on a bank. |
| Sprint | `character intent 1 0 1`, 30 paired steps; status/capture `sprint`; fixed_ticks=390, finite forward progress. CPU tests establish the 1.5 ratio. |
| Settle before jump | `character intent 0 0 0`, 120 paired steps; status `jump_base`; fixed_ticks=510 and grounded true. |
| Paused latch | `character jump`, status `latched`, `wait_frames 5`, status `paused`; same position/ticks/counters, jump_pending true. |
| One-step jump | One `step`/`wait_frames 1`; status/capture `jump`; fixed_ticks=511, jumps_consumed=1, jumps_started=1, jump_pending false, rising and not grounded. |
| No repeated jump | 120 paired zero-intent steps; status/capture `landed`; fixed_ticks=631, consumed/started remain 1, grounded true. |
| Resume | `play`, `wait_frames 20`, `pause`, status `resumed`; ticks increased beyond 631. `wait_frames 5`, status `paused_again`; position and ticks unchanged from resumed. Resume timing is relational, not exact. |
| Stop | `character jump`, `sim stop`, `wait_frames 2`, status/capture `stopped`; Edit, initial position/controller state/counters restored, zero intent/latch, walk disabled. |
| River sanity | Use existing `cam`/`shot_now` commands with camera eye `[48,91,31]`, target `[31,69,7]`; capture `river-restored` after Stop and a separate short Play/Pause river view. Preserve reference body configuration and inspect crate/raft water/terrain interaction. End with `quit`. |

Run the checker after both processes exit. Compare `grounded`, `walk`, `sprint`, `jump_base`, `latched`, `paused`, `jump`, and `landed` positions/velocities/counters across runs with 0.001 m numeric tolerance. Exclude wall-time Play-resume samples from exact cross-run comparison. Status/captures must come from this invocation, and all FIFO timeout markers fail even if later commands continued.

If the chosen authored bank spawn or path fails the grounding/progress predicates, inspect installed-collision status and captures, select a supported authored bank location within the unchanged union, update the explicit recipe/Node invariant/timeline expectations, and repeat the same tests. This is a test-driven authored placement correction, not permission to weaken predicates or fabricate support.

- [x] **4.6 Run the smallest complete native/Node/tooling gate, then the editor.**

```powershell
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -Target matter_character_integration_tests
& $controllerCTest --test-dir $controllerBuild -C RelWithDebInfo -R '^(character_controller_tests|character_walk_controller_tests|ecs_tests|physics_tests|scene_registry_tests|entity_recipe_tests|scene_tracker_tests|simulation_control_tests|terrain_collision_(definition|artifact|physics|session)_tests|river_float_system_tests|viewer_logic_tests)$' --output-on-failure --no-tests=error
node --experimental-default-type=module projects/world_demo/tests/river_float_lab_scene_tests.mjs
node --experimental-default-type=module projects/world_demo/tests/river_hydrology_scene_tests.mjs
& $controllerToolchain.Python -m unittest discover -s MatterEngine3/tools/tests -p character_controller_acceptance_tests.py
./tools/build-windows.ps1 -Config RelWithDebInfo -EnablePhysx -Target matter_editor
./MatterEngine3/tools/run_character_controller_acceptance.ps1 -OutputDir build/qa/river-character-controller-2026-08-30
```

The output path must be unused; if it already contains evidence, choose the next numeric suffix and record the exact path rather than deleting prior evidence. No later non-PhysX configuration is allowed before this runtime acceptance.

- [x] **4.7 Inspect evidence and complete the findings record.** Open current grounded/walk/jump/landed/stopped/river screenshots; verify camera height/contact, no disappearing ground, no penetration/teleport, and unchanged visible river-body behavior. Pair this with status, static-filter/body-noninterference tests, terrain/session tests, and preserved float snapshot tests. Report that whole-river traversal, buoyancy endurance, swimming, and craft riding are not proven by these captures. If any required check fails, leave the roadmap incomplete and retain the failure artifacts.

- [x] **4.8 Final review and surgical commit checkpoint.** Record every actual command/result, editor/DLL hashes, feature check, per-run log/capture paths, measured status deltas, source commit provenance, and limitations in the findings document. Review the full diff against the spec, then update only the relevant controller roadmap item with its evidence link. Stage only the authored-player hunk, its Node tests, new runner/checker/tests/findings, and the reviewed roadmap hunk. Inspect staged diff and ensure no waterfall/other agent changes are included. Proposed checkpoint: `test(character): validate RiverFloatLab controller integration`.

```powershell
git add -- MatterEngine3/tools/run_character_controller_acceptance.ps1 MatterEngine3/tools/character_controller_acceptance.py MatterEngine3/tools/tests/character_controller_acceptance_tests.py docs/findings/river-character-controller-integration-acceptance-2026-08-30.md
git add -p -- projects/world_demo/scenes/RiverFloatLab/RiverFloatLab.js projects/world_demo/tests/river_float_lab_scene_tests.mjs ROADMAP.md
git diff --cached --check
git diff --cached --stat
git commit -m 'test(character): validate RiverFloatLab controller integration'
```

## Final self-review gate

- [x] M0/M2/M3/M4 provenance is cited; old M1/streamed terrain code and floor fallbacks are absent.
- [x] Runtime owns registration; root/unit-scale ghost owns transform; static queries cannot take raft ownership.
- [x] Strict authoring/editor validation, all component dispatch paths, original snapshots, river runtime fields, and intent reset are tested.
- [x] Jump and fixed-tick diagnostics have defined meanings, are observable through typed FIFO commands, and are not substituted by screenshots.
- [x] Pause resume and true one-tick Step regressions are green through the same helpers used by main.
- [x] Every build-wrapper command includes `-EnablePhysx`; final binary/DLL/features are verified; native focused tests, Node tests, checker tests, and two fresh-process captures pass.
- [x] Existing working caches, UI state, waterfall changes, and unrelated staged work are preserved; acceptance writes only its new fixture/output tree.
- [x] The findings and roadmap make no claims about swimming, platforms, pushing, advanced stairs, or full-river traversal.
