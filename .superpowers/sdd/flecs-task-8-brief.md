### Task 8: Migrate All WorldSession Tick Callers

**Files:**

- Modify: `MatterViewer/main.cpp`
- Modify: `MatterViewer/main_linux.cpp`
- Modify: `ExplorerDemo/main.cpp`
- Modify: every `MatterEngine3/tests/*.cpp` call site where the receiver is a `WorldSession`

- [ ] **Step 1: Enumerate only WorldSession call sites**

Run:

```powershell
rg -n "session->tick\(\)|WorldSession.*tick\(\)|\.tick\(\)" MatterViewer ExplorerDemo MatterEngine3\tests
```

Do not change `live_edit::Session::tick()` calls in `dev_live_edit_tests.cpp`; that is a different type and API.

- [ ] **Step 2: Migrate viewer loops to measured time**

At each viewer call site, pass the frame delta already measured by the application loop:

```cpp
matter::TickDesc tick{};
tick.frame_delta_seconds = frame_delta_seconds;
session->tick(tick);
```

If the local value is milliseconds, convert once to seconds. Do not call a second clock and do not couple the tick to the render camera.

- [ ] **Step 3: Migrate deterministic tests and ExplorerDemo**

For tests that only pump async work, use `TickDesc{0.0f}` so ECS frame systems run without accumulating fixed simulation. For endless-flight/session loops and ExplorerDemo, pass their existing real or simulated frame delta. Avoid inserting sleeps solely to advance ECS time.

- [ ] **Step 4: Prove the old WorldSession API is gone**

Run:

```powershell
rg -n "session->tick\(\)" MatterViewer ExplorerDemo MatterEngine3\tests
```

Expected: no matches. Any remaining `.tick()` matches must be inspected and confirmed to be another class.

- [ ] **Step 5: Build migrated callers**

Run `run-ecs`, `run-worldstream`, MatterViewer Windows, and ExplorerDemo Windows.

Expected: all compile and pass with no parameterless `WorldSession::tick()` declaration.

- [ ] **Step 6: Commit caller migration**

```powershell
git add MatterViewer/main.cpp MatterViewer/main_linux.cpp ExplorerDemo/main.cpp MatterEngine3/tests
git commit -m "refactor(ecs): pass explicit time to world sessions"
```

---

