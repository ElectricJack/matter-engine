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

- [ ] **Step 1: Write failing path and cache-key tests.** Assert world source is `<project>/worlds/<name>.js`, object sources are `<project>/objects`, outputs are under `<project>/.cache/<name>`, project and engine shared libraries affect the cache fingerprint, and stale `WorldData/world.manifest` is ignored.

- [ ] **Step 2: Run red gates.**

  Run: `make -C MatterEngine3/tests run-world-definition run-resolvecache`

  Expected: failures showing legacy `schemas_dir/world_data_dir` routing.

- [ ] **Step 3: Replace production path plumbing.** Update Viewer discovery to enumerate `.js` files directly in `worlds/`. Derive all project paths once in `open_world`; keep `engine_shared_lib_dir` as the engine support tier and treat missing project `shared-lib/` as empty. Add the explicitly temporary `project_dir == nullptr` test fallback described above without routing MatterViewer through it.

- [ ] **Step 4: Replace provider manifest input with `WorldDefinition`.** Convert `WorldRoot` values to existing graph `ChildRequest` values and lights/settings to current runtime structures. Preserve root order, transforms, params, `expand`, and `tileset` semantics.

- [ ] **Step 5: Move cache outputs and fingerprint inputs.** Cache identity includes world JS, recursively used object sources, project shared sources, engine shared sources, and canonical parameters. It never hashes stale manifests.

- [ ] **Step 6: Run focused tests and Viewer logic compile.**

  Run: `make -C MatterEngine3/tests run-world-definition run-resolvecache run-viewer-logic`

  Expected: all PASS.

- [ ] **Step 7: Commit.**

  ```bash
  git add MatterEngine3/include/matter/world_session.h MatterEngine3/src/provider MatterEngine3/src/matter_engine.cpp MatterEngine3/src/resolve_cache.* MatterViewer MatterEngine3/tests
  git commit -m "refactor(world): use project-root JavaScript layout"
  ```

