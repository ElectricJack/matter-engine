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
