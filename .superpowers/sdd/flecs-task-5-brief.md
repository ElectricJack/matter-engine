### Task 5: Implement Fixed-Step and Frame Pipelines

**Files:**

- Modify: `MatterEngine3/include/matter/world_session.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.h`
- Modify: `MatterEngine3/src/ecs/ecs_runtime.cpp`
- Modify: `MatterEngine3/tests/ecs_tests.cpp`

- [ ] **Step 1: Add the public tick descriptor and stats fields**

Add before `FrameStats`:

```cpp
struct TickDesc {
    float frame_delta_seconds = 0.0f;
    float fixed_delta_seconds = 1.0f / 60.0f;
    uint32_t max_fixed_steps = 4;
};
```

Append to `FrameStats`:

```cpp
uint64_t ecs_fixed_steps = 0;
uint64_t ecs_dropped_steps = 0;
uint64_t ecs_invalid_ticks = 0;
```

Declare only `void tick(const TickDesc& tick);`.

- [ ] **Step 2: Add failing runtime scheduling tests**

Define private runtime result types in `ecs_runtime.h`:

```cpp
struct TickResult {
    uint32_t fixed_steps = 0;
    uint32_t dropped_steps = 0;
    bool invalid = false;
};
```

Test with recording systems that the exact order for one fixed step is:

```text
FixedPreUpdate, FixedUpdate, PrePhysics, Physics,
PostPhysics, FixedPostUpdate, FrameUpdate
```

Also test:

- half a fixed step runs only `FrameUpdate`;
- two accumulated steps run the six fixed phases twice, then frame once;
- `frame_delta_seconds=1.0`, `fixed_delta_seconds=0.1`, `max_fixed_steps=2` clamps the contribution to `0.25`, runs two, reports zero dropped steps, and preserves a `0.05` fractional remainder;
- `frame_delta_seconds=0.25`, `fixed_delta_seconds=0.01`, `max_fixed_steps=2` runs two, reports 23 dropped complete steps, and preserves no whole step;
- a later `0.05 + 0.05` pair proves fractional remainder preservation;
- negative/NaN frame delta, zero/NaN fixed delta, and zero max steps report invalid and run no systems;
- contributed frame time clamps to `0.25f`.

- [ ] **Step 3: Run tests and observe scheduling failures**

Expected: compile/link failures until `Runtime` exists.

- [ ] **Step 4: Implement the private runtime contract**

`ecs_runtime.h` declares a non-copyable `Runtime` owning:

```cpp
flecs::world world_;
flecs::entity fixed_pipeline_;
flecs::entity frame_pipeline_;
double accumulator_seconds_ = 0.0;
```

Public private-layer methods:

```cpp
flecs::world& world() noexcept;
const flecs::world& world() const noexcept;
TickResult tick(const TickDesc& desc);
void enqueue_world_state(WorldStateCommand command);
```

Build two Flecs pipelines. Fixed systems carry `FixedPipelineSystem`; frame systems carry `FramePipelineSystem`. Each system also uses its ordered custom phase via `kind(...)`. Run the fixed pipeline once per accumulator step with exactly `fixed_delta_seconds`, then the frame pipeline once with the clamped frame delta.

Add public `matter::ecs::enqueue_reparent(child, parent)` and `enqueue_clear_parent(child)` helpers backed by a world-owned last-write-wins command map. They are the supported way to request a new final parent/root while an immediate hierarchy mutation is pending or from inside a `ChildOf` observer. `Runtime::tick()` drains these commands at the beginning of the next valid tick, before ECS pipelines run and after the previous Flecs merge has completed. Dead/cross-world entities are discarded; a command that is still temporarily pending remains queued for the next tick. Tests prove multiple queued requests for one child collapse to the last desired state and never perform two same-child Flecs mutations in one observer/merge sequence.

- [ ] **Step 5: Implement validation, clamping, and drop policy**

Use `double` for the accumulator and validate with `std::isfinite` before adding time. On invalid input return `{0,0,true}` without draining commands or progressing either pipeline. Clamp only the contributed frame delta to `0.25`. After the catch-up limit, calculate complete excess steps with `floor(accumulator / fixed_delta)`, subtract them, and report their count; keep the remaining fractional duration.

- [ ] **Step 6: Run the ECS test suite twice**

Run `run-ecs` twice to catch accidental static/global world state.

Expected: identical passing output both times.

- [ ] **Step 7: Commit scheduling**

```powershell
git add MatterEngine3/include/matter/world_session.h MatterEngine3/src/ecs MatterEngine3/tests/ecs_tests.cpp
git commit -m "feat(ecs): add deterministic session tick pipelines"
```

---

