# MatterEngine3 Flecs ECS Foundation — Design Spec

**Date:** 2026-07-17
**Status:** Approved; Phase 1 implemented with mandatory GNU/MinGW regression and finite-world viewer verification pending

## Summary

MatterEngine3 will adopt Flecs as its entity-component-system foundation. A pinned
Flecs v4.1.6 release will be vendored under `Libraries/flecs/`, and every
`WorldSession` will own one Flecs world. MatterEngine3 owns ECS lifecycle,
foundational components, scheduling conventions, reflection metadata, and public
access; MatterViewer is a consumer and contains no ECS implementation.

Phase 1 establishes the ECS foundation only. Runtime Box3D physics, sector
streaming, render-scene synchronization, authoring, persistent runtime scripting,
and multiplayer replication are separate follow-on phases. This split makes the
foundation independently testable and prevents the original ExplorerDemo
consolidation from becoming an engine-wide rewrite.

## Motivation

The infinite-world migration exposed a missing runtime object model. Sector
streaming is currently driven by `WorldSession::set_bake_focus()` and applications
conventionally pass the camera position. That makes the capability camera-specific,
cannot express dynamic component attachment, and offers no reusable basis for
physics, editor objects, gameplay, or multiplayer.

A one-off streaming anchor would solve only the immediate problem. MatterEngine3
instead needs a general runtime model in which:

- entities are stable generational identifiers;
- components can be added and removed dynamically;
- systems operate over cache-friendly component queries;
- structural mutation is safe during iteration;
- transforms and hierarchy are shared by editor, physics, rendering, and streaming;
- fixed simulation ticks support future physics and network prediction;
- component metadata can drive MatterViewer inspectors and later replication;
- headless server worlds use the same simulation without graphics initialization.

Flecs supplies an archetype/SoA ECS, typed C++17 API, relationships, hierarchy,
cached queries, deferred mutation, modules, reflection, and scheduling. Adopting it
avoids maintaining a custom ECS while matching MatterEngine3's C++17 and
cross-platform requirements.

## Decisions of Record

| Question | Decision |
|---|---|
| ECS implementation | Vendor Flecs v4.1.6; do not build a custom ECS or use EnTT |
| Ownership | ECS belongs entirely to MatterEngine3; MatterViewer only consumes it |
| ECS world lifetime | Exactly one Flecs world per `WorldSession` |
| Reload semantics | ECS entities survive `reload()` and `regenerate()` |
| World-switch semantics | Destroying/replacing a `WorldSession` destroys its ECS world |
| Public API | Flecs is an intentional transitive public dependency; expose the session world rather than wrapping every Flecs feature |
| Threading | Main-thread ECS mutation in Phase 1; workers enqueue commands and never retain component pointers |
| Simulation | Session-owned fixed-step accumulator plus one variable frame update |
| Reflection | Flecs metadata drives tools; it is not a persistence or network wire contract |
| Physics | Runtime Box3D integration is Phase 2 |
| Streaming | Sector streaming becomes a dynamically attached ECS component in Phase 3 |
| Networking | GameNetworkingSockets is the preferred future transport; no networking dependency in Phases 1–3 |

## Dependency and Build Layout

The repository will vendor the complete pinned Flecs release:

```text
Libraries/flecs/
├── LICENSE
├── VERSION
├── flecs.h
└── flecs.c
```

`VERSION` records `4.1.6` and the upstream release URL or commit. The vendored
license is shipped unchanged. `flecs.h` and `flecs.c` are the official upstream
amalgamated distribution files, which keep the vendored and Windows source lists
small while retaining the selected addons.

MatterEngine3 compiles Flecs into `libmatter_engine3.a` on Linux. The existing
Windows viewer currently compiles MatterEngine3 sources directly, so its build list
will mechanically include Flecs until a later build-system cleanup produces and
links a Windows MatterEngine3 library. This duplication is build debt only: no ECS
behavior or component implementation lives in MatterViewer.

Flecs reflection and JSON facilities required for editor metadata are enabled.
Optional HTTP/REST explorer facilities are not part of Phase 1.

## Public API Boundary

`MatterEngine3/include/matter/ecs.h` is the single public entry point for the
engine's ECS conventions. It includes the vendored Flecs C++ API and declares the
MatterEngine3 components, phases, and module registration types.

`WorldSession` exposes its world by reference:

```cpp
flecs::world& ecs();
const flecs::world& ecs() const;
```

Game, editor, and later engine modules may use normal Flecs typed components,
queries, observers, relationships, and systems. MatterEngine3 does not create an
imitation wrapper for these features. The engine still owns world creation,
progression, and destruction; applications must not call `progress()` themselves.

Flecs entity IDs are session-local runtime identifiers. They must never be written
as asset IDs, save-game IDs, or network IDs. Future replication uses a separate,
stable `NetworkId` component.

## Foundational Components

Phase 1 registers a small `matter::ecs` module:

```cpp
namespace matter::ecs {

struct LocalTransform {
    Float3 translation{0.0f, 0.0f, 0.0f};
    Quaternion rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Float3 scale{1.0f, 1.0f, 1.0f};
};

struct WorldTransform {
    float matrix[16];
};

struct TransformDirty {};

struct WorldRuntimeState {
    enum class Status : uint8_t { Loading, Ready, Failed };
    Status status = Status::Loading;
    uint64_t content_generation = 0;
};

} // namespace matter::ecs
```

`Quaternion` is a public engine POD type added beside the existing camera/math
types. `WorldTransform` uses MatterEngine3's existing matrix convention and is
derived data: external systems read it but do not write it. `TransformDirty` is an
internal tag used to avoid unnecessary propagation. Flecs built-in names provide
editor-visible identity, and the built-in `ChildOf` relationship defines hierarchy.

The transform system computes root world transforms directly and child world
transforms from parent `WorldTransform × LocalTransform`. Reparenting, local
transform writes, and parent changes dirty the affected subtree. A hierarchy cycle
is rejected and reported before propagation.

## World and Entity Lifecycle

The Flecs world is created during successful `WorldSession` construction and the
`matter::ecs` module is imported before the session is returned to its caller.
Applications may then register their own components and systems.

Runtime entities persist through authored-content reloads and seed regeneration.
`WorldRuntimeState` transitions to `Loading` when a bake begins, `Ready` after the
new world publishes successfully, and `Failed` after a fatal bake failure. Its
`content_generation` increments for each successfully installed authored-world
generation. Systems that require authored content query this singleton and remain
inactive unless it is `Ready`.

A failed reload does not destroy runtime ECS entities. Existing rendering retains
the engine's current fail-closed behavior; ECS systems that are independent of
authored content may continue running.

Destroying the `WorldSession` stops ECS progression, flushes engine-owned cleanup,
and destroys the Flecs world. Entity handles from that session are invalid after
destruction. Switching worlds in MatterViewer creates a new session and therefore
a new ECS world.

## Tick and Scheduling Contract

The parameterless session tick becomes a time-aware contract:

```cpp
struct TickDesc {
    float frame_delta_seconds = 0.0f;
    float fixed_delta_seconds = 1.0f / 60.0f;
    uint32_t max_fixed_steps = 4;
};

void WorldSession::tick(const TickDesc& tick);
```

All callers and tests migrate to the explicit descriptor; no permanent
parameterless compatibility overload remains. `frame_delta_seconds` must be finite
and non-negative. `fixed_delta_seconds` must be finite and greater than zero.
`max_fixed_steps` must be at least one. Invalid input is rejected with a diagnostic
and does not progress simulation.

The session owns the accumulator. Each call performs:

1. Clamp the contributed frame delta to 0.25 seconds.
2. Drain thread-safe external engine commands into the main-thread world.
3. Run zero through `max_fixed_steps` fixed simulation steps.
4. If accumulated time still contains complete steps, drop the excess and increment
   a diagnostic counter rather than entering a spiral of death; preserve only the
   fractional remainder smaller than one fixed step.
5. Run one variable-rate frame update.
6. Poll and apply provider/bake results.

The ordered ECS phases are:

```text
FixedPreUpdate
FixedUpdate
PrePhysics
Physics
PostPhysics
FixedPostUpdate
FrameUpdate
```

Phase 1 registers the phase graph and transform systems but no physics systems.
Phase 2 inserts Box3D synchronization and stepping into the reserved physics phases.
Phase 3 evaluates sector streaming after its anchor's world transform is current.

Rendering remains a separate `WorldSession::render(...)` call. Applications cannot
advance the ECS from rendering, and render code cannot perform structural mutation.

`FrameStats` gains cumulative `ecs_fixed_steps`, `ecs_dropped_steps`, and
`ecs_invalid_ticks` counters. These make the catch-up and invalid-input policies
observable without adding callbacks or exceptions to the tick API.

## Threading and Mutation Rules

Phase 1 deliberately uses a single ECS thread even though Flecs can schedule work
across threads. This establishes deterministic ownership before physics and
networking introduce more concurrency.

- The thread calling `WorldSession::tick()` owns all Flecs access.
- Systems may add/remove components or entities through Flecs deferral.
- Engine worker threads communicate through engine-owned thread-safe queues.
- Worker threads never store `flecs::entity`, component references, query iterators,
  or pointers into archetype storage.
- Engine services may be referenced by stable context pointers registered on the
  world, but service objects remain owned by `WorldSession::Impl`.
- System ordering is expressed through the registered phase dependency graph, not
  incidental registration order.

Multithreaded Flecs scheduling is a future optimization that requires profiling and
an explicit design review.

## Reflection and Tooling Contract

Core components and their fields are registered with Flecs metadata. Phase 1 tests
that metadata can enumerate component names, field names, field types, and writable
values. MatterViewer does not yet render an inspector, but Phase 4 can consume the
metadata without changing component definitions.

Reflection supports:

- component inspectors and property editing;
- axis-gizmo binding to transform fields;
- debug serialization and test fixtures;
- future replication-schema generation.

Reflection does not guarantee persistence or network compatibility. Those formats
use explicit stable type names, field IDs, versions, and serializers. Raw component
bytes, Flecs component IDs, and Flecs entity IDs are never external formats.

## Phase 1 Scope

Phase 1 includes:

- pinned Flecs vendoring, license, and build integration;
- one ECS world per `WorldSession`;
- public ECS access and engine module registration;
- entity and component lifecycle;
- local/world transforms and hierarchy;
- deferred structural mutation;
- fixed-step and frame-step phases;
- world runtime loading state;
- core component reflection;
- headless ECS tests and minimal session lifecycle integration;
- clean Linux and MinGW Windows build gates.

Phase 1 excludes:

- Box3D runtime physics;
- sector streaming conversion;
- converting baked world parts or render instances into entities;
- MatterViewer ECS panels, selection, or gizmos;
- JavaScript-authored runtime components;
- save-game/persistence formats;
- GameNetworkingSockets or replication;
- Flecs multithreaded scheduling.

Existing worlds and MatterViewer output must remain unchanged.

## Testing

### Headless ECS tests

Tests construct the engine ECS runtime without GL, Vulkan, a window, or authored
world data and verify:

1. create/destroy and stale generational entity handles;
2. runtime add/remove and archetype movement;
3. typed queries with required, optional, and excluded components;
4. structural mutation during iteration is deferred and applied afterward;
5. root and multi-level child transform propagation;
6. reparenting and parent destruction;
7. hierarchy-cycle rejection;
8. reflected component/field enumeration and transform field writes;
9. deterministic phase order;
10. fixed accumulator behavior, frame-delta clamp, catch-up limit, and dropped-step
    diagnostic;
11. invalid tick descriptors do not progress the world.

### WorldSession integration tests

A minimal fixture verifies:

- the ECS world and core module exist after `open_world()`;
- runtime entities survive `reload()` and `regenerate()`;
- `WorldRuntimeState` transitions and content generations match bake events;
- session teardown destroys the Flecs world and registered engine contexts; callers
  are forbidden from retaining or dereferencing entity handles after teardown;
- creating a replacement session starts with a fresh ECS world.

### Build and regression gates

- MatterEngine3 library and headless tests build and pass on Linux.
- MatterViewer's clean MinGW Windows target builds with the pinned Flecs source.
- Existing MatterEngine3 tests pass after migrating to `TickDesc`.
- MatterViewer opens and renders an existing finite world without visual or control
  changes.

## Follow-on Roadmap

### Phase 2 — Box3D runtime physics

Create one persistent Box3D world per `WorldSession`. Flecs components reference
opaque `b3BodyId`/shape handles while Box3D retains ownership of solver data.
Pre-physics systems create/destroy bodies and synchronize static/kinematic state;
`Physics` calls `b3World_Step()` once per fixed tick; post-physics systems consume
contiguous movement/contact/sensor events and update dynamic transforms.

Box3D `userData` never points into movable Flecs component storage. A stable bridge
record maps each physics object to its Flecs entity. Structural changes caused by
contacts are deferred. Dynamic rigid bodies cannot inherit arbitrary scaled parent
transforms; Phase 2 defines and enforces the supported hierarchy rules.

### Phase 3 — Sector streaming and ExplorerDemo consolidation

Convert the existing `SectorStreamer` into an engine ECS system. A dynamically
attached `SectorStreaming` component requires a resolved `WorldTransform`; exactly
one such component is permitted per client session. Its position, not the render
camera, determines the desired sector set. Adding the component starts streaming;
removing it evicts all component-owned sectors.

MatterViewer adds controls to bind the entity transform to the editor camera or
detach it and manipulate it through an axis gizmo while viewing loads and evictions
externally. Existing endless-flight, bounded-residency, rung-swap, and cleanup gates
move from ExplorerDemo to MatterEngine3/MatterViewer tests. Once those gates pass,
ExplorerDemo and its build, packaging, smoke, and `build-all.sh` wiring are deleted.

### Phase 4 — Runtime scene and editor bridge

Add ECS cameras, render views, lights/environment components, selection state, and
dynamic `PartInstance`/`RenderProxy` entities. A synchronization system writes ECS
dynamic objects into the existing bulk `WorldState`/`PartStore`/Vulkan scene.

High-volume immutable procedural children remain compact baked/render data; they do
not become one Flecs entity per grass blade, rock, or sector child. Objects that need
gameplay, editor, physics, or network identity become ECS entities.

### Phase 5 — World authoring integration

Align the ECS runtime with the world-as-JS roadmap. Define which authored world roots
instantiate ECS entities and which remain immutable baked content. Do not create a
second authoring format.

### Phase 6 — Persistent runtime scripting

Add a persistent gameplay/editor QuickJS context that operates on ECS entities and
components. Keep it separate from fresh hermetic bake contexts.

### Phase 7 — Multiplayer replication

Use GameNetworkingSockets as the preferred cross-platform transport. Add a separate
MatterEngine3 replication layer with stable `NetworkId`, authority, ownership,
prediction/interpolation, versioned component schemas, snapshots, RPCs, and interest
management. Never transmit raw Flecs IDs or component memory. Sector residency stays
client-local and follows the locally controlled/predicted streaming entity.

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Flecs leaks through too much engine code | Treat it as the deliberate runtime model, but keep bake/render services behind existing MatterEngine3 APIs |
| ECS becomes a service locator | Only object-like mutable runtime state becomes components; pipelines, caches, renderers, and providers remain services |
| Every render instance becomes an entity | Keep immutable procedural placements in bulk `WorldState`; bridge only dynamic/editor/gameplay objects |
| Tick migration destabilizes bake tests | Convert callers mechanically first, then add ECS progression; preserve provider/bake ordering with explicit integration tests |
| Reload destroys gameplay state | Keep the Flecs world alive and expose authored-content readiness through `WorldRuntimeState` |
| Reflection becomes an accidental file/network ABI | Require explicit stable schemas and serializers for every external format |
| Flecs version churn | Pin v4.1.6, record upstream identity, and upgrade only through a dedicated compatibility change |
| Windows direct-source list diverges again | Add a build gate now; later replace it with a Windows MatterEngine3 library target |

## Success Criteria

Phase 1 is complete when:

1. MatterEngine3 owns a pinned, licensed Flecs integration.
2. Every `WorldSession` exposes and progresses exactly one ECS world.
3. Core lifecycle, transform, hierarchy, deferral, reflection, and scheduling tests
   pass headlessly.
4. Runtime entities survive reload/regeneration and die with the session.
5. Existing finite worlds and MatterViewer rendering remain unchanged.
6. Linux tests and a clean MinGW Windows MatterViewer build pass.
7. No Box3D runtime, sector-streaming ECS conversion, editor UI, or networking code
   has leaked into Phase 1.
