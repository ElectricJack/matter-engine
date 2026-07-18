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

