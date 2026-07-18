# Box3D Runtime Physics Phase 2 — Final Review Fix Report

## Findings addressed

1. Reconciliation now consumes only observer-dirtied full entity IDs. The
   unconditional per-tick union of all `RigidBody` entities and live bridges is
   reserved for the fail-closed allocation fallback.
2. Physics-authored dynamic transform pulls survive deferred Flecs `OnSet`
   delivery through an O(1) full-ID set. The transform-specific observer verifies
   the delivered pose still matches Box3D before suppressing it, so a later user
   edit remains dirty.
3. Ray and overlap queries iterate validated live bridge shapes and apply the
   requested mask only to `b3Filter::categoryBits`. Ray casts use
   `b3Shape_RayCast`; overlaps use `b3Shape_GetClosestPoint`, preserving precise
   shape tests without simulation-mask coupling.
4. Equal-fraction rays select the smaller full generational entity ID.
5. `mark_for_reconcile` catches allocation failure and raises a scalar
   `full_reconcile_required` flag. A private one-shot seam proves the next pass
   audits all bodies/bridges and retires stale native state.

Obsolete Box3D world-query callback contexts were removed after the category-only
implementation made them unreachable.

## TDD evidence

After only the private failure-seam plumbing was added, the current-source MSVC
suite compiled and ran with five expected failures:

- unchanged static/dynamic hulls rebuilt on later ticks;
- the resulting descriptor build count was wrong;
- a category-matching ray missed a collider with `mask_bits == 0`;
- equal-fraction ray hits did not select the smaller full ID;
- category-matching overlap missed colliders with `mask_bits == 0`.

The forced dirty-mark failure happened to pass in the old implementation only
because its unconditional full scan masked the dropped mark. The final dirty-only
implementation explicitly activates that same audit through a non-allocating
fallback flag.

Regression coverage also proves descriptor replacement, invalid scale, parenting,
component removal, physics-authored dynamic pull, and a user transform edit in
`FixedPostUpdate` after Pull all take the correct path.

## Verification

- Focused current-source MSVC C++17 physics suite: `ALL PASS`.
- Fresh full MSVC build: exactly 49 Box3D C sources as C17, Flecs as C17, all
  current physics/ECS C++17 sources; physics and ECS executables both `ALL PASS`.
- Box3D Phase 2 static build-contract checker: `PASS`.
- `git diff --check`: `PASS`.
- Scope/dead-code scan found no retired world-query callbacks, temporary debug
  output, or stale suppression implementation.

GNU/MinGW and GPU/product gates remain unavailable on this host and are not
claimed passing.

## Second senior-review hardening round

The next senior review identified two scalability regressions in the first fix.
Tests first instrumented the current implementation and produced exactly three
runtime failures: moved bodies inserted heap markers, ray queries inspected every
bridge, and overlaps inspected every bridge.

`BridgeRecord` now carries `physics_transform_pending` directly. Pull sets the bit
before the deferred Flecs write; the transform observer consumes it, verifies the
final ECS pose against the live Box3D body, and dirties any real edit. A 64-body
movement fixture reports zero marker-allocation attempts, while the existing
post-Pull user scale edit still retires its body correctly.

Each context now owns one private `b3DynamicTree` indexing every live shape by AABB
and category bits. Bridge publication creates the proxy with the full generational
entity ID as user data; callbacks map-find before touching bridge memory.
Replacement publishes a new proxy before retiring the old; invalidation and
removal destroy it before freeing the bridge. Static pushes, public teleports,
stepped kinematic targets, gravity-driven dynamic body events, and preserved
replacement state update proxy bounds.

Ray and overlap queries traverse this category index and retain precise
`b3Shape_RayCast` / `b3Shape_GetClosestPoint` narrow phases. A fixture with 256
irrelevant-category bodies bounds each query to at most two precise candidates.
All prior zero-mask, category, tie, stale, world, stepping, sorting, and
deduplication regressions remain green.

Verification repeated the focused current-source suite (`ALL PASS`), then freshly
compiled exactly 49 Box3D C17 sources, Flecs C17, and all current physics/ECS C++17
sources; physics and ECS both printed `ALL PASS`. The expanded kinematic/dynamic
movement suite then passed against that fresh full link. Static checker and
diff/scope scans pass.
