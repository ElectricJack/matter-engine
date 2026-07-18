### Task 6: Integrate One ECS Runtime into Every WorldSession

**Files:**

- Modify: `MatterEngine3/src/matter_engine.cpp`
- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/tests/world_stream_tests.cpp`

- [ ] **Step 1: Add failing session integration assertions**

Extend `world_stream_tests.cpp` to verify:

- `session->ecs()` resolves the `matter.ecs` module and the `WorldRuntimeState` singleton;
- a named runtime entity remains alive with the same ID after `reload()` completes;
- it remains alive after `regenerate(seed)` completes;
- successful publish sets `Ready` and increments `content_generation` exactly once;
- a forced fatal bake error sets `Failed` without deleting the runtime entity;
- a newly opened replacement session cannot resolve the old entity ID as alive.

Keep these checks in the existing session fixture so they reuse its GL/Vulkan setup rather than creating a second integration binary.

- [ ] **Step 2: Run the integration target and observe missing API failures**

```powershell
wsl bash -lc 'cd "/mnt/d/Shared With Desktop/AI/matter-engine-cpp" && make -C MatterEngine3/tests run-worldstream'
```

Expected: compilation fails because `WorldSession::ecs()` is not defined.

- [ ] **Step 3: Add session ownership and accessors**

Add `ecs_runtime::Runtime ecs_runtime;` to `WorldSession::Impl` as a value member so it is constructed once with the session and destroyed after the worker is joined. Declare/define:

```cpp
flecs::world& WorldSession::ecs() { return impl_->ecs_runtime.world(); }
const flecs::world& WorldSession::ecs() const { return impl_->ecs_runtime.world(); }
```

Include `matter/ecs.h` from `world_session.h` so the transitive Flecs dependency is intentional and visible.

- [ ] **Step 4: Bridge worker bake state through plain-data commands**

Use:

```cpp
enum class WorldStateCommandKind { Loading, Ready, Failed };
struct WorldStateCommand { WorldStateCommandKind kind; };
```

`enqueue_world_state` is mutex-protected and callable by bake workers. `Runtime::tick()` swaps the queue into a local vector and applies it on the tick thread. `Ready` increments `content_generation`; `Loading` and `Failed` do not. Cancelled/superseded bakes do not set `Failed`. Non-fatal per-part errors do not set `Failed` if a world generation publishes successfully.

- [ ] **Step 5: Replace the old tick body without changing provider ordering**

Move the existing provider/live-edit polling portion into a private `WorldSession::Impl::poll_runtime_sources()` helper. Implement public tick in this order:

```cpp
void WorldSession::tick(const TickDesc& desc) {
    const ecs_runtime::TickResult result = impl_->ecs_runtime.tick(desc);
    if (result.invalid) {
        ++impl_->stats.ecs_invalid_ticks;
        return;
    }
    impl_->stats.ecs_fixed_steps += result.fixed_steps;
    impl_->stats.ecs_dropped_steps += result.dropped_steps;
    impl_->poll_runtime_sources();
}
```

This preserves the approved rule that invalid ticks progress nothing. Keep `poll_runtime_sources()`'s existing bake-active early return and live-edit debounce behavior intact.

- [ ] **Step 6: Verify session lifecycle and state transitions**

Run `run-worldstream`.

Expected: existing sector assertions and all new ECS lifecycle assertions pass.

- [ ] **Step 7: Commit session ownership**

```powershell
git add MatterEngine3/include/matter/world_session.h MatterEngine3/src/matter_engine.cpp MatterEngine3/tests/world_stream_tests.cpp
git commit -m "feat(ecs): own ECS runtime in WorldSession"
```

---

