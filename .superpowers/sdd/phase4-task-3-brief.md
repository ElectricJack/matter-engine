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

