# Stress-forest flatten-policy test

Encode as an automated check the invariant behind the recent bake-hardening work:
individual heavy parts (Tree) flatten *themselves* into a single merged artifact,
but a top-level scatter of 50k of them stays as `FlatInstanceRef`s and never
triggers a monolithic expansion.

## Motivation

Commits `96431f6` (per-part flatten decision + instance-boundary support) and
`42ae87f` (streaming flatten) landed the machinery: `part_flatten::FlattenTargets::budget_tri_bytes`
(default 512 MB) gates a bottom-up pass that marks each part `INLINE` or
`BOUNDARY`; parents of a `BOUNDARY` child emit a `FlatInstanceRef` in the
`.flat.part` trailer instead of inlining the child's triangles.

Meadow (`meadow_bake_check`) covers realistic 40k-instance mixed content, but its
scatter isn't heavy enough for the parent to trip the budget — every part there
comes back `INLINE`. Nothing in the automated test lane currently asserts that a
scatter *should* go `BOUNDARY`. If a future change silently regresses the
policy (raises the budget past sanity, drops the `BOUNDARY` propagation, etc.),
the failure mode is silent OOM at bake, not a red test.

## Changes

### Schemas — `MatterEngine3/examples/world_demo/schemas/StressForest{50k,100k,200k,500k}.js`

Each fixture keeps `COUNT` placements but picks the child kind by `i % 3`:

- bucket 0 — `placeChild('Pebble')`, scale `0.7–1.3` (unchanged)
- bucket 1 — `placeChild('Rock', { seed: 0 })`, scale `1.2–2.4` (larger)
- bucket 2 — `placeChild('Tree')`, scale `0.9–1.1`

`static requires` gains `Rock/seed=0` and `Tree`. Single seeded rng preserves
determinism. Stream length is exactly `COUNT`.

### Test — `MatterEngine3/tests/stress_forest_tests.cpp`

Replace the synthetic-hash harness with a real-content policy test built on the
existing full-pipeline stanza (mirrors `meadow_bake_check`; already includes
ScriptHost + QuickJS + compose + MSL backend, `-DMATTER_HAVE_SCRIPT_HOST`).

Per run:

1. Fresh sandbox with two independent cache dirs.
2. Bake `Pebble(seed=0)`, `Rock(seed=0)`, `Tree` via `ScriptHost` with
   `set_shared_lib_root(../examples/world_demo/shared-lib)` and the real schema
   sources. Capture their resolved hashes.
3. Bake `StressForest50k` with the three real child hashes.
4. Assertions in each cache:
   - `Tree.flat.part` present. `load_flat_v3` returns clusters with real merged
     triangles (Tree's own voxel trunk + TreeBranch twigs fused); `instance_refs`
     is empty. → *Tree is `INLINE`*.
   - `StressForest50k.flat.part` present. Load returns 0 clusters of merged
     geometry; `instance_refs.size() == 50000`, each pointing at one of
     `{Pebble, Rock, Tree}`'s resolved hash. → *StressForest50k is `BOUNDARY`*.
   - `bake_source` result `error.ok == true`.
5. Cross-cache determinism (preserved):
   - Same `resolved_hash` for `StressForest50k` across both caches.
   - Byte-identical `.flat.part` bytes across both caches.
   - Identical FNV-1a over the `FlatInstanceRef` stream
     (`child_resolved_hash` + `transform[16]`, in table order).

If a future bump raises `budget_tri_bytes` past sane bounds, or the `BOUNDARY`
propagation regresses, the `instance_refs.size() == 50000` assertion goes red
long before RSS blows up.

### Windows build

`make windows` in `MatterEngine3/viewer/` after code changes (per
`always_make_windows` — stale `viewer.exe` silently ships old engine).

## Explicitly out of scope

- **Not measuring FPS.** The tests lane is GL-free (headless). The property
  being tested (BOUNDARY → 50k `FlatInstanceRef`s at runtime) lands the fixture
  in exactly the shape GPU-instancing + GPU-culling already handle for Meadow.
  A separate scripted `viewer.exe` FPS smoke on Windows is a follow-up, not
  part of this change.
- **Not touching the 100k/200k/500k tests.** Only the schemas get the mix;
  the automated assertion runs against 50k for runtime sanity.
- **No new "LargeRock" module.** `Rock(seed=0)` at ~2x scale meets "larger
  rocks" without a new schema.
