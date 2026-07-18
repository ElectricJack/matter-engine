### Task 4: Implement Transform Hierarchy and Validated Reparenting

**Files:**

- Create: `MatterEngine3/src/ecs/transform_math.h`
- Modify: `MatterEngine3/src/ecs/transform_system.cpp`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/tests/ecs_tests.cpp`

- [ ] **Step 1: Add failing transform and hierarchy tests**

Add tests covering:

- root TRS produces the expected row-major/column-vector `Mat4f`;
- parent translation `(10,0,0)` plus child translation `(0,2,0)` produces world translation `(10,2,0)`;
- a three-level hierarchy propagates changes from the root;
- `reparent(child, new_parent)` changes the result and dirties the subtree;
- `clear_parent(child)` preserves the local transform and recomputes it as a root;
- `reparent(root, grandchild)` returns `false` and leaves the hierarchy unchanged;
- destroying a parent uses Flecs' built-in `ChildOf` ownership semantics and cascade-deletes its descendants; callers must explicitly detach or reparent children they intend to preserve.

Use approximate float checks with an epsilon of `1e-5f`.

- [ ] **Step 2: Run and observe failures**

Expected: link failures for `reparent`/`clear_parent` or missing `WorldTransform` values.

- [ ] **Step 3: Implement deterministic TRS math**

Create `transform_math.h` with a pure `trs_matrix(const LocalTransform&)` function. Normalize a finite non-zero quaternion; use identity rotation for invalid/zero-length input. Emit a row-major matrix for column-vector algebra, with translation at indices `3`, `7`, and `11`, matching `matter::Mat4f` and `viewer::mat4_mul`.

- [ ] **Step 4: Implement safe hierarchy mutation**

In `transform_system.cpp`:

- Reject null, dead, cross-world, self-parent, and descendant-parent requests.
- Walk `flecs::ChildOf` ancestors before mutation; never rely on a Flecs abort for cycle detection.
- Apply `child.child_of(parent)` only after validation.
- `clear_parent` removes the existing `(ChildOf, *)` pair.
- Mark the changed entity and every current descendant with `TransformDirty`.
- Document `reparent`/`clear_parent` as the supported hierarchy mutation API; direct `ChildOf` edits bypass MatterEngine validation.

- [ ] **Step 5: Register dirty observers and propagation systems**

Register observers that mark the affected subtree dirty on `OnSet<LocalTransform>` and explicit parent removal. Parent destruction follows Flecs' built-in `ChildOf` cascade-delete policy. Register transform propagation in both `FixedPostUpdate` and `FrameUpdate`; dirty tags prevent duplicate work when both phases run in one frame.

Propagation rules:

```text
root WorldTransform  = TRS(LocalTransform)
child WorldTransform = parent WorldTransform × TRS(LocalTransform)
```

Traverse parents before children with a cached hierarchy query. If a parent lacks `WorldTransform`, compute/add it before the child. Remove `TransformDirty` only after a successful write. Perform structural writes inside Flecs deferral.

- [ ] **Step 6: Run the ECS test suite**

Expected: all hierarchy and transform checks pass; no Flecs assertion/abort occurs in the cycle case.

- [ ] **Step 7: Commit transform support**

```powershell
git add MatterEngine3/src/ecs MatterEngine3/tests/ecs_tests.cpp MatterEngine3/include/matter/ecs.h
git commit -m "feat(ecs): add hierarchical transform propagation"
```

---

