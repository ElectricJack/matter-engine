# Phase 4 Runtime Scene Editor Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ordinary MatterEngine3 worlds able to author, simulate, render, select, and edit ECS entities—including a playable physics playground—while retaining the existing procedural world lane and removing one-off editor panels.

**Architecture:** World JavaScript is evaluated once into engine-owned `WorldDefinition` and `EntityRecipe` values. Static roots continue through `WorldState` and the sector resolver; dynamic ECS entities flow through a stable-slot dynamic renderer lane after transform/physics publication. MatterEngine3 owns runtime state and safe mutation/snapshot APIs, while MatterViewer owns generic outliner, Properties, gizmo, and simulation-control presentation.

**Tech Stack:** C++17, Flecs, Box2D/Box3D C API already vendored as Box3D, QuickJS-ng, Vulkan, GLFW, Dear ImGui, ImGuizmo, GNU Make/MSYS2 UCRT64, PowerShell static checks.

## Global Constraints

- Sector streaming remains an optional ECS component that may be attached dynamically to exactly one anchor per client; it is never inherently camera-owned.
- Physics remains one private physics context per `WorldSession`; no Box3D IDs or pointers enter public components.
- Static procedural content remains `WorldState -> resolver`; moving ECS bodies must not bump `WorldState::version()` or force procedural re-binning.
- Dynamic rendering uses stable internal slots keyed externally by `SceneEntityId`, with frame-safe reuse and no Vulkan handles in public ECS components.
- Display names use Flecs-owned entity names; copied recipes and snapshots own their strings.
- World authoring has one source format: `worlds/<Name>.js`; no runtime JSON, physics manifest, editor scene file, or compatibility manifest path remains after migration.
- Project-local `shared-lib/` is searched before the existing engine-owned `MatterEngine3/shared-lib`; engine DSL support is not duplicated into projects.
- `Meadow` and `MeadowWorld` remain distinct selectable world identities during the hard cut because both exist today; their declarations migrate independently.
- `static entities` and load-time `buildEntities()` normalize into the same ordered `EntityRecipe` stream; runtime scripting and live script spawning are deferred.
- Authored entity IDs are non-empty and unique across both declaration forms; parent references use authored IDs, never Flecs IDs.
- Candidate bootstrap is transactional: any validation/publication failure leaves the previous usable runtime scene intact.
- Play snapshots whitelist public editable values and hierarchy; Stop reconstructs private physics/render state and never restores raw Flecs or Box3D memory.
- Automated gates prioritize fast CPU/unit/static checks. The user performs visual and in-app acceptance; no screenshot, cinematic, or long-flight automation is added.
- ExplorerDemo and one-off Physics Playground/Sector Streaming windows must not return.

## File and ownership map

- `MatterEngine3/include/matter/world_definition.h`: public engine-owned world statics and entity recipes.
- `MatterEngine3/include/matter/scene.h`: public scene identity, renderable components, editor-safe records, mutations, snapshots, and simulation mode.
- `MatterEngine3/src/script/world_definition_loader.{h,cpp}`: QuickJS world statics plus hermetic bootstrap extraction.
- `MatterEngine3/src/ecs/scene_registry.{h,cpp}`: component registration, validation, recipe instantiation, reflection/edit metadata, snapshot capture/restore.
- `MatterEngine3/src/ecs/dynamic_scene_bridge.{h,cpp}`: Flecs query reconciliation into renderer-neutral stable-slot commands.
- `MatterEngine3/src/render/dynamic_instance_slots.{h,cpp}`: CPU-testable stable slot allocation/change tracking.
- `MatterEngine3/src/render/vk_scene_renderer.{h,cpp}`: Vulkan consumption of static instances plus dynamic slots.
- `MatterEngine3/src/matter_engine.cpp`: `WorldSession` lifecycle orchestration only; new domain logic stays in focused files above.
- `MatterViewer/editor_model.{h,cpp}`: selection, hierarchy/outliner filtering, and editor commands without ImGui dependencies.
- `MatterViewer/properties_registry.{h,cpp}`: generic and specialized component editor descriptors/actions.
- `MatterViewer/ui.{h,cpp}`: ImGui presentation, generic gizmos, toolbar; no world-specific physics window.

---

### Task 1: World JavaScript statics contract

**Files:**
- Create: `MatterEngine3/include/matter/world_definition.h`
- Create: `MatterEngine3/src/script/world_definition_loader.h`
- Create: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/src/script_host.h`
- Modify: `MatterEngine3/src/script_host.cpp`
- Test: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Produces: `matter::WorldRoot`, `WorldLight`, `WorldSettings`, `EntityRecipe`, `WorldDefinition`; `load_world_definition(const WorldLoadDesc&, WorldDefinition&, WorldLoadError&)`.
- `WorldLoadDesc` contains `world_path`, `objects_dir`, `project_shared_lib_dir`, `engine_shared_lib_dir`, `world_seed`, and canonical parameter JSON.
- This task extracts raw entity declarations as owned JSON strings; typed component validation is Task 4.

- [x] **Step 1: Add a failing focused evaluator test.** Cover base-class rejection, roots/lights/settings extraction, absence of accidental `field()` execution, project-before-engine shared-library lookup, and no-entity worlds. Use temporary fixtures whose `field()` throws if invoked.

- [x] **Step 2: Run the red test.**

  Run: `make -C MatterEngine3/tests run-world-definition`

  Expected: compile failure because `matter/world_definition.h` and `load_world_definition` do not exist.

- [x] **Step 3: Define the owned contract and loader.** The public shape must include:

  ```cpp
  struct WorldRoot { std::string module; std::string params_json = "{}"; Mat4f transform{}; bool expand = false; bool tileset = false; };
  struct WorldLight { Float3 position{}; Float3 color{1,1,1}; float intensity = 1.0f; float range = 10.0f; };
  struct RawEntityRecipe { std::string authored_id, display_name, parent_authored_id, components_json; };
  struct WorldDefinition { std::vector<WorldRoot> roots; std::vector<WorldLight> lights; std::vector<RawEntityRecipe> entities; WorldSettings settings{}; };
  bool load_world_definition(const WorldLoadDesc&, WorldDefinition&, WorldLoadError&);
  ```

  Evaluate only the selected class statics plus optional `buildEntities()` in a fresh QuickJS context. Bind `this.entity(record)` to append into the same ordered array after `static entities`. Copy all strings before destroying the context.

- [x] **Step 4: Make lookup and execution hermetic.** Permit only explicit module roots and seed/parameter bindings; do not expose clock, network, ECS pointers, frame callbacks, or post-load spawning handles. Return source location and property path in `WorldLoadError`.

- [x] **Step 5: Run focused and existing evaluator tests.**

  Run: `make -C MatterEngine3/tests run-world-definition run-evalworld run-script`

  Expected: all PASS.

- [x] **Step 6: Commit.**

  ```bash
  git add MatterEngine3/include/matter/world_definition.h MatterEngine3/src/script MatterEngine3/src/script_host.h MatterEngine3/src/script_host.cpp MatterEngine3/tests/world_definition_tests.cpp MatterEngine3/tests/Makefile
  git commit -m "feat(world): evaluate world JavaScript definitions"
  ```

### Task 2: Hard-cut runtime paths to the project layout

**Files:**
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/provider/local_provider.h`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/src/resolve_cache.h`
- Modify: `MatterEngine3/src/resolve_cache.cpp`
- Modify: `MatterViewer/ui.h`
- Modify: `MatterViewer/ui.cpp`
- Modify: `MatterViewer/main.cpp`
- Modify: `MatterViewer/main_linux.cpp`
- Test: `MatterEngine3/tests/world_definition_tests.cpp`
- Test: `MatterEngine3/tests/resolve_cache_tests.cpp`

**Interfaces:**
- Consumes: `load_world_definition` from Task 1.
- Produces: preferred `WorldDesc { project_dir, world_name, engine_shared_lib_dir, enable_live_edit }`; internal `LocalProviderConfig` paths derived as `objects/`, `worlds/`, `shared-lib/`, `.cache/<world>/`.
- Transitional compile seam: retain the existing `schemas_dir`, `world_data_dir`, and `shared_lib_dir` members through Task 2 only, used solely when `project_dir == nullptr` by still-unmigrated tests. MatterViewer and every newly written test must use `project_dir`. Task 3 migrates the remaining fixtures and deletes the fields and fallback in the same commit, so no compatibility authoring path survives the migration stage.

- [x] **Step 1: Write failing path and cache-key tests.** Assert world source is `<project>/worlds/<name>.js`, object sources are `<project>/objects`, outputs are under `<project>/.cache/<name>`, project and engine shared libraries affect the cache fingerprint, and stale `WorldData/world.manifest` is ignored.

- [x] **Step 2: Run red gates.**

  Run: `make -C MatterEngine3/tests run-world-definition run-resolvecache`

  Expected: failures showing legacy `schemas_dir/world_data_dir` routing.

- [x] **Step 3: Replace production path plumbing.** Update Viewer discovery to enumerate `.js` files directly in `worlds/`. Derive all project paths once in `open_world`; keep `engine_shared_lib_dir` as the engine support tier and treat missing project `shared-lib/` as empty. Add the explicitly temporary `project_dir == nullptr` test fallback described above without routing MatterViewer through it.

- [x] **Step 4: Replace provider manifest input with `WorldDefinition`.** Convert `WorldRoot` values to existing graph `ChildRequest` values and lights/settings to current runtime structures. Preserve root order, transforms, params, `expand`, and `tileset` semantics.

- [x] **Step 5: Move cache outputs and fingerprint inputs.** Cache identity includes world JS, recursively used object sources, project shared sources, engine shared sources, and canonical parameters. It never hashes stale manifests.

- [x] **Step 6: Run focused tests and Viewer logic compile.**

  Run: `make -C MatterEngine3/tests run-world-definition run-resolvecache run-viewer-logic`

  Expected: all PASS.

- [x] **Step 7: Commit.**

  ```bash
  git add MatterEngine3/include/matter/world_session.h MatterEngine3/src/provider MatterEngine3/src/matter_engine.cpp MatterEngine3/src/resolve_cache.* MatterViewer MatterEngine3/tests
  git commit -m "refactor(world): use project-root JavaScript layout"
  ```

### Task 3: Migrate fixtures and delete manifest authoring

**Files:**
- Rename: `MatterEngine3/examples/world_demo/schemas/` to `MatterEngine3/examples/world_demo/objects/`
- Create: `MatterEngine3/examples/world_demo/worlds/*.js`
- Create: `MatterEngine3/examples/world_demo/shared-lib/.gitkeep`
- Modify: `.gitignore`
- Modify: all test fixtures and tests returned by `rg -l "world\.manifest|schemas_dir|world_data_dir" MatterEngine3/tests MatterViewer`
- Delete: `MatterEngine3/examples/world_demo/WorldData/`
- Modify: `MatterEngine3/src/world_lights.h`
- Modify: `MatterEngine3/src/world_lights.cpp`
- Modify: `MatterEngine3/src/part_graph.h`
- Modify: `MatterEngine3/src/part_graph.cpp`

**Interfaces:**
- Consumes: hard-cut layout from Task 2.
- Produces: zero active callers or definitions of `PartGraph::read_manifest`; migrated Demo, Meadow, MeadowWorld, CornellBox, LightingGarden, FloorDemo, RockGallery, and four StressForest worlds.
- Produces: deletion of Task 2's transitional `WorldDesc` legacy fields and `project_dir == nullptr` fallback after every fixture is migrated.

**Execution subdivision:** Task 3 remains one atomic final hard cut but is executed
through four sequential review gates because the legacy inventory spans 27 files.
The parser and compatibility fallback remain intact through 3A-3C and are deleted only
in 3D after all consumers compile on the new format.

- **3A — Example parity:** move `schemas/` to `objects/`, create all 11 distinct
  `worlds/*.js`, update ignore rules, and land the parity tests.
- **3B — Async fixtures:** migrate `MatterEngine3/tests/async_bake_tests.cpp` only.
- **3C — Demand/streaming fixtures:** migrate `demand_bake_tests.cpp`,
  `refine_loop_tests.cpp`, and `transient_tests.cpp`.
- **3D — Closure:** migrate the remaining smaller API/example/gallery/lighting/
  tileset/viewer/world-stream/resolve-cache fixtures and Makefiles, then delete
  `read_manifest`, `parse_lights`, legacy descriptor/provider fields, the temporary
  fallback, active `WorldData`, and manifest-only targets. Run Task 3's full gates only
  after 3D.

- [ ] **Step 1: Add parity assertions before deleting fixtures.** Encode the current root module, params, transform, flags, lights, and field settings as expected `WorldDefinition` values for every example world. Keep `Meadow` and `MeadowWorld` separate.

- [ ] **Step 2: Run parity tests red against not-yet-created world files.**

  Run: `make -C MatterEngine3/tests run-world-definition`

  Expected: missing `worlds/<name>.js` failures.

- [ ] **Step 3: Move object sources and create world classes.** Each file exports one `class <Name> extends World` with declarative statics matching its old manifest and any existing field-world class behavior. Add `.cache/` to the example/project ignore rules.

- [ ] **Step 4: Convert all temporary test sandboxes.** Fixture helpers create `objects/`, `worlds/Test.js`, optional `shared-lib/`, and `.cache/`; update `WorldDesc` construction to the Task 2 contract. Then delete `schemas_dir`, `world_data_dir`, and `shared_lib_dir` from `WorldDesc`/provider configuration and remove the temporary fallback before running the task gates.

- [ ] **Step 5: Remove manifest parser and legacy layout code.** Delete `read_manifest` and `parse_lights`, while retaining `WorldLights`, `SpotLight`, and `lights_fingerprint` for runtime rendering. Delete active `WorldData` fixtures and manifest-only test targets. Retain unrelated instance-record names such as `manifest_idx` only if they are internal indices with no authoring dependency; otherwise rename them to `root_idx`.

- [ ] **Step 6: Run migration and regression gates.**

  Run: `make -C MatterEngine3/tests run-world-definition run-graph run-graph-integration run-asyncbake run-demandbake run-resolvecache run-worldstream`

  Expected: all available gates PASS; `rg -n "read_manifest|world\.manifest|WorldData/" MatterEngine3/src MatterEngine3/include MatterViewer MatterEngine3/examples MatterEngine3/tests` returns no active authoring code.

- [ ] **Step 7: Commit.**

  ```bash
  git add -A MatterEngine3 MatterViewer .gitignore
  git commit -m "refactor(world): retire manifest authoring"
  ```

### Task 4: Public scene components and reflected registry

**Files:**
- Create: `MatterEngine3/include/matter/scene.h`
- Create: `MatterEngine3/src/ecs/scene_registry.h`
- Create: `MatterEngine3/src/ecs/scene_registry.cpp`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Test: `MatterEngine3/tests/scene_registry_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Produces: `matter::scene::SceneEntityId`, `PartInstance`, `PartInstanceError`, `ComponentKind`, `FieldDescriptor`, `ComponentDescriptor`, `SceneRegistry`.
- `SceneRegistry::validate(const RawEntityRecipe&, EntityRecipe&, RecipeError&)`, `instantiate(flecs::world&, span<const EntityRecipe>, SceneGeneration&, RecipeError&)`, and lookup/mutation functions keyed by `SceneEntityId`.

- [ ] **Step 1: Write failing registry tests.** Cover Flecs reflection registration for transform, render, rigid-body, velocity, four collider types, and sector streaming; reject unknown component/field, invalid enum/range, multiple colliders, empty/duplicate IDs, missing parent, and cycles.

- [ ] **Step 2: Run red test.**

  Run: `make -C MatterEngine3/tests run-scene-registry`

  Expected: missing scene registry types.

- [ ] **Step 3: Add renderer-opaque public components.** Use exactly:

  ```cpp
  struct SceneEntityId { uint64_t value = 0; };
  struct PartInstance { uint64_t part_hash = 0; bool visible = true; bool casts_shadow = true; };
  enum class PartInstanceErrorCode : uint8_t { None, MissingPart, PartUnavailable, RendererCapacity };
  struct PartInstanceError { PartInstanceErrorCode code = PartInstanceErrorCode::None; uint64_t part_hash = 0; };
  ```

  Register public types in one module. Do not expose renderer slots, Vulkan handles, Box3D IDs, pointers, or borrowed strings.

- [ ] **Step 4: Implement metadata and validation.** Each editable component has a descriptor with add/remove safety, field type/range/enum metadata, copy-to-record, apply-from-record, and validation callbacks. Component names in JS match the public type names.

- [ ] **Step 5: Implement deterministic identity.** Authored IDs hash canonical world identity plus authored ID with collision detection. Session-created IDs use a monotonic collision-checked allocator with the high bit set. Runtime Flecs IDs are never serialized as stable identity.

- [ ] **Step 6: Run tests and public-header static scan.**

  Run: `make -C MatterEngine3/tests run-scene-registry run-ecs run-physics`

  Expected: all PASS and `rg -n "Vk|b2BodyId|b2ShapeId|Box3D|void\*" MatterEngine3/include/matter/scene.h` returns no matches.

- [ ] **Step 7: Commit.**

  ```bash
  git add MatterEngine3/include/matter/scene.h MatterEngine3/src/ecs MatterEngine3/tests
  git commit -m "feat(scene): add reflected ECS scene registry"
  ```

### Task 5: Recipe normalization and transactional bootstrap

**Files:**
- Modify: `MatterEngine3/include/matter/world_definition.h`
- Modify: `MatterEngine3/src/script/world_definition_loader.cpp`
- Modify: `MatterEngine3/src/ecs/scene_registry.h`
- Modify: `MatterEngine3/src/ecs/scene_registry.cpp`
- Modify: `MatterEngine3/src/provider/local_provider.h`
- Modify: `MatterEngine3/src/provider/local_provider.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Test: `MatterEngine3/tests/entity_recipe_tests.cpp`
- Test: `MatterEngine3/tests/async_bake_tests.cpp`

**Interfaces:**
- Produces: typed `EntityRecipe` component variants and `SceneBootstrapCandidate`; `WorldSession` publishes a candidate only at a successful bake publication boundary.
- Recipe part references contribute graph dependencies but never `WorldState` placements.

- [ ] **Step 1: Write failing equivalence and failure tests.** Declarative and DSL entities with the same records must normalize identically. Cover stable ordering/IDs, duplicate IDs across forms, bad parent, missing part, part dependency without placement, and failed reload retaining the prior generation/entity set.

- [ ] **Step 2: Run red tests.**

  Run: `make -C MatterEngine3/tests run-entity-recipes run-asyncbake`

  Expected: missing normalization/bootstrap behavior.

- [ ] **Step 3: Normalize before mutation.** Convert raw JSON through `SceneRegistry::validate` into owned typed variants. Resolve `PartInstance.part` module names through the graph result, replacing them with `part_hash`. Produce errors containing world, authored ID, component, and field.

- [ ] **Step 4: Extend graph dependency collection.** Add recipe part modules as bake/load roots tagged `placement=false`; exclude them when constructing `WorldState` instances.

- [ ] **Step 5: Publish transactionally.** Build candidate recipes and all dependencies off to the side. On successful provider publication, exit Play if needed, drain old dynamic slots, instantiate a fresh authored scene generation, then publish generation/editor lookup atomically. On failure, emit the error and retain the prior usable ECS scene and render state.

- [ ] **Step 6: Run focused and lifecycle tests.**

  Run: `make -C MatterEngine3/tests run-entity-recipes run-scene-registry run-asyncbake run-demandbake`

  Expected: all PASS.

- [ ] **Step 7: Commit.**

  ```bash
  git add MatterEngine3/include/matter/world_definition.h MatterEngine3/src/script MatterEngine3/src/ecs MatterEngine3/src/provider MatterEngine3/src/matter_engine.cpp MatterEngine3/tests
  git commit -m "feat(scene): bootstrap authored ECS entities transactionally"
  ```

### Task 6: CPU stable dynamic instance slots

**Files:**
- Create: `MatterEngine3/src/render/dynamic_instance_slots.h`
- Create: `MatterEngine3/src/render/dynamic_instance_slots.cpp`
- Test: `MatterEngine3/tests/dynamic_instance_slots_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Produces: `DynamicInstanceInput { SceneEntityId id; uint64_t part_hash; Mat4f object_to_world; bool casts_shadow; }`, `DynamicSlotHandle { index, generation }`, and `DynamicInstanceSlots::{upsert,remove,finish_frame,drain,changes}`.
- Slot reuse is deferred until the caller-provided completed frame serial reaches the slot's retire serial.

- [ ] **Step 1: Write failing pure CPU tests.** Cover add, no-op upsert, transform patch, part replacement, hide/remove, destruction, stable handle, stale-generation rejection, capacity failure, frame-safe delayed reuse, and drain.

- [ ] **Step 2: Run red test.**

  Run: `make -C MatterEngine3/tests run-dynamic-slots`

  Expected: missing slot implementation.

- [ ] **Step 3: Implement dense slots plus generation-checked free list.** Track last part/transform/shadow values and emit `Bind`, `Transform`, or `Remove` changes only when values differ. Capacity failure returns a recoverable result without surrendering ECS identity.

- [ ] **Step 4: Run focused tests.**

  Run: `make -C MatterEngine3/tests run-dynamic-slots`

  Expected: PASS with no Vulkan runtime.

- [ ] **Step 5: Commit.**

  ```bash
  git add MatterEngine3/src/render/dynamic_instance_slots.* MatterEngine3/tests/dynamic_instance_slots_tests.cpp MatterEngine3/tests/Makefile
  git commit -m "feat(render): add stable dynamic instance slots"
  ```

### Task 7: Vulkan dynamic lane

**Files:**
- Modify: `MatterEngine3/src/render/vk_scene_renderer.h`
- Modify: `MatterEngine3/src/render/vk_scene_renderer.cpp`
- Modify: `MatterEngine3/src/render/vk_instance_cache.h`
- Modify: `MatterEngine3/src/render/vk_instance_cache.cpp`
- Test: `MatterEngine3/tests/vk_scene_renderer_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Consumes: `DynamicInstanceSlots` changes from Task 6.
- Produces: `VkSceneRenderer::update_dynamic_instances(span<const DynamicSlotChange>, uint64_t submit_serial, std::string&)` and `finish_dynamic_frame(uint64_t completed_serial)`.
- Existing `update_instances(const std::vector<VkSceneInstance>&, ...)` remains the static bulk lane.

- [ ] **Step 1: Add failing renderer assembly tests.** Verify static and dynamic records share part resources/cull command construction, a transform-only update does not rebuild static command layout or upload static instances, removal cannot reuse a GPU-visible slot before completion, and identity words preserve the dynamic tag plus `SceneEntityId`.

- [ ] **Step 2: Run red CPU renderer test.**

  Run: `make -C MatterEngine3/tests run-vk-scene-renderer`

  Expected: missing dynamic interface assertions.

- [ ] **Step 3: Extend GPU-visible instance storage.** Reserve/tag identity so static hits retain their current instance identity and dynamic hits carry `SceneEntityId`. Grow buffers without invalidating live frame resources; update only dirty dynamic ranges.

- [ ] **Step 4: Share rendering resources.** Dynamic records bind the same `VkScenePart`, BLAS, material, culling, lighting, and submission resources as static records. Do not concatenate dynamic inputs into the static fingerprint/cache vector.

- [ ] **Step 5: Run renderer and lightweight Vulkan tests.**

  Run: `make -C MatterEngine3/tests run-vk-scene-renderer`

  Run when available: `make -C MatterViewer vulkan-smoke`

  Expected: CPU suite PASS; smoke compiles/runs without validation errors on a capable host.

- [ ] **Step 6: Commit.**

  ```bash
  git add MatterEngine3/src/render MatterEngine3/tests/vk_scene_renderer_tests.cpp MatterEngine3/tests/Makefile
  git commit -m "feat(vulkan): render stable ECS dynamic instances"
  ```

### Task 8: ECS dynamic render bridge and picking identity

**Files:**
- Create: `MatterEngine3/src/ecs/dynamic_scene_bridge.h`
- Create: `MatterEngine3/src/ecs/dynamic_scene_bridge.cpp`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/include/matter/query.h`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Test: `MatterEngine3/tests/dynamic_scene_bridge_tests.cpp`
- Test: `MatterEngine3/tests/api_tests.cpp`

**Interfaces:**
- Produces: frame-stage reconciliation query requiring `SceneEntityId`, `WorldTransform`, visible `PartInstance`; tagged `ScenePick { kind, scene_entity_id, static_instance }`; `WorldSession::scene_entity_at`, `scene_entities`, and dynamic pick resolution.

- [ ] **Step 1: Write failing bridge tests with a fake sink.** Cover add, transform-only, part change, hide, remove, destroy, missing part error/recovery, capacity error/retry, generation replacement, no-op frame, and unchanged procedural world version/re-bin count.

- [ ] **Step 2: Run red test.**

  Run: `make -C MatterEngine3/tests run-dynamic-bridge`

  Expected: missing bridge/sink API.

- [ ] **Step 3: Implement frame-ordered reconciliation.** Register the bridge after physics and world-transform propagation in `FrameUpdate`. Keep maps keyed by `SceneEntityId`; resolve current Flecs entity each use. Add/remove `PartInstanceError` through deferred-safe ECS operations and catch all callback-boundary exceptions.

- [ ] **Step 4: Wire WorldSession render lifecycle.** Feed bridge changes to the Vulkan dynamic lane, call completion after presentation, drain before reload/shutdown, and expose renderer-neutral entity/pick queries. Rendering performs no structural ECS mutation.

- [ ] **Step 5: Run bridge, ECS, physics, and API tests.**

  Run: `make -C MatterEngine3/tests run-dynamic-bridge run-ecs run-physics run-api-tests`

  Expected: all available tests PASS.

- [ ] **Step 6: Commit.**

  ```bash
  git add MatterEngine3/src/ecs MatterEngine3/include/matter MatterEngine3/src/matter_engine.cpp MatterEngine3/tests
  git commit -m "feat(scene): bridge ECS entities to dynamic rendering"
  ```

### Task 9: Editor model, selection, and hierarchy commands

**Files:**
- Create: `MatterViewer/editor_model.h`
- Create: `MatterViewer/editor_model.cpp`
- Modify: `MatterEngine3/include/matter/scene.h`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Test: `MatterEngine3/tests/editor_model_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`

**Interfaces:**
- Produces: `viewer::Selection { SceneEntityId id; uint64_t world_generation; }`, `EditorModel::{refresh,filter,select,create_empty,duplicate_selected,delete_selected,reparent_selected}`.
- Engine mutation APIs accept stable IDs and return structured `SceneEditResult`; internal entities without `SceneEntityId` are hidden.

- [ ] **Step 1: Write failing GL-free editor tests.** Cover hierarchy preorder, deterministic sibling order, name/ID filtering, viewport/outliner selection parity, stale generation/entity invalidation, create, deep-value duplicate with new ID, delete, reparent, cycle rejection, and child cleanup semantics.

- [ ] **Step 2: Run red test.**

  Run: `make -C MatterEngine3/tests run-editor-model`

  Expected: missing model and engine scene-edit API.

- [ ] **Step 3: Add engine-safe scene records and commands.** Return copied names and component summaries; resolve `SceneEntityId` immediately before mutation; queue hierarchy/component changes through supported ECS APIs; never retain raw Flecs entities in Viewer state.

- [ ] **Step 4: Implement the Viewer model without ImGui.** `refresh` consumes copied engine records, builds parent/child indices, preserves selection only when generation and ID resolve, and presents stable filtered preorder rows.

- [ ] **Step 5: Run focused tests.**

  Run: `make -C MatterEngine3/tests run-editor-model run-viewer-logic`

  Expected: all PASS.

- [ ] **Step 6: Commit.**

  ```bash
  git add MatterViewer/editor_model.* MatterEngine3/include/matter MatterEngine3/src/matter_engine.cpp MatterEngine3/tests
  git commit -m "feat(viewer): add generic ECS editor model"
  ```

### Task 10: Generic Entities and Properties UI

**Files:**
- Create: `MatterViewer/properties_registry.h`
- Create: `MatterViewer/properties_registry.cpp`
- Modify: `MatterViewer/ui.h`
- Modify: `MatterViewer/ui.cpp`
- Modify: `MatterViewer/main.cpp`
- Test: `MatterEngine3/tests/properties_registry_tests.cpp`
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Consumes: `ComponentDescriptor`, copied scene records, and stable-ID mutation APIs.
- Produces: generic Entities window and Properties window; `PropertiesRegistry` editors for bool, integer, float, enum, `Float3`, and quaternion values plus safe add/remove lists.

- [ ] **Step 1: Write failing registry/UI-logic tests.** Verify field widget kind/range mapping, unsupported/internal components excluded from Add Component, add/remove calls engine mutation boundaries, field validation errors remain visible, and no raw component pointer survives a frame.

- [ ] **Step 2: Run red tests.**

  Run: `make -C MatterEngine3/tests run-properties-registry run-viewer-logic`

  Expected: missing registry/UI behavior.

- [ ] **Step 3: Implement generic registry and windows.** Entities shows searchable hierarchy with Create/Duplicate/Delete/Reparent actions. Properties enumerates registered components and commits copied edits by stable ID. Keep application-wide panels only; add no Physics Playground window.

- [ ] **Step 4: Generalize gizmo selection.** Replace streaming-anchor-only transform state with selected `LocalTransform`; support Translate/Rotate/Scale using current-frame ImGuizmo arbitration and full-screen draw-list rules. In Edit/Pause use scene mutation; in Play route dynamic rigid bodies through physics teleport. Reject unsupported hierarchy/physics scale with a visible diagnostic.

- [ ] **Step 5: Run logic and translation-unit gates.**

  Run: `make -C MatterEngine3/tests run-properties-registry run-editor-model run-viewer-logic`

  Run: `powershell -ExecutionPolicy Bypass -File .superpowers/sdd/flecs-task-7-static-check.ps1`

  Expected: all PASS.

- [ ] **Step 6: Commit.**

  ```bash
  git add MatterViewer MatterEngine3/tests
  git commit -m "feat(viewer): add Entities and generic Properties"
  ```

### Task 11: Play, Pause, Step, and reset-on-Stop

**Files:**
- Modify: `MatterEngine3/include/matter/scene.h`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/src/ecs/scene_registry.h`
- Modify: `MatterEngine3/src/ecs/scene_registry.cpp`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterViewer/ui.h`
- Modify: `MatterViewer/ui.cpp`
- Modify: `MatterViewer/main.cpp`
- Test: `MatterEngine3/tests/simulation_control_tests.cpp`

**Interfaces:**
- Produces: `SimulationMode { Edit, Play, Pause }`; `WorldSession::{play,pause,step,stop,simulation_mode}`.
- Snapshot records explicitly whitelisted entity name, ID, parent ID, and registered editable components; private/status/error components are excluded.

- [ ] **Step 1: Write failing simulation tests.** Cover Play capture after queued edit reconciliation, Edit/Pause no accumulation, exactly one fixed Step, Stop transform/component/hierarchy restoration, recreation of deleted entities, removal of Play-created entities, stable selection ID, rebuilt—not deserialized—physics/render private state, and Stop before world reload/switch/regenerate.

- [ ] **Step 2: Run red test.**

  Run: `make -C MatterEngine3/tests run-simulation-control`

  Expected: missing simulation controls.

- [ ] **Step 3: Separate frame progression from fixed progression.** Extend runtime tick input with `advance_fixed` and `single_fixed_step`. Frame systems remain responsive in Edit/Pause; Step executes exactly one `fixed_delta_seconds` pipeline without consuming stale accumulator time.

- [ ] **Step 4: Implement whitelist snapshot/restore.** Capture stable records only after pending edits drain. Stop transactionally removes current authorable entities, recreates snapshot entities/components/names/hierarchy with the same `SceneEntityId`, then lets physics and render bridges reconcile private state. Clear selection only if its ID was not restored.

- [ ] **Step 5: Add shared toolbar controls.** Present Play/Pause/Step/Stop in the main editor toolbar with mode-correct enablement; no physics-specific control window.

- [ ] **Step 6: Run focused regressions.**

  Run: `make -C MatterEngine3/tests run-simulation-control run-physics run-dynamic-bridge run-editor-model run-viewer-logic`

  Expected: all PASS.

- [ ] **Step 7: Commit.**

  ```bash
  git add MatterEngine3/include/matter MatterEngine3/src/ecs MatterEngine3/src/matter_engine.cpp MatterViewer MatterEngine3/tests
  git commit -m "feat(editor): add reset-on-stop simulation controls"
  ```

### Task 12: Specialized component editors and sector-panel retirement

**Files:**
- Modify: `MatterViewer/properties_registry.h`
- Modify: `MatterViewer/properties_registry.cpp`
- Modify: `MatterViewer/ui.h`
- Modify: `MatterViewer/ui.cpp`
- Modify: `MatterViewer/streaming_anchor_controller.h`
- Modify: `MatterViewer/streaming_anchor_controller.cpp`
- Modify: `MatterEngine3/include/matter/scene.h`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/matter_engine.cpp`
- Test: `MatterEngine3/tests/properties_registry_tests.cpp`
- Test: `MatterEngine3/tests/viewer_logic_tests.cpp`

**Interfaces:**
- Produces: custom Properties adapters for `PartInstance`, physics components, and `SectorStreaming`; engine commands for part assignment, teleport, velocity, impulse, wake, and streaming attach/remove/follow/status.
- Exactly one `SectorStreaming` component may exist; its entity can optionally follow the player camera but remains independently gizmo-movable when detached.

- [ ] **Step 1: Add failing action/parity tests.** Verify loaded-part selection hashes safely, missing part diagnostics, physics validation and command routing, streaming component attach/remove uniqueness, camera-follow toggle, detached transform editing, counts/status/errors, and current Phase 3 input arbitration.

- [ ] **Step 2: Run red tests.**

  Run: `make -C MatterEngine3/tests run-properties-registry run-viewer-logic`

  Expected: specialized actions/parity missing.

- [ ] **Step 3: Register specialized editors.** Keep generic reflected fields visible where safe, then add actions: Part picker/visibility; rigid body type/damping/gravity plus velocity/impulse/wake/teleport; collider validation; streaming attach/remove/follow/rings/status. All actions resolve stable identity at call time.

- [ ] **Step 4: Retire standalone streaming UI.** Delete `draw_sector_streaming_panel` and its menu/window state only after all controls are reachable on the selected streaming entity in Properties. Preserve `StreamingAnchorController` as non-UI behavior or fold it into `EditorModel`; do not reintroduce camera ownership.

- [ ] **Step 5: Run parity/static gates.**

  Run: `make -C MatterEngine3/tests run-properties-registry run-viewer-logic run-sectorcoord`

  Run: `powershell -ExecutionPolicy Bypass -File .superpowers/sdd/sector-streaming-phase3-static-check.ps1`

  Expected: tests PASS; static check is updated to require Properties integration and absence of the standalone window.

- [ ] **Step 6: Commit.**

  ```bash
  git add MatterViewer MatterEngine3/include/matter MatterEngine3/src/matter_engine.cpp MatterEngine3/tests .superpowers/sdd/sector-streaming-phase3-static-check.ps1
  git commit -m "feat(viewer): edit runtime components through Properties"
  ```

### Task 13: Ordinary PhysicsPlayground world and closure

**Files:**
- Create: `MatterEngine3/examples/world_demo/objects/PlaygroundFloor.js`
- Create: `MatterEngine3/examples/world_demo/objects/Crate.js`
- Create: `MatterEngine3/examples/world_demo/worlds/PhysicsPlayground.js`
- Modify: `MatterEngine3/tests/world_definition_tests.cpp`
- Modify: `MatterEngine3/tests/entity_recipe_tests.cpp`
- Create: `.superpowers/sdd/phase4-static-check.ps1`
- Modify: `docs/superpowers/specs/2026-07-19-phase4-runtime-scene-editor-bridge-design.md`
- Modify: `.superpowers/sdd/phase4-progress.md`

**Interfaces:**
- Produces: one normal discoverable world with a static floor and a mix of declarative plus `buildEntities()` dynamic boxes; no special application/window code.

- [ ] **Step 1: Add failing fixture assertions.** Load `PhysicsPlayground`; assert floor root, one declarative body, deterministic DSL body sequence, valid PartInstance/RigidBody/BoxCollider values, and recipe part dependencies.

- [ ] **Step 2: Run red tests.**

  Run: `make -C MatterEngine3/tests run-world-definition run-entity-recipes`

  Expected: missing playground files.

- [ ] **Step 3: Implement the ordinary world.** `PhysicsPlayground extends World`; `static roots` places `PlaygroundFloor`; `static entities` declares at least one named box; `buildEntities()` loops deterministically to add several uniquely identified boxes with PartInstance, Dynamic RigidBody, and BoxCollider. Use no viewer-specific branch.

- [ ] **Step 4: Add the Phase 4 static closure check.** Require exactly one registration/source occurrence for new systems, unique flattened Windows source basenames, public-header opacity, no active manifests/WorldData/ExplorerDemo, no one-off Physics Playground or Sector Streaming window, and all three Phase 1–3 closure scripts still present.

- [ ] **Step 5: Run targeted full regression and clean product build.**

  Run: `make -C MatterEngine3/tests run-world-definition run-entity-recipes run-scene-registry run-dynamic-slots run-dynamic-bridge run-editor-model run-properties-registry run-simulation-control run-ecs run-physics run-sectorcoord run-vk-scene-renderer`

  Run: `powershell -ExecutionPolicy Bypass -File .superpowers/sdd/phase4-static-check.ps1`

  Run in MSYS2 UCRT64: `make -C MatterViewer clean && make -C MatterViewer windows -j1`

  Expected: every focused suite and static check PASS; `MatterViewer.exe` links successfully. A pre-existing unrelated warning may be recorded but no new warning is accepted silently.

- [ ] **Step 6: Update durable records.** Mark automated gates complete in the design and progress ledger. Leave the ten-item manual acceptance checklist explicitly pending for the user; do not manufacture screenshot evidence.

- [ ] **Step 7: Commit.**

  ```bash
  git add MatterEngine3/examples MatterEngine3/tests .superpowers/sdd docs/superpowers/specs
  git commit -m "feat(world): add playable physics playground"
  ```

## Final review and handoff

- [ ] Generate a whole-branch review package from `git merge-base main HEAD` through `HEAD` and dispatch a fresh high-capability reviewer using `superpowers:requesting-code-review`.
- [ ] Resolve every Critical and Important finding through a fresh fix/re-review loop; record Minor findings in `.superpowers/sdd/phase4-progress.md` with disposition.
- [ ] Re-run the Task 13 focused regression, Phase 4 static check, and clean Windows product build after the final fix commit.
- [ ] Hand the user the exact MSYS2 build/run command and the ten manual acceptance observations from the design. Do not merge Phase 4 until the user gives manual acceptance.
