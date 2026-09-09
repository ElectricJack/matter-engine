# Alpine Streaming Vegetation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `StreamMountain`'s generic tree and grass scatter with deterministic, ecologically placed instances of all 19 alpine vegetation forms.

**Architecture:** A pure JavaScript ecology module owns the fixed asset catalog, habitat fields, suitability rules, dryness selection, and sector placement planning. `WorldSector` remains the thin DSL boundary: it chooses the alpine profile, asks the module for placements, and emits declared child instances; worlds without the profile retain the legacy path.

**Tech Stack:** MatterEngine JavaScript DSL, ECMAScript modules, Node.js 20 for headless behavior tests, existing `scatter_grid` shared library.

## Global Constraints

- Production integration is JavaScript-only; do not modify C++, renderer, materials, terrain generation, or build files.
- `StreamMountain` uses the profile string exactly `"alpine-lush"`.
- Other streaming worlds retain legacy generic vegetation behavior.
- The fixed dryness states are exactly `[0.0, 0.35, 0.7, 1.0]`.
- Hard tree ceiling is `455.0` m and hard vegetation ceiling is `520.0` m.
- Hard per-sector placement caps are trees `36`, shrubs `96`, ground cover `72`, flowers `96`, and grass `900`.
- Rung 0 includes trees; rung 1 additionally includes shrubs; rung 2 additionally includes ground cover, flowers, and grass.
- Candidate identity controls every stochastic choice; adding a near-tier family must not change persistent placements.
- Every emitted `{module, params}` pair must appear in `static requires`.
- Do not launch the editor or capture screenshots in this execution; verification is headless JavaScript behavior and syntax only.

---

## File Structure

| File | Responsibility |
|---|---|
| `projects/world_demo/shared-lib/alpine_ecology.js` | Fixed catalog, habitat fields, species suitability, dryness state selection, and pure sector placement planning |
| `projects/world_demo/tests/alpine_ecology_tests.mjs` | Node behavior tests against the real ecology module and real scatter grid |
| `projects/world_demo/objects/WorldSector.js` | Profile dispatch, fixed requirements, and placement emission |
| `projects/world_demo/worlds/StreamMountain.js` | Opt into the `alpine-lush` profile and remove generic vegetation counts |

### Task 1: Pure Alpine Ecology and Asset Catalog

**Files:**
- Create: `projects/world_demo/shared-lib/alpine_ecology.js`
- Create: `projects/world_demo/tests/alpine_ecology_tests.mjs`

**Interfaces:**
- Produces:

```js
export const ALPINE_PROFILE = 'alpine-lush';
export const DRYNESS_STATES = Object.freeze([0.0, 0.35, 0.7, 1.0]);
export const FAMILY_CAPS = Object.freeze({
  tree: 36, shrub: 96, groundCover: 72, flower: 96, grass: 900,
});
export const FAMILY_SLOPE_MAX = Object.freeze({
  tree: 0.625, shrub: 0.675, groundCover: 0.781,
  flower: 0.675, grass: 0.781,
});

export function isAlpineProfile(table);
export function alpineAssetVariants();
export function selectVegetationCatalog(table, legacyVariants);
export function environmentalDryness({ moisture, exposure, altitude, slope });
export function sampleHabitat({ x, z, altitude, slope, worldSeed });
export function selectDrynessState(dryness, identity);
export function selectAlpineAsset(family, habitat, identity);
export function familiesForRung(rung);
```

- `alpineAssetVariants()` returns one declared entry for every combination of
  19 forms and four dryness states: exactly 76 entries. Form seeds and sizes
  match the existing gallery rows so their cached bakes can be reused.
- `selectAlpineAsset` returns `null` when unsuitable, otherwise:

```js
{
  module: 'AlpineConifer',
  params: { seed: 3101, dryness: 0.35, size: 1.85, form: 0 },
  scale: 1.0,
  sinkY: 0.18,
}
```

- `sampleHabitat` returns finite clamped fields including `moisture`,
  `exposure`, `dryness`, `forest`, `forestEdge`, `shrubPatch`, `meadowPatch`,
  `flowerPatch`, and `groundCoverPatch`.

- [ ] **Step 1: Write the failing catalog/profile tests**

Create `projects/world_demo/tests/alpine_ecology_tests.mjs` with literal
assertions that name the behaviors:

```js
import assert from 'node:assert/strict';
import {
  ALPINE_PROFILE, DRYNESS_STATES, FAMILY_CAPS,
  FAMILY_SLOPE_MAX, isAlpineProfile, alpineAssetVariants,
  selectVegetationCatalog,
} from '../shared-lib/alpine_ecology.js';

assert.equal(ALPINE_PROFILE, 'alpine-lush');
assert.deepEqual(DRYNESS_STATES, [0, 0.35, 0.7, 1]);
assert.deepEqual(FAMILY_CAPS,
  { tree: 36, shrub: 96, groundCover: 72, flower: 96, grass: 900 });
assert.deepEqual(FAMILY_SLOPE_MAX, {
  tree: 0.625, shrub: 0.675, groundCover: 0.781,
  flower: 0.675, grass: 0.781,
});
assert.equal(isAlpineProfile({ __vegetation: { profile: 'alpine-lush' } }), true);
assert.equal(isAlpineProfile({ __vegetation: { profile: 'unknown' } }), false);
assert.equal(isAlpineProfile(null), false);

const variants = alpineAssetVariants();
assert.equal(variants.length, 76);
assert.equal(new Set(variants.map(v =>
  `${v.module}:${v.params.form}:${v.params.dryness}`)).size, 76);
assert.deepEqual([...new Set(variants.map(v => v.params.dryness))],
  [0, 0.35, 0.7, 1]);
const legacy = [{ module: 'Grass', params: { seed: 0 } }];
assert.deepEqual(selectVegetationCatalog({}, legacy), legacy);
assert.equal(selectVegetationCatalog(
  { __vegetation: { profile: 'alpine-lush' } }, legacy).length, 76);
```

Continue with literal per-module form counts:

```js
assert.deepEqual(Object.fromEntries(
  ['AlpineGrass', 'AlpineFlower', 'AlpineShrub', 'AlpineGroundCover',
   'AlpineConifer', 'AlpineDeciduous'].map(module => [
     module,
     new Set(variants.filter(v => v.module === module)
       .map(v => v.params.form)).size,
   ])),
  {
    AlpineGrass: 4, AlpineFlower: 3, AlpineShrub: 4,
    AlpineGroundCover: 2, AlpineConifer: 3, AlpineDeciduous: 3,
  });
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```powershell
node --experimental-default-type=module projects/world_demo/tests/alpine_ecology_tests.mjs
```

Expected: failure because `shared-lib/alpine_ecology.js` does not exist.

- [ ] **Step 3: Implement the catalog and profile guard**

Create `alpine_ecology.js` with a single frozen form table whose rows use the
gallery's exact module, form, seed, size, and sink values. Expand that table
over `DRYNESS_STATES` in `alpineAssetVariants()`. Return fresh `{module,
params}` records so callers cannot mutate the catalog.

Implement `isAlpineProfile` as a null-safe exact profile check. Do not accept
prefixes, truthy values, or alternative spellings.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the same Node command. Expected: catalog/profile assertions pass.

- [ ] **Step 5: Add failing habitat, dryness, and species tests**

Test deterministic and environmental behavior with literal fixtures:

```js
const base = { x: 125.5, z: -81.25, altitude: 180, slope: 0.2, worldSeed: 77 };
assert.deepEqual(sampleHabitat(base), sampleHabitat(base));

const invalid = sampleHabitat(
  { x: NaN, z: 0, altitude: 100, slope: 0.1, worldSeed: 1 });
assert.equal(invalid.valid, false);

assert.ok(environmentalDryness(
  { moisture: 0.2, exposure: 0.5, altitude: 200, slope: 0.2 }) >
  environmentalDryness(
  { moisture: 0.8, exposure: 0.5, altitude: 200, slope: 0.2 }));
assert.ok(environmentalDryness(
  { moisture: 0.5, exposure: 0.8, altitude: 350, slope: 0.4 }) >
  environmentalDryness(
  { moisture: 0.5, exposure: 0.2, altitude: 100, slope: 0.1 }));

assert.equal(selectDrynessState(-1, 0.2), 0);
assert.equal(selectDrynessState(2, 0.2), 1);
assert.ok(DRYNESS_STATES.includes(selectDrynessState(0.56, 0.1)));
assert.notEqual(selectDrynessState(0.56, 0.1),
                selectDrynessState(0.56, 0.9));

assert.deepEqual(familiesForRung(0), ['tree']);
assert.deepEqual(familiesForRung(1), ['tree', 'shrub']);
assert.deepEqual(familiesForRung(2),
  ['tree', 'shrub', 'groundCover', 'flower', 'grass']);
```

Add a reachability table of hand-authored habitat fixtures. Across the table,
assert the returned assets cover all expected module/form keys:

```js
[
  'AlpineConifer:0', 'AlpineConifer:1', 'AlpineConifer:2',
  'AlpineDeciduous:0', 'AlpineDeciduous:1', 'AlpineDeciduous:2',
  'AlpineShrub:0', 'AlpineShrub:1', 'AlpineShrub:2', 'AlpineShrub:3',
  'AlpineGroundCover:0', 'AlpineGroundCover:1',
  'AlpineFlower:0', 'AlpineFlower:1', 'AlpineFlower:2',
  'AlpineGrass:0', 'AlpineGrass:1', 'AlpineGrass:2', 'AlpineGrass:3',
]
```

Test hard gates independently of high suitability:

```js
assert.equal(selectAlpineAsset('tree',
  { ...lushTreeHabitat, altitude: 455.01 }, 0.5), null);
for (const family of ['tree', 'shrub', 'groundCover', 'flower', 'grass'])
  assert.equal(selectAlpineAsset(family,
    { ...bestHabitatFor[family], altitude: 520.01 }, 0.5), null);
```

Include per-family over-slope fixtures using the exact literal limits from
`FAMILY_SLOPE_MAX`. The assertion must observe `null`, not inspect source text.

- [ ] **Step 6: Run the test and verify RED**

Expected: missing exported functions or failing habitat/species behavior.

- [ ] **Step 7: Implement habitat and species selection**

Implement deterministic hash/value-noise/FBM locally with no imports.
Environmental dryness must increase with inverse moisture, exposure, altitude,
and slope, then clamp to `[0, 1]`.

Use smooth suitability products rather than one giant branch. Hard altitude
and slope gates run last and return `null`. Species routing must cover the
rules in the design spec, and it must only return parameter records present in
the fixed catalog. Use identity-derived channels for acceptance, form choice,
dryness interpolation, and scale so those choices do not share mutable RNG
state.

- [ ] **Step 8: Run tests, syntax-check, and commit**

Run:

```powershell
node --experimental-default-type=module projects/world_demo/tests/alpine_ecology_tests.mjs
Get-Content -Raw projects/world_demo/shared-lib/alpine_ecology.js |
  node --input-type=module --check
git diff --check
```

Expected: all assertions pass, syntax check exits 0, and `git diff --check`
prints nothing.

Commit:

```powershell
git add projects/world_demo/shared-lib/alpine_ecology.js `
        projects/world_demo/tests/alpine_ecology_tests.mjs
git commit -m "feat(world): add deterministic alpine ecology model"
```

### Task 2: Sector Planner and StreamMountain Integration

**Files:**
- Modify: `projects/world_demo/shared-lib/alpine_ecology.js`
- Modify: `projects/world_demo/tests/alpine_ecology_tests.mjs`
- Modify: `projects/world_demo/objects/WorldSector.js`
- Modify: `projects/world_demo/worlds/StreamMountain.js`

**Interfaces:**
- Consumes all Task 1 exports.
- Adds:

```js
export function planAlpineSector({
  rung, worldSeed, ox, oz, sectorSize,
  heightAt, slopeAt, candidatesInRect,
});
```

- Returns ordered placement records:

```js
{
  family: 'tree',
  x: 12.5,
  z: -8.0,
  rotation: 1.7,
  scale: 1.1,
  sinkY: 0.18,
  module: 'AlpineConifer',
  params: { seed: 3101, dryness: 0.35, size: 1.85, form: 0 },
}
```

- [ ] **Step 1: Write failing sector-planner behavior tests**

Import the real scatter grid:

```js
import { candidatesInRect } from
  '../../../MatterEngine3/shared-lib/scatter_grid.js';
```

Add tests using real candidates and controlled terrain callbacks:

```js
const flatHeight = () => 180;
const gentleSlope = () => 0.1;
const args = {
  worldSeed: 20260722, ox: 0, oz: 0, sectorSize: 64,
  heightAt: flatHeight, slopeAt: gentleSlope, candidatesInRect,
};
const far = planAlpineSector({ ...args, rung: 0 });
const mid = planAlpineSector({ ...args, rung: 1 });
const near = planAlpineSector({ ...args, rung: 2 });

assert.ok(far.every(p => p.family === 'tree'));
assert.ok(mid.every(p => ['tree', 'shrub'].includes(p.family)));
assert.ok(near.every(p =>
  ['tree', 'shrub', 'groundCover', 'flower', 'grass'].includes(p.family)));
```

Compare persistent placement keys, not array prefixes:

```js
const key = p => JSON.stringify(
  [p.family, p.x, p.z, p.rotation, p.scale, p.module, p.params]);
assert.deepEqual(far.map(key).sort(),
  near.filter(p => p.family === 'tree').map(key).sort());
assert.deepEqual(mid.map(key).sort(),
  near.filter(p => p.family === 'tree' || p.family === 'shrub')
    .map(key).sort());
```

Test that a height callback returning `456` emits no trees and one returning
`521` emits no vegetation. Test a steep slope callback above every hard limit
emits no vegetation. Count placements per family and assert every count is at
or below `FAMILY_CAPS`.

Test four adjacent 64 m sectors with the real scatter grid. Assert every
placement belongs to exactly one sector, no placement key is duplicated, and
the combined placements are unchanged when the four sectors are planned in a
different call order. This exercises world-space ownership without adding a
non-production planner mode.

- [ ] **Step 2: Run the test and verify RED**

Run:

```powershell
node --experimental-default-type=module projects/world_demo/tests/alpine_ecology_tests.mjs
```

Expected: failure because `planAlpineSector` is not exported.

- [ ] **Step 3: Implement the pure sector planner**

Define fixed family specs with stable kind IDs and minimum distances.
For each family enabled by `familiesForRung`, call the injected real
`candidatesInRect`. For each candidate:

1. sample height and slope once;
2. derive habitat with `sampleHabitat`;
3. derive independent stable identities from world position, seed, family,
   and purpose;
4. call `selectAlpineAsset`;
5. append a placement if accepted;
6. stop the family loop at its exact cap.

Ordering must be deterministic. No mutable RNG stream may be shared between
families.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run the Node test. Expected: all catalog, habitat, species, tier-stability,
hard-gate, cap, and sector-seam assertions pass.

- [ ] **Step 5: Add the alpine profile to `StreamMountain`**

Change `biomes()` to include:

```js
__vegetation: { profile: "alpine-lush" },
```

Remove the generic `grass` counts from the `foothills` and `meadow` entries.
Do not change terrain material, rocks, pebbles, the field, surface
classification, streaming rings, or terrain bands.

- [ ] **Step 6: Integrate the planner into `WorldSector`**

Import the Task 1/2 API. In `assetVariants(biomesJson)`:

- detect `alpine-lush`;
- retain rocks and boulders;
- append `alpineAssetVariants()`;
- omit generic `Grass` and `Tree`;
- preserve the existing legacy list for every other table.

In `build(p)`, after terrain and shared rock/boulder setup:

- dispatch `planAlpineSector` for the alpine profile;
- emit each result through a dedicated transform helper using the planned
  world position, `heightAt`, rotation, scale, and sink;
- return before the legacy generic tree/grass blocks;
- keep ordinary rocks at rung 1 and higher without consuming planner state.

Do not alter the transform or selection of legacy-world vegetation.

- [ ] **Step 7: Add requirements/placement correspondence tests**

Use `alpineAssetVariants()` as the allowed-key set and assert every planned
placement belongs to it:

```js
const allowed = new Set(alpineAssetVariants().map(v =>
  `${v.module}:${JSON.stringify(v.params)}`));
for (const p of near)
  assert.ok(allowed.has(`${p.module}:${JSON.stringify(p.params)}`));
```

Test `selectVegetationCatalog`, the same helper used by
`WorldSector.static requires`. Its observable contract is:

- alpine profile: rocks/boulders plus 76 alpine variants, no generic
  `Tree` or `Grass`;
- legacy profile: the prior generic requirements and no alpine modules.

- [ ] **Step 8: Run complete headless JavaScript verification**

Run:

```powershell
node --experimental-default-type=module projects/world_demo/tests/alpine_ecology_tests.mjs
Get-Content -Raw projects/world_demo/shared-lib/alpine_ecology.js |
  node --input-type=module --check
Get-Content -Raw projects/world_demo/objects/WorldSector.js |
  node --input-type=module --check
Get-Content -Raw projects/world_demo/worlds/StreamMountain.js |
  node --input-type=module --check
git diff --check
```

Expected: tests pass, all three JavaScript syntax checks exit 0, and
`git diff --check` prints nothing.

- [ ] **Step 9: Commit**

```powershell
git add projects/world_demo/shared-lib/alpine_ecology.js `
        projects/world_demo/tests/alpine_ecology_tests.mjs `
        projects/world_demo/objects/WorldSector.js `
        projects/world_demo/worlds/StreamMountain.js
git commit -m "feat(world): scatter alpine vegetation by habitat"
```

## Final Verification

After both reviewed tasks, run once from the feature worktree:

```powershell
node --experimental-default-type=module projects/world_demo/tests/alpine_ecology_tests.mjs
$files = @(
  'projects/world_demo/shared-lib/alpine_ecology.js',
  'projects/world_demo/objects/WorldSector.js',
  'projects/world_demo/worlds/StreamMountain.js',
  'projects/world_demo/objects/AlpineGrass.js',
  'projects/world_demo/objects/AlpineFlower.js',
  'projects/world_demo/objects/AlpineShrub.js',
  'projects/world_demo/objects/AlpineGroundCover.js',
  'projects/world_demo/objects/AlpineConifer.js',
  'projects/world_demo/objects/AlpineDeciduous.js'
)
foreach ($file in $files) {
  Get-Content -Raw $file | node --input-type=module --check
  if ($LASTEXITCODE -ne 0) { throw "syntax failure: $file" }
}
git diff --check HEAD~2..HEAD
git status --short
```

Expected: all behavior tests and nine syntax checks pass, diff check is clean,
and the worktree has no tracked modifications.
