# Task 3 Report: Rock.js Rewrite — Ellipsoid Body + Raycast-Placed Facet Cuts

## Status: DONE

---

## What Was Implemented

Rewrote `MatterEngine3/examples/world_demo/schemas/Rock.js` in full per the brief:

- **Imports**: Added `import { add, sub, scale as vscale, normalize, length } from 'shared-lib/vecmath'` alongside existing `rng` import. The `scale` alias to `vscale` avoids the name collision with `this.scale()`.
- **Voxel spacing**: Changed from `0.15` to `0.10`; smoothing from `0.12` to `0.06`.
- **Blob body (4-7 ellipsoids)**: Replaced the old 3-5 random sphere/box approach with an ellipsoid-based construction: a dominant yaw axis is chosen, stretch (1.1-1.5) and squashY (0.65-0.9) set the anisotropy, then 4-7 blobs are placed along that axis via `pushMatrix -> translate -> rotateY -> rotateX -> scale -> sphere -> popMatrix`. Each blob is jittered slightly from the axis.
- **Facet cuts (5-9)**: Each cut probes the live surface via `this.raycast()` from outside the body (origin = centroid + dir*3, pointing inward). On a hit, the surface normal is jittered ~25 degrees and a depth `t` is computed (`min(range(0.03, 0.12), 0.45 * hitDist)`) — the 0.45 clamp ensures no L-shape gouges (plane stays within 55% of centroid-surface distance). A box at `q + m*B` (sized 2B) is aimed via `lookAt`, rolled with `rotateZ`, then subtracted via `this.difference()`. Misses skip (`if (!hit) continue`).
- **Retopo modifier**: Preserved unchanged (`target_ratio: 1.0, iterations: 3, seed: 42, timeout_seconds: 60`).
- **Public contract**: `static params = { seed: 0 }`, class name `Rock`, Meadow interface unchanged.

---

## Gate Commands and Results

```
make -C MatterEngine3/tests run-script
```
Result: ALL PASS (no new warnings or errors)

```
make -C MatterEngine3/tests run-meadow-check
```
Result: ALL PASS — baked 10 artifacts, 269 hits, 44896 children, 276 unique variants.

Pre-existing messages only (not from Rock.js changes):
- `[modifier] ... retopo unavailable (built without autoremesher), skipped` — expected; autoremesher not linked in test build
- `Warning: No draw records to build TLAS from` — pre-existing, not from Rock.js

---

## Files Changed

- `MatterEngine3/examples/world_demo/schemas/Rock.js` — full rewrite (62 insertions, 19 deletions)

---

## Commit

`7adb8cd feat(rock): ellipsoid blob body + raycast-placed facet cuts (0.10 spacing, 0.06 smoothing)`

---

## Self-Review Findings

- **Completeness**: All elements present — blob counts (4-7), cut counts (5-9), 0.10 spacing, 0.06 smoothing, depth clamp guard (0.45 * hitDist), jitter magnitude (~0.45 per axis), lookAt + rotateZ, difference(), retopo modifier preserved.
- **Quality**: Matches the brief's code verbatim. No leftover old Rock.js code — all previous content removed.
- **Discipline**: No extras added beyond the brief. No FIXMEs, no parallel implementations.
- **Gates**: Both commands pass; no bake errors or new warnings introduced by Rock.js.

---

## Concerns

None. Output is clean. The "retopo unavailable" and "No draw records" messages were present before this task and are unrelated to the Rock.js changes.
