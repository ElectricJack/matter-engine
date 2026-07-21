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

- [ ] **Step 1: Add a failing focused evaluator test.** Cover base-class rejection, roots/lights/settings extraction, absence of accidental `field()` execution, project-before-engine shared-library lookup, and no-entity worlds. Use temporary fixtures whose `field()` throws if invoked.

- [ ] **Step 2: Run the red test.**

  Run: `make -C MatterEngine3/tests run-world-definition`

  Expected: compile failure because `matter/world_definition.h` and `load_world_definition` do not exist.

- [ ] **Step 3: Define the owned contract and loader.** The public shape must include:

  ```cpp
  struct WorldRoot { std::string module; std::string params_json = "{}"; Mat4f transform{}; bool expand = false; bool tileset = false; };
  struct WorldLight { Float3 position{}; Float3 color{1,1,1}; float intensity = 1.0f; float range = 10.0f; };
  struct RawEntityRecipe { std::string authored_id, display_name, parent_authored_id, components_json; };
  struct WorldDefinition { std::vector<WorldRoot> roots; std::vector<WorldLight> lights; std::vector<RawEntityRecipe> entities; WorldSettings settings{}; };
  bool load_world_definition(const WorldLoadDesc&, WorldDefinition&, WorldLoadError&);
  ```

  Evaluate only the selected class statics plus optional `buildEntities()` in a fresh QuickJS context. Bind `this.entity(record)` to append into the same ordered array after `static entities`. Copy all strings before destroying the context.

- [ ] **Step 4: Make lookup and execution hermetic.** Permit only explicit module roots and seed/parameter bindings; do not expose clock, network, ECS pointers, frame callbacks, or post-load spawning handles. Return source location and property path in `WorldLoadError`.

- [ ] **Step 5: Run focused and existing evaluator tests.**

  Run: `make -C MatterEngine3/tests run-world-definition run-evalworld run-script`

  Expected: all PASS.

- [ ] **Step 6: Commit.**

  ```bash
  git add MatterEngine3/include/matter/world_definition.h MatterEngine3/src/script MatterEngine3/src/script_host.h MatterEngine3/src/script_host.cpp MatterEngine3/tests/world_definition_tests.cpp MatterEngine3/tests/Makefile
  git commit -m "feat(world): evaluate world JavaScript definitions"
  ```

