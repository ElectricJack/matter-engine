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

