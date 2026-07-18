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
