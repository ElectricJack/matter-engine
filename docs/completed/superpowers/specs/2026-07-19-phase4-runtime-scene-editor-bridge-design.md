# Phase 4 Runtime Scene and Editor Bridge Design

**Date:** 2026-07-19  
**Status:** Approved design; implementation pending  
**Depends on:** Flecs ECS Phase 1, Box3D runtime Phase 2, sector streaming Phase 3  
**Includes:** the approved world-as-JS foundation and a narrow world-authored ECS bootstrap slice

## Summary

Phase 4 makes runtime physics visible and editable without turning procedural world
content into millions of ECS entities. It adds a dedicated dynamic ECS lane to the
existing renderer, completes the world-as-JS migration, lets worlds declare or
procedurally bootstrap runtime entities, and replaces one-off MatterViewer panels
with a generic Entities/Properties workflow.

The first playable milestone is physics-first. A normal procedural world can define
rendered rigid bodies, the user can select and edit them through a generic editor,
and Play/Pause/Step/Stop provides reset-on-Stop simulation. Persistent scripts and
live scripted spawning remain future work.

## Decisions of Record

| Question | Decision |
|---|---|
| First Phase 4 milestone | Rendered, editable physics entities |
| Playground packaging | Physics playgrounds are ordinary worlds, not Viewer-specific tools |
| Editor UI | Generic Entities and Properties windows; no Physics Playground window |
| Existing streaming UI | Fold into Properties as a specialized `SectorStreaming` component editor |
| Selection | Outliner and viewport picking share one selected scene identity |
| Simulation | Play, Pause, single Step, and Stop; Stop restores the pre-Play scene |
| Procedural/static rendering | Existing `WorldState` and sector/LOD resolver remain unchanged |
| Dynamic rendering | Dedicated stable-slot ECS lane inside the existing Vulkan scene |
| World format | Complete the approved world-as-JS migration; do not extend the retiring manifest |
| Entity authoring | Both declarative `static entities` and hermetic `buildEntities()` DSL |
| DSL lifetime | Load/regenerate-time bootstrap only; no persistent callbacks or live spawning yet |
| Runtime identity | `SceneEntityId`, separate from Flecs IDs and future network IDs |
| Testing | Fast headless/logic/build gates plus user-performed in-app acceptance |

## Scope

Phase 4 includes:

- the approved world-as-JS foundation migration;
- deterministic world-authored ECS bootstrap recipes;
- public scene identity and renderable-part components;
- a private ECS-to-renderer synchronization bridge;
- stable dynamic Vulkan instance slots and transform-only updates;
- renderer identity readback sufficient for viewport picking;
- editor selection, hierarchy/outliner, and reflected Properties UI;
- generic component add/remove/edit operations;
- translate/rotate/scale gizmos for selected runtime entities;
- Play/Pause/Step/Stop with reset-on-Stop;
- migration of Sector Streaming controls into Properties;
- one normal physics playground world used for manual acceptance.

Phase 4 does not include:

- persistent gameplay/editor QuickJS contexts;
- live scripted spawning during simulation;
- save-game or general scene serialization;
- network identity, replication, prediction, or rollback;
- joints, vehicles, ragdolls, or a character controller;
- one ECS entity per immutable procedural child, grass blade, rock, or sector item;
- a second renderer, asset store, physics world, or authoring format;
- screenshot, cinematic, long-flight, or performance acceptance automation.

## Architecture

Static procedural content and dynamic ECS content remain distinct until the Vulkan
scene consumes them:

```text
World JS roots -> bake/provider -> WorldState -> sector/LOD resolver --+
                                                                  +--> Vulkan scene
World JS entities -> EntityRecipe -> Flecs -> dynamic render bridge --+
                                      ^
                                      +--> Box3D fixed pipeline
```

The procedural lane keeps its compact bulk representation and existing caches. The
dynamic lane shares `PartStore`, loaded part assets, Vulkan part resources, culling,
lighting, ray tracing, and frame submission, but owns stable instance slots whose
transforms can be patched independently.

### Rejected alternatives

1. **Mirror moving bodies into `WorldState`.** Rejected because every physics move
   would bump `WorldState::version()`, invalidate resolver caches, and potentially
   re-bin the complete procedural world.
2. **Append dynamic entities to the resolved static vector each frame.** Rejected as
   the long-term contract because a single moving body would fingerprint, rebuild,
   and upload the combined instance set.
3. **Dedicated dynamic lane.** Selected because it preserves the bulk procedural
   path, supports stable picking identity, and scales transform updates with the
   number of changed ECS entities.

## Public ECS Scene Contract

New public data belongs under `matter::scene` (or an equivalently explicit public
namespace selected during implementation). Components remain renderer-opaque.

```cpp
struct SceneEntityId {
    uint64_t value = 0;
};

struct PartInstance {
    uint64_t part_hash = 0;
    bool visible = true;
    bool casts_shadow = true;
};

enum class PartInstanceErrorCode : uint8_t {
    None,
    MissingPart,
    PartUnavailable
};

struct PartInstanceError {
    PartInstanceErrorCode code = PartInstanceErrorCode::None;
    uint64_t part_hash = 0;
};
```

Display names use Flecs' world-owned entity-name facility rather than adding a
borrowed string component. Recipes and editor snapshots copy names into engine-owned
`std::string` values before mutation, and reconstruction restores them with the Flecs
name API.

`PartInstance` stores the resolved runtime asset identity. World JS and the editor
may present module names, but resolution to `part_hash` occurs at the world/session
boundary. No public component stores Vulkan objects, `PartStore*`, Box3D IDs,
worker pointers, renderer slots, or persistent raw Flecs IDs.

`SceneEntityId` is stable across Play/Stop reconstruction and within a loaded world
generation. It is not an asset ID, save-game ID, or network ID. Authored IDs derive
deterministically from world identity plus the authored string ID. Editor-created
IDs use a collision-checked session allocator and remain stable across the current
Play/Stop snapshot.

## Dynamic Render Bridge

The bridge is private to `WorldSession`/its ECS runtime integration. It evaluates
after physics and world-transform propagation so the renderer sees the current
fixed-step result.

Its source query requires:

- `SceneEntityId`;
- `WorldTransform`;
- `PartInstance` with `visible == true`.

The bridge retains only session-owned bookkeeping keyed by `SceneEntityId`:

- current Flecs entity lookup for editor/picking resolution;
- dynamic renderer slot;
- last submitted part hash and transform revision/fingerprint;
- recoverable publication error state.

Reconciliation behavior:

- a newly valid entity allocates a stable slot;
- transform-only changes patch that slot;
- a part change releases the old asset reference and binds the new part;
- hiding or removing `PartInstance` releases the slot;
- destroying the entity releases the slot;
- missing parts produce `PartInstanceError` and leave the entity alive but invisible;
- world reload/regenerate tears down dynamic slots before replacing bootstrap state;
- shutdown drains renderer-owned resources before destroying the ECS runtime.

Slot identity is internal and may be reused only after the renderer has completed
the relevant frame lifetime. `SceneEntityId`, not slot number, is the stable identity
presented to selection and picking.

### Renderer integration

`VkSceneRenderer` gains a dynamic-instance interface alongside the existing bulk
`update_instances` path. The interface supports add/bind, transform update,
visibility/removal, and frame-safe release. Static and dynamic instances use the
same part resources and cull/render pipelines.

The GPU-visible identity attachment/readback must return enough information to map
a clicked dynamic instance to `SceneEntityId`. Static procedural picking continues
to use its existing instance/query identity. A tagged result distinguishes static
world instances from dynamic scene entities.

## World-as-JS Foundation

Phase 4 incorporates the approved `2026-07-17-world-as-js-authoring-design.md`
foundation rather than extending `world.manifest`.

The migration performs a hard cut:

- `schemas/` becomes `objects/`;
- shared script libraries become a project-root peer;
- worlds move to `worlds/<Name>.js` and extend `World`;
- roots, lights, field settings, and parameters move into the World class;
- generated data moves under gitignored `.cache/<world>/`;
- all existing worlds migrate with root/light parity tests;
- the manifest parser and active `WorldData` source layout are deleted after parity.

Entity declarations extend this single world format; no `runtime.json`, physics
manifest, editor scene file, or compatibility authoring path is added.

## Entity Recipes

World bootstrap input normalizes to engine-owned recipes before any Flecs mutation.

```cpp
struct EntityRecipe {
    std::string authored_id;
    std::string display_name;
    std::optional<std::string> parent_authored_id;
    // Validated component values in a registry-owned representation.
};
```

The implementation may use typed variants rather than a generic value tree, but
validation must be driven by an explicit bootstrap registry. Flecs reflection
metadata informs editor fields; it is not by itself a persistence ABI.

### Declarative entities

```js
class PhysicsPlayground extends World {
  static roots = [
    { module: "PlaygroundFloor" },
  ];

  static entities = [{
    id: "falling-box",
    name: "Falling Box",
    components: {
      LocalTransform: { translation: [0, 10, 0] },
      PartInstance: { part: "Crate" },
      RigidBody: { type: "dynamic", mass: 1 },
      BoxCollider: { halfExtents: [0.5, 0.5, 0.5] },
    },
  }];
}
```

### Procedural bootstrap DSL

```js
class PhysicsPlayground extends World {
  buildEntities() {
    for (let x = -5; x <= 5; ++x) {
      this.entity({
        id: `box-${x}`,
        name: `Box ${x}`,
        components: {
          LocalTransform: { translation: [x * 1.2, 8, 0] },
          PartInstance: { part: "Crate" },
          RigidBody: { type: "dynamic" },
          BoxCollider: { halfExtents: [0.5, 0.5, 0.5] },
        },
      });
    }
  }
}
```

`static entities` and `buildEntities()` append to the same ordered recipe stream.
Both require unique authored string IDs. Parent references use those IDs, never
Flecs IDs. Duplicate IDs across the combined stream are errors.

The bootstrap QuickJS context is fresh, deterministic, and hermetic. It may consume
world parameters and the world seed through explicit bindings. It cannot retain ECS
component pointers, access wall-clock/network state, register frame callbacks, or
spawn entities after bootstrap. The context is destroyed after recipes are copied
into engine-owned data.

Part names referenced by recipes contribute graph/bake dependencies even though
they do not become procedural `WorldState` placements. Recipe evaluation therefore
occurs early enough to feed part resolution before transactional ECS instantiation.

## Transactional Bootstrap and World Lifecycle

The engine validates the entire recipe set before structural mutation:

1. evaluate world statics and `buildEntities()`;
2. normalize declarations into ordered recipes;
3. validate IDs, component names, fields, enums, ranges, and parent references;
4. resolve part-module references and include them in bake dependencies;
5. wait for the successful world publication boundary;
6. instantiate entities and hierarchy through the supported ECS APIs;
7. publish the new bootstrap generation atomically to editor selection/render bridges.

An invalid recipe rejects the candidate world scene without partially creating
entities. Failed reload/regenerate retains the prior usable scene. Successful
reload/regenerate exits Play first, removes old dynamic render slots, replaces the
bootstrap generation, and lets physics/render/streaming private state reconcile.

Closed worlds and worlds with no `entities`/`buildEntities()` remain valid and
create no bootstrap entities.

## Editor Model

MatterViewer owns editor presentation and interaction state. MatterEngine3 owns the
runtime components, scene identity, bridge behavior, recipe extraction, and safe
mutation APIs.

### Entities window

The generic outliner supports:

- hierarchy display;
- selection by `SceneEntityId`;
- search by display name/ID;
- create empty entity;
- duplicate selected entity and supported public components;
- delete selected entity;
- reparent through the engine hierarchy API.

Entities without `SceneEntityId` are runtime-internal and hidden by default. A debug
toggle may expose them read-only, but editor actions never assume every Flecs entity
is authorable.

### Properties window

Properties enumerates supported public components on the selected entity. Generic
reflected editors cover booleans, integers, floats, enums, vectors, quaternions, and
other explicitly registered value types. Add Component lists only components whose
construction/editing contract is safe and registered. Remove Component obeys the
engine lifecycle APIs and Flecs deferral rules.

Specialized component editors exist only where generic field editing is insufficient:

- `LocalTransform`: numeric editing and translate/rotate/scale gizmos;
- `PartInstance`: loaded-part asset picker and visibility diagnostics;
- physics components: validation, impulse, velocity, and teleport actions;
- `SectorStreaming`: attach/remove, camera-follow, status, counts, and recoverable errors.

After feature parity, the standalone Sector Streaming window is deleted. Future
component-specific tools extend the Properties registry instead of adding one-off
top-level windows.

### Selection and viewport picking

Outliner and viewport selection update one MatterViewer selection record containing
`SceneEntityId` plus world generation. Every use resolves that ID back to the current
Flecs entity. Stale selection clears on world-generation mismatch or entity removal.

Static procedural hits may continue to show read-only instance/part information.
Dynamic hits select the ECS entity and expose editable Properties. Selection itself
is editor state, not a replicated/gameplay component.

### Gizmos

The existing ImGuizmo integration becomes selection-generic and supports translate,
rotate, and scale. It retains the current-frame input arbitration and full-screen
interaction draw-list rules established in Phase 3.

- Edit/Pause mode writes supported transform edits through engine mutation APIs.
- During Play, moving a dynamic body queues a physics teleport.
- Unsupported hierarchy/physics scale edits fail closed with a visible diagnostic.
- Camera movement never implicitly changes the selected entity.

## Simulation Control and Reset-on-Stop

MatterViewer exposes Play, Pause, Step, and Stop in a shared editor toolbar, not a
physics-specific window.

### Edit

Physics fixed-step advancement is paused. Frame/editor systems may continue so
selection, Properties, gizmos, rendering, and sector streaming remain responsive.

### Play

Before the first simulation tick, the editor captures a scene snapshot of supported
editable entities keyed by `SceneEntityId`. It includes public authoring/runtime
inputs and hierarchy, but excludes derived or private state.

### Pause and Step

Pause stops fixed-step accumulation without freezing the editor. Step advances
exactly one configured fixed tick, including pre-physics reconciliation, Box3D,
post-physics transform publication, and the dynamic render bridge.

### Stop

Stop restores the pre-Play scene:

- entities created during Play are removed;
- entities deleted during Play are recreated;
- editable components and hierarchy return to captured values;
- selection resolves by `SceneEntityId` and is retained when possible;
- private Box3D bodies/shapes, renderer slots, statuses, and errors are rebuilt;
- no raw Flecs world snapshot or Box3D memory image is restored.

The snapshot schema is an explicit whitelist/registry, not arbitrary component-memory
serialization. This prevents pointers, callbacks, renderer handles, and transient
worker state from crossing the restore boundary.

World switch, successful reload, and regenerate perform Stop before changing the
world. Application shutdown may discard the snapshot without restoration.

## System Ordering

The fixed and frame pipelines preserve the existing Phase 1-3 contracts:

```text
FixedPreUpdate
  -> component/command reconciliation
  -> transform propagation
PrePhysics
  -> ECS-to-Box3D synchronization
Physics
  -> exactly one Box3D step
PostPhysics
  -> movement/contact/sensor publication
FixedPostUpdate
  -> physics-authored transform propagation

FrameUpdate
  -> sector streaming samples resolved anchor transform
  -> dynamic render bridge samples resolved scene transforms
```

Rendering remains outside ECS progression. `WorldSession::render()` consumes the
latest published static and dynamic scene state and never performs structural ECS
mutation.

## Error Handling

World-candidate errors reject publication transactionally:

- wrong World base class or malformed world statics;
- unknown entity/component/field names;
- duplicate or empty authored IDs;
- broken parent references or hierarchy cycles;
- invalid reflected values or unsupported component combinations;
- unresolved part modules required by entity recipes.

Runtime entity errors are recoverable and reflected:

- unavailable part asset leaves the entity alive but invisible;
- invalid rigid-body/collider data leaves physics disabled for that entity;
- unsupported dynamic-body hierarchy/scale fails closed;
- renderer capacity/allocation failure preserves ECS ownership and retries only
  through an explicit, bounded reconciliation path;
- stale picking IDs or scene generations clear selection without touching the world.

Failures must identify the world, authored entity ID, component, and field when that
context exists. Exceptions never unwind through Flecs C callbacks, Box3D callbacks,
renderer submission, or worker boundaries.

## Testing Strategy

Testing emphasizes fast targeted behavior rather than screenshot acceptance.

### World-as-JS and recipe tests

- world statics extraction does not execute field/runtime methods accidentally;
- every existing manifest migrates with root/light/flag parity;
- declarative and DSL forms normalize identically;
- deterministic ordering and IDs across repeated evaluation;
- duplicate ID, unknown component/field, bad enum/range, missing part, and bad parent errors;
- recipe part references contribute bake dependencies without `WorldState` placement;
- candidate failure leaves the previous runtime scene unchanged.

### Dynamic bridge tests

- add, transform-only update, part replacement, hide, remove, and destruction;
- stable slot identity and frame-safe reuse;
- moving bodies do not bump procedural `WorldState::version()` or resolver re-bin count;
- no-op frames perform no dynamic transform uploads;
- renderer capacity failure retains ownership and recovers without stale slots;
- picking identity maps back through `SceneEntityId` after entity reconstruction.

### Editor logic tests

- outliner filtering, hierarchy ordering, selection invalidation, duplicate/delete;
- generic field edits use supported reflected types and mutation boundaries;
- component add/remove registry rejects unsupported/internal components;
- custom Part/physics/streaming editor actions call engine-safe operations;
- gizmo input arbitration remains current-frame and operation-specific;
- standalone Sector Streaming UI is removed only after Properties parity.

### Simulation tests

- Play captures only after pending edits reconcile;
- Pause prevents accumulation; Step advances exactly one fixed tick;
- Stop restores transforms, components, hierarchy, deleted entities, and selection;
- Stop removes Play-created entities;
- private physics/render/status state is rebuilt rather than deserialized;
- world switch/reload/regenerate performs the required Stop boundary.

### Regression and build gates

- existing ECS, physics, streaming, async, and finite-world suites remain green;
- source graphs contain each new engine system exactly once;
- public headers remain free of Box3D/Vulkan/private renderer handles;
- Windows flattened source basenames remain unique;
- MatterViewer translation units and clean MinGW product build pass;
- available GNU builds remain lightweight compile/link gates;
- no ExplorerDemo or one-off Physics Playground application/window returns.

## Manual Acceptance

The user validates one ordinary `PhysicsPlayground` world:

1. The world opens with statically and procedurally declared ECS bodies.
2. Rendered dynamic boxes fall and collide with static bodies.
3. Outliner and viewport selection identify the same entity.
4. Properties edits reflected fields and shows validation errors.
5. Pause freezes physics while selection and gizmos remain responsive.
6. Step advances one visible fixed tick.
7. Gizmo movement of a dynamic body during Play queues a safe teleport.
8. Stop restores the exact pre-Play editable scene and removes Play-created entities.
9. Sector Streaming is controlled from Properties with Phase 3 behavior intact.
10. A finite world with no entity bootstrap opens and renders unchanged.

## Delivery Stages

The implementation plan should preserve these dependency stages:

1. World-as-JS foundation and parity migration.
2. Public scene components, stable scene identity, and bootstrap component registry.
3. Entity recipe schema, DSL extraction, validation, and transactional bootstrap.
4. Dynamic renderer slots and ECS bridge.
5. Picking and selection.
6. Generic Entities/Properties editor framework.
7. Simulation snapshot and Play/Pause/Step/Stop.
8. Component-specific Part/physics/streaming editors and standalone-panel retirement.
9. Physics playground world, regression closure, and manual acceptance handoff.

## Success Criteria

Phase 4 is complete when:

1. Existing worlds use the single world-as-JS format with manifest parity and no
   active manifest parser.
2. Worlds can bootstrap runtime ECS entities declaratively and through the hermetic
   DSL, with deterministic identities and transactional failure behavior.
3. Rendered ECS physics bodies update through a dedicated dynamic lane without
   invalidating procedural `WorldState` caches.
4. Dynamic entities are selectable from the viewport/outliner and editable through
   generic Properties.
5. Play/Pause/Step/Stop works and Stop restores the pre-Play editable scene.
6. Sector Streaming controls live in Properties and the one-off panel is retired.
7. A normal physics playground world satisfies the manual acceptance checklist.
8. Existing finite/procedural worlds, ECS, physics, streaming, and renderer behavior
   remain intact under focused regression and clean Windows product gates.
9. No persistent runtime scripting, networking, or second scene/authoring format is
   introduced.
