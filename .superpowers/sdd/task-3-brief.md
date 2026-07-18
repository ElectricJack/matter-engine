### Task 3: Body Validation, Shapes, and Stable Bridge Lifecycle

**Files:**
- Create: `MatterEngine3/src/ecs/physics_shapes.h`
- Create: `MatterEngine3/src/ecs/physics_shapes.cpp`
- Create: `MatterEngine3/src/ecs/physics_systems.cpp`
- Modify: `MatterEngine3/src/ecs/physics_context.h/.cpp`
- Modify: `MatterEngine3/tests/physics_tests.cpp`
- Modify: `MatterEngine3/tests/Makefile`
- Modify: `MatterEngine3/Makefile`
- Modify: `MatterViewer/Makefile`
- Modify: `ExplorerDemo/Makefile`
- Modify: `.superpowers/sdd/box3d-phase2-static-check.ps1`

**Interfaces:**
- Consumes: public body/collider components and private context.
- Produces: deterministic reconcile system, valid Box3D bodies/shapes, `PhysicsError`, and stable bridges.

- [ ] **Step 1: Write the failing validation/lifecycle matrix**

Add table-driven tests for missing transform, parent, nonunit/nonfinite scale,
missing/multiple collider, invalid body numbers, invalid primitive dimensions,
invalid material, too-small/coplanar/over-32 hulls, and Box3D hull failure. Assert the
exact `PhysicsErrorCode`, zero live bodies, then correct each entity and assert the
error is removed and one body exists.

Add successful static/dynamic creation tests for sphere, capsule, oriented box, and
32-point hull. Delete/remove/invalidate each and assert body/shape validity becomes
false at the next fixed reconciliation boundary.

Add an archetype-move test: create a body, add/remove unrelated components, step,
and prove Box3D user data still resolves the full original entity ID.

- [ ] **Step 2: Run RED**

Run: `make -C MatterEngine3/tests run-physics`

Expected: validation, body-count, and bridge checks fail because reconciliation does
not exist.

- [ ] **Step 3: Implement pure desired-shape validation**

In `physics_shapes.h`, define `DesiredBody` with a tagged shape union/value data and:

```cpp
ValidationResult validate_desired_body(flecs::entity entity);
b3ShapeId create_shape(b3BodyId body, const DesiredBody& desired,
                       b3HullData*& temporary_hull);
```

Validation counts the four collider components, checks the root/unit-scale contract,
normalizes quaternions, validates finite values/material/filter data, and calls
`b3CreateHull(points, count, 32)` only after pure checks pass. Use RAII to call
`b3DestroyHull` on every success/failure path.

- [ ] **Step 4: Implement stable bridges and deterministic reconciliation**

Store bridges as `std::unordered_map<flecs::entity_t,
std::unique_ptr<BridgeRecord>>`; the pointed-to record remains stable across map
rehashes. It contains full entity ID, `b3BodyId`, `b3ShapeId`, and live state.

At `PhysicsReconcile`, collect candidates and existing bridge IDs, sort/unique full
IDs, then reconcile in ascending order. On valid creation, set body/shape user data
to the bridge pointer. On removal/failure, clear user data, destroy the body, retire
the bridge, update stats, and attach the exact error. Never retain a Flecs component
pointer across a structural mutation.

Any body/collider/transform/`ChildOf` change marks the entity for reconcile. A cheap
hash of the full desired configuration prevents rebuilding unchanged bodies. When
replacing a valid dynamic body, snapshot pose, velocities, and awake state; publish
the replacement before destroying the old body; then restore state.

Add `physics_shapes.cpp` and `physics_systems.cpp` to every runtime-bearing source
union at the same time they are created. Strengthen the static checker to require all
three physics implementation files, preventing an intermediate broken archive or
product link graph.

- [ ] **Step 5: Run GREEN plus sanitizer/static lifetime checks**

Run twice: `make -C MatterEngine3/tests run-physics`

Run: `git diff --check`

Expected: all validation/lifecycle tests `ALL PASS`; no pointer stored in user data
originates from Flecs component/query memory.

- [ ] **Step 6: Commit**

```bash
git add MatterEngine3/src/ecs/physics_shapes.* MatterEngine3/src/ecs/physics_systems.cpp \
  MatterEngine3/src/ecs/physics_context.* MatterEngine3/tests/physics_tests.cpp \
  MatterEngine3/tests/Makefile MatterEngine3/Makefile MatterViewer/Makefile \
  ExplorerDemo/Makefile .superpowers/sdd/box3d-phase2-static-check.ps1
git commit -m "feat(physics): reconcile ECS bodies and convex colliders"
```

---
