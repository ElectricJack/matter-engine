# Flecs ECS Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a pinned Flecs ECS foundation to MatterEngine3 so every `WorldSession` owns, exposes, and advances exactly one reflected, fixed-step-capable ECS world without changing existing rendering or world behavior.

**Architecture:** MatterEngine3 owns an internal `ecs_runtime::Runtime` that wraps `flecs::world`, registers the public `matter::ecs` module, manages the fixed-step accumulator, drains worker-to-main-thread state commands, and runs separate fixed and frame pipelines. `WorldSession` owns this runtime for its full lifetime and exposes only the underlying world by reference. Core transforms are ordinary Flecs components; hierarchy uses `flecs::ChildOf`, while MatterEngine3 supplies validated reparenting and dirty-subtree propagation.

**Tech Stack:** C++17, Flecs v4.1.6 amalgamated C/C++ distribution, GNU Make, MinGW cross-build, existing MatterEngine3 `check.h` test harness.

## Global Constraints

- Phase 1 is entirely inside MatterEngine3 except for mechanical build/caller changes in MatterViewer and ExplorerDemo.
- Do not add Box3D runtime behavior, sector-streaming components, editor UI/gizmos, runtime JavaScript, persistence, replication, or GameNetworkingSockets.
- Do not convert baked parts, render instances, sectors, lights, or cameras into entities.
- Keep one ECS world per `WorldSession`; it survives `reload()` and `regenerate()` and is destroyed only with the session.
- The thread calling `WorldSession::tick()` is the only thread allowed to access Flecs. Worker threads may enqueue plain-data state messages only.
- `WorldSession::tick(const TickDesc&)` replaces the parameterless overload. Do not retain a compatibility overload.
- Existing finite-world rendering and controls must remain behaviorally unchanged.
- Use the exact Flecs v4.1.6 `distr/flecs.h` and `distr/flecs.c` files and ship the upstream license unchanged.
- Run every test command from the repository root. On Windows, use the shown `wsl bash -lc` wrapper for GNU Make commands.

---

## File and Responsibility Map

| Path | Responsibility after Phase 1 |
|---|---|
| `Libraries/flecs/flecs.h` | Pinned public Flecs C/C++ amalgamated header |
| `Libraries/flecs/flecs.c` | Pinned Flecs implementation, compiled exactly once per final binary/library |
| `Libraries/flecs/LICENSE` | Unmodified upstream license |
| `Libraries/flecs/VERSION` | Pin, release URL, and source hashes |
| `MatterEngine3/include/matter/math_types.h` | Adds public `Quaternion` POD |
| `MatterEngine3/include/matter/ecs.h` | Public components, phases, module, hierarchy helper, and Flecs include |
| `MatterEngine3/include/matter/world_session.h` | Public `TickDesc`, ECS accessors, and cumulative ECS stats |
| `MatterEngine3/src/ecs/transform_math.h` | Header-only TRS-to-matrix helper using engine matrix convention |
| `MatterEngine3/src/ecs/ecs_runtime.h` | Private runtime, tick result, queued state-message contract |
| `MatterEngine3/src/ecs/ecs_runtime.cpp` | Module registration, phases/pipelines, accumulator, command drain |
| `MatterEngine3/src/ecs/transform_system.cpp` | Dirty tracking, validated reparenting, hierarchy propagation |
| `MatterEngine3/src/matter_engine.cpp` | Owns runtime and bridges session/bake lifecycle to it |
| `MatterEngine3/tests/ecs_tests.cpp` | Headless ECS, reflection, transforms, hierarchy, scheduling, tick tests |
| `MatterEngine3/tests/world_stream_tests.cpp` | Session lifecycle/state survival regression using explicit `TickDesc` |
| `MatterEngine3/Makefile` | Builds Flecs C object and ECS C++ sources into `libmatter_engine3.a` |
| `MatterEngine3/tests/Makefile` | Builds/runs `ecs_tests`; links Flecs wherever session code requires it |
| `MatterViewer/main.cpp` | Supplies measured frame delta to session tick |
| `MatterViewer/main_linux.cpp` | Supplies measured frame delta to session tick |
| `MatterViewer/Makefile` | Windows direct-source compatibility for ECS and Flecs |
| `ExplorerDemo/main.cpp` | Temporary explicit-tick migration until Phase 3 deletion |
| `ExplorerDemo/Makefile` | Temporary Windows direct-source compatibility until Phase 3 deletion |

---

### Task 1: Vendor and Pin Flecs v4.1.6

**Files:**

- Create: `Libraries/flecs/flecs.h`
- Create: `Libraries/flecs/flecs.c`
- Create: `Libraries/flecs/LICENSE`
- Create: `Libraries/flecs/VERSION`

- [ ] **Step 1: Record the expected pre-vendor failure**

Run:

```powershell
Test-Path Libraries\flecs\flecs.h
```

Expected: `False`.

- [ ] **Step 2: Download the exact upstream release artifacts**

Run:

```powershell
New-Item -ItemType Directory -Force Libraries\flecs
curl.exe -L https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/distr/flecs.h -o Libraries\flecs\flecs.h
curl.exe -L https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/distr/flecs.c -o Libraries\flecs\flecs.c
curl.exe -L https://raw.githubusercontent.com/SanderMertens/flecs/v4.1.6/LICENSE -o Libraries\flecs\LICENSE
```

Do not regenerate or edit the downloaded files.

- [ ] **Step 3: Add a reproducible pin manifest**

Generate `Libraries/flecs/VERSION` from the downloaded files:

```powershell
$headerHash = (Get-FileHash -Algorithm SHA256 Libraries\flecs\flecs.h).Hash.ToLowerInvariant()
$sourceHash = (Get-FileHash -Algorithm SHA256 Libraries\flecs\flecs.c).Hash.ToLowerInvariant()
$version = @(
    'version=4.1.6'
    'release=https://github.com/SanderMertens/flecs/releases/tag/v4.1.6'
    "header_sha256=$headerHash"
    "source_sha256=$sourceHash"
) -join "`n"
[System.IO.File]::WriteAllText((Join-Path $PWD 'Libraries\flecs\VERSION'), $version + "`n")
```

The resulting file contains two actual lowercase 64-character hashes and no symbolic values.

- [ ] **Step 4: Verify pin contents and license**

Run:

```powershell
Get-FileHash -Algorithm SHA256 Libraries\flecs\flecs.h, Libraries\flecs\flecs.c
Get-Content Libraries\flecs\VERSION
Select-String -Path Libraries\flecs\LICENSE -Pattern "MIT License"
```

Expected: computed hashes match `VERSION`; the license query returns a match.

- [ ] **Step 5: Commit the dependency pin**

```powershell
git add Libraries/flecs
git commit -m "build(ecs): vendor Flecs 4.1.6"
```

---

### Task 2: Add the Public ECS Contract and Headless Build Target

**Files:**

- Modify: `MatterEngine3/include/matter/math_types.h`
- Create: `MatterEngine3/include/matter/ecs.h`
- Create: `MatterEngine3/tests/ecs_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

- [ ] **Step 1: Write the failing public-contract test**

Create `MatterEngine3/tests/ecs_tests.cpp` with the existing `check.h` harness style and these first cases:

```cpp
#include "check.h"
#include "matter/ecs.h"

using namespace matter;

static void test_entity_lifecycle_and_components() {
    flecs::world world;
    world.import_<ecs::CoreModule>();

    const flecs::entity entity = world.entity("RuntimeObject")
        .set<ecs::LocalTransform>({{1, 2, 3}, {}, {1, 1, 1}});
    CHECK(entity.is_alive());
    CHECK(entity.has<ecs::LocalTransform>());
    entity.add<ecs::TransformDirty>();
    CHECK(entity.has<ecs::TransformDirty>());
    entity.remove<ecs::TransformDirty>();
    CHECK(!entity.has<ecs::TransformDirty>());

    const flecs::entity_t id = entity.id();
    entity.destruct();
    CHECK(!world.is_alive(id));
}

static void test_deferred_structural_mutation() {
    flecs::world world;
    world.import_<ecs::CoreModule>();
    world.entity().set<ecs::LocalTransform>({});

    world.defer_begin();
    world.each<ecs::LocalTransform>([](flecs::entity e, ecs::LocalTransform&) {
        e.add<ecs::TransformDirty>();
    });
    world.defer_end();

    CHECK(world.count<ecs::TransformDirty>() == 1);
}
```

Add a `main()` that calls both tests and follows the pass/fail reporting convention used by `sector_streamer_tests.cpp`.

- [ ] **Step 2: Add the test target before adding the header**

In `MatterEngine3/tests/Makefile` add:

```make
FLECS_DIR = ../../Libraries/flecs
FLECS_C = $(FLECS_DIR)/flecs.c

ECS_TARGET = ecs_tests
ECS_CPP = ecs_tests.cpp ../src/ecs/ecs_runtime.cpp ../src/ecs/transform_system.cpp
ECS_C = $(FLECS_C)
```

Add a dedicated `flecsc` C flavor compiled with `gcc -std=c99 -O2 -I$(FLECS_DIR)`. Add `ECS_CPP` to the `def` source union, `ECS_C` to the new C flavor union, and add:

```make
ECS_OBJS = $(call obj_list,def,$(ECS_CPP)) $(call obj_list,flecsc,$(ECS_C))

$(ECS_TARGET): $(ECS_OBJS)
	$(CC) $(ECS_OBJS) -o $@ $(FLAVOR_def_FLAGS) $(LDFLAGS)

run-ecs: $(ECS_TARGET)
	./$(ECS_TARGET)
```

Also add `run-ecs` to `.PHONY`, `flecsc_C_OBJS` to `ALL_OBJS`, and `$(ECS_TARGET)` to `clean`.

- [ ] **Step 3: Run the test to prove the API is missing**

Run:

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterEngine3/tests run-ecs'
```

Expected: compilation fails because `matter/ecs.h` does not exist.

- [ ] **Step 4: Add the public PODs and module declarations**

Append to `MatterEngine3/include/matter/math_types.h`:

```cpp
struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};
```

Create `MatterEngine3/include/matter/ecs.h`:

```cpp
#pragma once
#include <cstdint>
#include "flecs.h"
#include "matter/math_types.h"

namespace matter::ecs {

struct LocalTransform {
    Float3 translation{};
    Quaternion rotation{};
    Float3 scale{1.0f, 1.0f, 1.0f};
};

struct WorldTransform { Mat4f matrix{}; };
struct TransformDirty {};

enum class WorldStatus : uint8_t { Loading, Ready, Failed };
struct WorldRuntimeState {
    WorldStatus status = WorldStatus::Loading;
    uint64_t content_generation = 0;
};

struct FixedPreUpdate {};
struct FixedUpdate {};
struct PrePhysics {};
struct Physics {};
struct PostPhysics {};
struct FixedPostUpdate {};
struct FrameUpdate {};
struct FixedPipelineSystem {};
struct FramePipelineSystem {};

struct CoreModule {
    explicit CoreModule(flecs::world& world);
};

bool reparent(flecs::entity child, flecs::entity parent);
void clear_parent(flecs::entity child);

} // namespace matter::ecs
```

- [ ] **Step 5: Add the minimum module registration needed by the tests**

Create `MatterEngine3/src/ecs/ecs_runtime.cpp` and define `CoreModule` with `world.module<CoreModule>()`, component registrations, and a `WorldRuntimeState` singleton. Create empty `ecs_runtime.h` and `transform_system.cpp` translation units temporarily so the target links. Register components under the `matter.ecs` module scope, then restore the previous scope.

- [ ] **Step 6: Run the public-contract tests**

Run the `run-ecs` command from Step 3.

Expected: `ecs_tests: all checks passed`.

- [ ] **Step 7: Commit the public foundation**

```powershell
git add MatterEngine3/include/matter/math_types.h MatterEngine3/include/matter/ecs.h MatterEngine3/src/ecs MatterEngine3/tests/ecs_tests.cpp MatterEngine3/tests/Makefile
git commit -m "feat(ecs): add core MatterEngine ECS module"
```

---

### Task 3: Register Reflection Metadata

**Files:**

- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/tests/ecs_tests.cpp`

- [ ] **Step 1: Add failing reflection assertions**

Add `test_core_component_reflection()` that:

1. Imports `ecs::CoreModule`.
2. Resolves `world.component<ecs::LocalTransform>()`.
3. Obtains its `flecs::MetaType` and verifies the component has members named `translation`, `rotation`, and `scale`.
4. Uses Flecs cursor/JSON reflection APIs to write `translation.x = 12.0f` into a `LocalTransform` value and verifies the typed value changed.
5. Verifies `WorldRuntimeState` exposes `status` and `content_generation`.

Use Flecs' v4.1.6 typed metadata API from the vendored header; do not inspect private ECS tables or hard-code component IDs.

- [ ] **Step 2: Run and observe the missing metadata failure**

Run `make -C MatterEngine3/tests run-ecs` through WSL.

Expected: the new metadata lookup checks fail.

- [ ] **Step 3: Register all core metadata**

In `CoreModule`, register `Float3`, `Quaternion`, `Mat4f`, `LocalTransform`, `WorldTransform`, `WorldStatus`, and `WorldRuntimeState` with `world.component<T>().member<...>(...)` / enum constants using the exact v4.1.6 API.

Metadata rules:

- `LocalTransform`: writable `translation`, `rotation`, `scale`.
- `WorldTransform`: reflected but documented as derived/read-only at the engine API level.
- `TransformDirty` and phase/pipeline tags: names only, no fields.
- `WorldStatus` constants: `Loading`, `Ready`, `Failed`.
- Do not enable or import Flecs REST.

- [ ] **Step 4: Run reflection tests**

Expected: all ECS checks pass, and serializing a `LocalTransform` to JSON produces named fields rather than raw bytes.

- [ ] **Step 5: Commit reflection support**

```powershell
git add MatterEngine3/src/ecs/ecs_runtime.cpp MatterEngine3/tests/ecs_tests.cpp
git commit -m "feat(ecs): reflect core runtime components"
```

---

### Task 4: Implement Transform Hierarchy and Validated Reparenting

**Files:**

- Create: `MatterEngine3/src/ecs/transform_math.h`
- Modify: `MatterEngine3/src/ecs/transform_system.cpp`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/tests/ecs_tests.cpp`

- [ ] **Step 1: Add failing transform and hierarchy tests**

Add tests covering:

- root TRS produces the expected row-major/column-vector `Mat4f`;
- parent translation `(10,0,0)` plus child translation `(0,2,0)` produces world translation `(10,2,0)`;
- a three-level hierarchy propagates changes from the root;
- `reparent(child, new_parent)` changes the result and dirties the subtree;
- `clear_parent(child)` preserves the local transform and recomputes it as a root;
- `reparent(root, grandchild)` returns `false` and leaves the hierarchy unchanged;
- destroying a parent uses Flecs' built-in `ChildOf` ownership semantics and cascade-deletes its descendants; callers must explicitly detach or reparent children they intend to preserve.

Use approximate float checks with an epsilon of `1e-5f`.

- [ ] **Step 2: Run and observe failures**

Expected: link failures for `reparent`/`clear_parent` or missing `WorldTransform` values.

- [ ] **Step 3: Implement deterministic TRS math**

Create `transform_math.h` with a pure `trs_matrix(const LocalTransform&)` function. Normalize a finite non-zero quaternion; use identity rotation for invalid/zero-length input. Emit a row-major matrix for column-vector algebra, with translation at indices `3`, `7`, and `11`, matching `matter::Mat4f` and `viewer::mat4_mul`.

- [ ] **Step 4: Implement safe hierarchy mutation**

In `transform_system.cpp`:

- Reject null, dead, cross-world, self-parent, and descendant-parent requests.
- Walk `flecs::ChildOf` ancestors before mutation; never rely on a Flecs abort for cycle detection.
- Apply `child.child_of(parent)` only after validation.
- `clear_parent` removes the existing `(ChildOf, *)` pair.
- Mark the changed entity and every current descendant with `TransformDirty`.
- Document `reparent`/`clear_parent` as the supported hierarchy mutation API; direct `ChildOf` edits bypass MatterEngine validation.

- [ ] **Step 5: Register dirty observers and propagation systems**

Register observers that mark the affected subtree dirty on `OnSet<LocalTransform>` and explicit parent removal. Parent destruction follows Flecs' built-in `ChildOf` cascade-delete policy. Register transform propagation in both `FixedPostUpdate` and `FrameUpdate`; dirty tags prevent duplicate work when both phases run in one frame.

Propagation rules:

```text
root WorldTransform  = TRS(LocalTransform)
child WorldTransform = parent WorldTransform × TRS(LocalTransform)
```

Traverse parents before children with a cached hierarchy query. If a parent lacks `WorldTransform`, compute/add it before the child. Remove `TransformDirty` only after a successful write. Perform structural writes inside Flecs deferral.

- [ ] **Step 6: Run the ECS test suite**

Expected: all hierarchy and transform checks pass; no Flecs assertion/abort occurs in the cycle case.

- [ ] **Step 7: Commit transform support**

```powershell
git add MatterEngine3/src/ecs MatterEngine3/tests/ecs_tests.cpp MatterEngine3/include/matter/ecs.h
git commit -m "feat(ecs): add hierarchical transform propagation"
```

---

### Task 5: Implement Fixed-Step and Frame Pipelines

**Files:**

- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/tests/ecs_tests.cpp`

- [ ] **Step 1: Add the public tick descriptor and stats fields**

Add before `FrameStats`:

```cpp
struct TickDesc {
    float frame_delta_seconds = 0.0f;
    float fixed_delta_seconds = 1.0f / 60.0f;
    uint32_t max_fixed_steps = 4;
};
```

Append to `FrameStats`:

```cpp
uint64_t ecs_fixed_steps = 0;
uint64_t ecs_dropped_steps = 0;
uint64_t ecs_invalid_ticks = 0;
```

Declare only `void tick(const TickDesc& tick);`.

- [ ] **Step 2: Add failing runtime scheduling tests**

Define private runtime result types in `ecs_runtime.h`:

```cpp
struct TickResult {
    uint32_t fixed_steps = 0;
    uint32_t dropped_steps = 0;
    bool invalid = false;
};
```

Test with recording systems that the exact order for one fixed step is:

```text
FixedPreUpdate, FixedUpdate, PrePhysics, Physics,
PostPhysics, FixedPostUpdate, FrameUpdate
```

Also test:

- half a fixed step runs only `FrameUpdate`;
- two accumulated steps run the six fixed phases twice, then frame once;
- `frame_delta_seconds=1.0`, `fixed_delta_seconds=0.1`, `max_fixed_steps=2` clamps the contribution to `0.25`, runs two, reports zero dropped steps, and preserves a `0.05` fractional remainder;
- `frame_delta_seconds=0.25`, `fixed_delta_seconds=0.01`, `max_fixed_steps=2` runs two, reports 23 dropped complete steps, and preserves no whole step;
- a later `0.05 + 0.05` pair proves fractional remainder preservation;
- negative/NaN frame delta, zero/NaN fixed delta, and zero max steps report invalid and run no systems;
- contributed frame time clamps to `0.25f`.

- [ ] **Step 3: Run tests and observe scheduling failures**

Expected: compile/link failures until `Runtime` exists.

- [ ] **Step 4: Implement the private runtime contract**

`ecs_runtime.h` declares a non-copyable `Runtime` owning:

```cpp
flecs::world world_;
flecs::entity fixed_pipeline_;
flecs::entity frame_pipeline_;
double accumulator_seconds_ = 0.0;
```

Public private-layer methods:

```cpp
flecs::world& world() noexcept;
const flecs::world& world() const noexcept;
TickResult tick(const TickDesc& desc);
void enqueue_world_state(WorldStateCommand command);
```

Build two Flecs pipelines. Fixed systems carry `FixedPipelineSystem`; frame systems carry `FramePipelineSystem`. Each system also uses its ordered custom phase via `kind(...)`. Run the fixed pipeline once per accumulator step with exactly `fixed_delta_seconds`, then the frame pipeline once with the clamped frame delta.

- [ ] **Step 5: Implement validation, clamping, and drop policy**

Use `double` for the accumulator and validate with `std::isfinite` before adding time. On invalid input return `{0,0,true}` without draining commands or progressing either pipeline. Clamp only the contributed frame delta to `0.25`. After the catch-up limit, calculate complete excess steps with `floor(accumulator / fixed_delta)`, subtract them, and report their count; keep the remaining fractional duration.

- [ ] **Step 6: Run the ECS test suite twice**

Run `run-ecs` twice to catch accidental static/global world state.

Expected: identical passing output both times.

- [ ] **Step 7: Commit scheduling**

```powershell
git add MatterEngine3/include/matter/world_session.h MatterEngine3/src/ecs MatterEngine3/tests/ecs_tests.cpp
git commit -m "feat(ecs): add deterministic session tick pipelines"
```

---

### Task 6: Integrate One ECS Runtime into Every WorldSession

**Files:**

- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/tests/world_stream_tests.cpp`

- [ ] **Step 1: Add failing session integration assertions**

Extend `world_stream_tests.cpp` to verify:

- `session->ecs()` resolves the `matter.ecs` module and the `WorldRuntimeState` singleton;
- a named runtime entity remains alive with the same ID after `reload()` completes;
- it remains alive after `regenerate(seed)` completes;
- successful publish sets `Ready` and increments `content_generation` exactly once;
- a forced fatal bake error sets `Failed` without deleting the runtime entity;
- a newly opened replacement session cannot resolve the old entity ID as alive.

Keep these checks in the existing session fixture so they reuse its GL/Vulkan setup rather than creating a second integration binary.

- [ ] **Step 2: Run the integration target and observe missing API failures**

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterEngine3/tests run-worldstream'
```

Expected: compilation fails because `WorldSession::ecs()` is not defined.

- [ ] **Step 3: Add session ownership and accessors**

Add `ecs_runtime::Runtime ecs_runtime;` to `WorldSession::Impl` as a value member so it is constructed once with the session and destroyed after the worker is joined. Declare/define:

```cpp
flecs::world& WorldSession::ecs() { return impl_->ecs_runtime.world(); }
const flecs::world& WorldSession::ecs() const { return impl_->ecs_runtime.world(); }
```

Include `matter/ecs.h` from `world_session.h` so the transitive Flecs dependency is intentional and visible.

- [ ] **Step 4: Bridge worker bake state through plain-data commands**

Use:

```cpp
enum class WorldStateCommandKind { Loading, Ready, Failed };
struct WorldStateCommand { WorldStateCommandKind kind; };
```

`enqueue_world_state` is mutex-protected and callable by bake workers. `Runtime::tick()` swaps the queue into a local vector and applies it on the tick thread. `Ready` increments `content_generation`; `Loading` and `Failed` do not. Cancelled/superseded bakes do not set `Failed`. Non-fatal per-part errors do not set `Failed` if a world generation publishes successfully.

- [ ] **Step 5: Replace the old tick body without changing provider ordering**

Move the existing provider/live-edit polling portion into a private `WorldSession::Impl::poll_runtime_sources()` helper. Implement public tick in this order:

```cpp
void WorldSession::tick(const TickDesc& desc) {
    const ecs_runtime::TickResult result = impl_->ecs_runtime.tick(desc);
    if (result.invalid) {
        ++impl_->stats.ecs_invalid_ticks;
        return;
    }
    impl_->stats.ecs_fixed_steps += result.fixed_steps;
    impl_->stats.ecs_dropped_steps += result.dropped_steps;
    impl_->poll_runtime_sources();
}
```

This preserves the approved rule that invalid ticks progress nothing. Keep `poll_runtime_sources()`'s existing bake-active early return and live-edit debounce behavior intact.

- [ ] **Step 6: Verify session lifecycle and state transitions**

Run `run-worldstream`.

Expected: existing sector assertions and all new ECS lifecycle assertions pass.

- [ ] **Step 7: Commit session ownership**

```powershell
git add MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEngine3/tests/world_stream_tests.cpp
git commit -m "feat(ecs): own ECS runtime in WorldSession"
```

---

### Task 7: Integrate Flecs into Engine, Test, and Windows Builds

**Files:**

- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `MatterViewer/Makefile`
- Modify: `ExplorerDemo/Makefile`

- [ ] **Step 1: Add Flecs and ECS sources to the MatterEngine3 library**

In `MatterEngine3/Makefile`:

- add `FLECS_DIR = ../Libraries/flecs` and `-I$(FLECS_DIR)` to includes;
- add `src/ecs/ecs_runtime.cpp` and `src/ecs/transform_system.cpp` to `ME3_CPP`;
- add `ecs_runtime.o transform_system.o` to `ME3_OBJ`;
- add `FLECS_C = $(FLECS_DIR)/flecs.c` and `FLECS_OBJ = flecs.o`;
- add a specific C rule using `gcc -std=c99 -O2 -I$(FLECS_DIR)`;
- add `$(FLECS_OBJ)` to `$(LIB)` prerequisites and archive members;
- add `$(FLECS_OBJ)` to `clean`;
- add `src/ecs` and `$(FLECS_DIR)` to the appropriate `vpath` lists.

Do not compile `flecs.c` as C++.

- [ ] **Step 2: Prove the engine library links**

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterEngine3 clean && make -C MatterEngine3 -j2'
```

Expected: `MatterEngine3/libmatter_engine3.a` contains `flecs.o`, `ecs_runtime.o`, and `transform_system.o`; no unresolved `ecs_*` symbols.

- [ ] **Step 3: Update session-bearing test flavors**

Ensure every test binary whose source union includes `matter_engine.cpp` also links the single `flecsc` object. Do this at the shared-object-list level (`VIEWER_LOGIC_OBJS` and `GPU_SHARED_OBJS`) rather than repeating `flecs.c` in individual link rules. Confirm `ecs_tests` links exactly one Flecs object.

- [ ] **Step 4: Update the temporary Windows direct-source builds**

For both `MatterViewer/Makefile` and `ExplorerDemo/Makefile`:

- add the two ECS C++ files to `WIN_ME3_CPP`;
- add `FLECS_DIR`, include path, C source/name/object variables;
- add `$(FLECS_DIR)` to C `vpath`;
- compile `flecs.c` with the existing MinGW C compiler, not the C++ compiler;
- link its one object into `W_ALL_OBJ`;
- clean the object through the existing build-directory removal.

This is temporary compatibility, not ownership: no ECS implementation is added to either application.

- [ ] **Step 5: Run clean Linux and Windows build gates**

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterEngine3/tests clean && make -C MatterEngine3/tests run-ecs'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterViewer clean && make -C MatterViewer windows -j2'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C ExplorerDemo clean && make -C ExplorerDemo windows -j2'
```

Expected: all three commands succeed. ExplorerDemo still builds in Phase 1 and is removed only after Phase 3 migration gates pass.

- [ ] **Step 6: Commit build integration**

```powershell
git add MatterEngine3/Makefile MatterEngine3/tests/Makefile MatterViewer/Makefile ExplorerDemo/Makefile
git commit -m "build(ecs): link Flecs across supported targets"
```

---

### Task 8: Migrate All WorldSession Tick Callers

**Files:**

- Modify: `MatterViewer/main.cpp`
- Modify: `MatterViewer/main_linux.cpp`
- Modify: `ExplorerDemo/main.cpp`
- Modify: every `MatterEngine3/tests/*.cpp` call site where the receiver is a `WorldSession`

- [ ] **Step 1: Enumerate only WorldSession call sites**

Run:

```powershell
rg -n "session->tick\(\)|WorldSession.*tick\(\)|\.tick\(\)" MatterViewer ExplorerDemo MatterEngine3\tests
```

Do not change `live_edit::Session::tick()` calls in `dev_live_edit_tests.cpp`; that is a different type and API.

- [ ] **Step 2: Migrate viewer loops to measured time**

At each viewer call site, pass the frame delta already measured by the application loop:

```cpp
matter::TickDesc tick{};
tick.frame_delta_seconds = frame_delta_seconds;
session->tick(tick);
```

If the local value is milliseconds, convert once to seconds. Do not call a second clock and do not couple the tick to the render camera.

- [ ] **Step 3: Migrate deterministic tests and ExplorerDemo**

For tests that only pump async work, use `TickDesc{0.0f}` so ECS frame systems run without accumulating fixed simulation. For endless-flight/session loops and ExplorerDemo, pass their existing real or simulated frame delta. Avoid inserting sleeps solely to advance ECS time.

- [ ] **Step 4: Prove the old WorldSession API is gone**

Run:

```powershell
rg -n "session->tick\(\)" MatterViewer ExplorerDemo MatterEngine3\tests
```

Expected: no matches. Any remaining `.tick()` matches must be inspected and confirmed to be another class.

- [ ] **Step 5: Build migrated callers**

Run `run-ecs`, `run-worldstream`, MatterViewer Windows, and ExplorerDemo Windows.

Expected: all compile and pass with no parameterless `WorldSession::tick()` declaration.

- [ ] **Step 6: Commit caller migration**

```powershell
git add MatterViewer/main.cpp MatterViewer/main_linux.cpp ExplorerDemo/main.cpp MatterEngine3/tests
git commit -m "refactor(ecs): pass explicit time to world sessions"
```

---

### Task 9: Complete Regression and Scope Gates

**Files:**

- Modify: `MatterEngine3/Makefile`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `docs/superpowers/specs/2026-07-17-flecs-ecs-foundation-design.md`

- [ ] **Step 1: Put the headless ECS suite in the standard engine test gate**

Add `$(MAKE) -C tests run-ecs` to `MatterEngine3/Makefile`'s `test` target before existing suites. Keep `run-ecs` separately invocable.

- [ ] **Step 2: Run focused correctness gates**

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterEngine3/tests run-ecs run-sectorstream run-worldstream'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterEngine3 test'
```

Expected: all selected tests pass.

- [ ] **Step 3: Run repository build gates**

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && ./build-all.sh'
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterViewer windows -j2'
```

Expected: `build-all.sh` and the clean Windows viewer target complete successfully.

- [ ] **Step 4: Perform a finite-world viewer smoke test**

Launch MatterViewer through the repository's normal run command, open an existing finite world, and verify:

- it reaches the same ready/rendered state;
- camera controls are unchanged;
- reload and regenerate still work;
- ECS cumulative counters advance but no ECS UI appears;
- no sector behavior is added to worlds that did not already use it.

Record the exact world used and result in the implementation handoff; do not commit screenshots or generated caches.

- [ ] **Step 5: Enforce Phase 1 scope mechanically**

Run:

```powershell
git diff b56286a --name-only
rg -n "GameNetworkingSockets|NetworkId|SectorStreaming|b3World_Step|ImGuizmo" MatterEngine3\include\matter\ecs.h MatterEngine3\src\ecs
```

Expected: the diff contains only files listed in this plan plus dependency artifacts; the scope query returns no matches.

- [ ] **Step 6: Update design status and commit the final gate wiring**

Change the design spec status to `Implemented — Phase 1 verified` only after every gate above passes.

```powershell
git add MatterEngine3/Makefile MatterEngine3/tests/Makefile docs/superpowers/specs/2026-07-17-flecs-ecs-foundation-design.md
git commit -m "test(ecs): gate the Flecs foundation"
```

---

## Final Verification Checklist

- [ ] `Libraries/flecs/VERSION` names 4.1.6 and hashes match the vendored files.
- [ ] `flecs.c` is compiled as C and exactly once per final binary/library.
- [ ] `WorldSession` exposes exactly one `flecs::world` and has no parameterless tick.
- [ ] ECS entities survive reload/regenerate and do not survive session replacement.
- [ ] Invalid tick descriptors advance neither ECS nor provider/live-edit polling.
- [ ] Fixed phase order, clamp, catch-up cap, drop count, and fractional remainder are tested.
- [ ] Transform tests cover roots, three levels, reparent, detach, destruction, and cycle rejection.
- [ ] Reflection tests enumerate and edit named fields without relying on raw IDs.
- [ ] Flecs REST, Box3D runtime, sector ECS, editor UI, scripting, persistence, and networking are absent.
- [ ] `run-ecs`, `run-worldstream`, `MatterEngine3 test`, `build-all.sh`, MatterViewer Windows, and ExplorerDemo Windows pass.
- [ ] An existing finite world renders with unchanged controls and output.
- [ ] `git status --short` is clean after the final commit.

## Implementation Handoff

Implement tasks in order; each task leaves a green, reviewable commit. If a Flecs v4.1.6 API name differs from the illustrative C++ spelling in this plan, consult the vendored `flecs.h`, use the public v4.1.6 equivalent, and update the test and implementation together without changing the behavioral contract. Stop and revise the design before adding any excluded Phase 2+ capability.
