### Task 2 Report: `raycast()` DSL verb

#### What Was Implemented

Five files changed:

1. **`MatterEngine3/src/dsl_state.h`** — added `bool DslState::raycast(const Vector3& origin, const Vector3& dir, Vector3& outPoint, Vector3& outNormal)` declaration in the public section after the voxel-emit group, with a brief doc comment matching the brief.

2. **`MatterEngine3/src/dsl_state.cpp`** — added:
   - A forward-declaration of `dsl::field_distance` in its own `namespace dsl {}` block at the top of the file (before the main `namespace dsl {` block). This avoids the float3 struct collision: `csg_lowering.h` transitively pulls in `bvh.h` which defines `struct float3 { x,y,z }` conflicting with raymath.h's `typedef struct float3 { v[3] } float3`. A forward declaration is the right pattern here, exactly like `dsl_triangle.cpp` splits off the `triangle_emit.hpp` collision.
   - `#include <cmath>` for `sqrtf`/`fmaxf`.
   - Full `DslState::raycast` implementation: fail-closed guard (outside session → set_error; no brushes → set_error; zero dir → set_error); normalization of direction; conservative sphere-trace (kStepScale=0.7, kMaxStep=0.5, kMaxT=100, kMaxSteps=512, kEps=1e-4); bisection refinement (32 iterations); central-difference normal (h = max(1e-3, 0.25 * spacing_)).

3. **`MatterEngine3/src/dsl_bindings.cpp`** — added `j_raycast` C function (reads 6 scalar args → calls `state_of(c)->raycast(...)` → returns `null` on miss/error or `{point:[x,y,z], normal:[x,y,z]}` on hit) and registered it with `bind("__dsl_raycast", j_raycast, 6)` next to `__dsl_op`/`__dsl_smoothing`.

4. **`MatterEngine3/src/part_base.js.h`** — added `raycast(o,d) { return __dsl_raycast(o[0],o[1],o[2], d[0],d[1],d[2]); }` after `smoothing`.

5. **`MatterEngine3/tests/script_host_tests.cpp`** — added 5 test functions and registered all 5 in `main()` at the end of the test list.

#### TDD Evidence

**RED run** (`make -C MatterEngine3/tests run-script` after writing tests, before implementing):
```
FAIL: raycast sphere probe: point+normal within tolerance
FAIL: raycast miss returns null
FAIL: error message names raycast
FAIL: raycast sees an earlier difference cut
make: *** [Makefile:679: run-script] Error 1
```
(4 of 5 tests failed; `raycast outside a voxel session fails the bake` accidentally passed because calling a non-existent method throws a JS TypeError making `r.error.ok == false`, but `error message names raycast` check failed since the message didn't contain "raycast".)

**GREEN run** (`make -C MatterEngine3/tests run-script` after implementation):
```
ALL PASS
```

**Pre-commit iso check** (`make -C MatterEngine3/tests run-iso`):
```
ALL PASS
```

#### Commit

`5a0e619` — `feat(dsl): raycast() — in-session analytic surface probe (point + normal)`

#### Architecture Note: Forward Declaration vs Include

The brief says "include `csg_lowering.h` at the top of the file". That cannot work in `dsl_state.cpp` because:
- `dsl_state.cpp` defines `RAYMATH_IMPLEMENTATION` and includes `raymath.h`, which defines `typedef struct float3 { float v[3]; } float3`
- `csg_lowering.h` → `cluster.h` → `vertex_ao.h` → `bvh.h` defines `struct float3 { x, y, z members }` — a collision
- This is the same reason `dsl_triangle.cpp` exists (it holds all `TriangleBuildBuffer`-touching code that pulls `triangle_emit.hpp`/`precomp.h`)

The forward declaration is the minimal deviation from the brief that respects the established architecture. No new `.cpp` file was needed (the brief lists only 4 source files to modify) and no Makefile changes were required.

#### Self-Review

- **Completeness**: All 5 tests implemented exactly as specified. All fail-closed cases covered (outside session, no brushes, zero-length direction). Hit case returns point+normal. Miss returns false (null in JS). The difference-cut test validates that CSG ops are visible to raycast.
- **Quality**: Names are clear. Code matches surrounding style (same brace style as `set_last_op`, same session-guard pattern). Forward-declaration comment explains the architecture decision.
- **Discipline**: Zero extras added. No overbuilding.
- **Testing**: TDD followed strictly. Tests verify real JS-level behavior (sphere-trace accuracy, miss detection, fail-closed error propagation to bake result).
- **Concerns**: None. The forward declaration is sound — `field_distance` is in the same translation group; the linker sees both objects. The `(void)fdPrev;` suppresses a would-be unused variable warning while keeping the variable for potential future debugging.
