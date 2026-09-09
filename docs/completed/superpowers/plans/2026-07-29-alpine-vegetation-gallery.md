# Alpine Vegetation Gallery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a realistic, dryness-parameterized Swiss alpine vegetation library and a standalone screenshot-review gallery using only the existing JavaScript DSL.

**Architecture:** A small shared helper module supplies deterministic vegetation geometry and color utilities. Six focused asset-family parts own their botanical forms, while one gallery part declares and places every comparison variant and one world supplies fixed presentation settings.

**Tech Stack:** QuickJS-ng ES modules, MatterEngine `Part`/`World` JavaScript DSL, existing seeded RNG helper, MatterEditor Vulkan screenshot capture.

## Global Constraints

- Authored assets, geometry, composition, and world setup must remain JavaScript-only.
- Do not modify C++, engine bindings, renderer code, terrain code, `WorldSector.js`, or `StreamMountain.js`.
- Every family accepts `{ seed, dryness, size, form }`; clamp `dryness` to `[0, 1]`.
- Identical parameters must produce deterministic output.
- Use direct triangles for thin vegetation and tapered DSL lines/tubes for woody structure.
- Prefer JavaScript reloads and the existing editor executable; build native code only if no compatible binary exists.
- Keep forms recognizable at close gallery distance without runaway bake cost.
- Sub-agents own visual judgment inside their family brief; the listed algorithms are suggestions, not a prescribed implementation.

## File Map

- Create `projects/world_demo/shared-lib/vegetation.js`: parameter, palette, frame, blade, leaf, petal, and cluster helpers.
- Create `projects/world_demo/objects/AlpineGrass.js`: four grass forms.
- Create `projects/world_demo/objects/AlpineFlower.js`: three flower forms.
- Create `projects/world_demo/objects/AlpineShrub.js`: four shrub forms.
- Create `projects/world_demo/objects/AlpineGroundCover.js`: two creeping forms.
- Create `projects/world_demo/objects/AlpineConifer.js`: three conifer forms.
- Create `projects/world_demo/objects/AlpineDeciduous.js`: three deciduous forms.
- Create `projects/world_demo/objects/VegetationGalleryGround.js`: neutral gallery ground.
- Create `projects/world_demo/objects/VegetationGallery.js`: fixed comparison layout and requirements.
- Create `projects/world_demo/worlds/VegetationGallery.js`: standalone world, camera, and lighting.

---

### Task 1: Shared Vegetation Helper Contract

**Files:**
- Create: `projects/world_demo/shared-lib/vegetation.js`

**Interfaces:**
- Consumes: `rng(seed)` from `shared-lib/rng`.
- Produces: `clamp01`, `vegetationParams`, `mixColor`, `dryPalette`,
  `emitBlade`, `emitLeaf`, `emitPetal`, and compact frame/vector helpers needed
  by multiple families.

- [ ] **Step 1: Define the exported contract**

Keep helper inputs plain arrays/numbers and pass the active `Part` into emission
helpers. `vegetationParams(p, formCount)` returns normalized
`{ seed, dryness, size, form }`.

- [ ] **Step 2: Implement deterministic geometry helpers**

Use five-vertex tapered strips for bent blades, small fans/diamonds for leaves
and petals, and stable perpendicular-frame fallbacks for near-vertical stems.
Emission helpers must not own RNG state.

- [ ] **Step 3: Check the module mechanically**

Run:

```powershell
rg -n "export function (clamp01|vegetationParams|mixColor|dryPalette|emitBlade|emitLeaf|emitPetal)" projects/world_demo/shared-lib/vegetation.js
```

Expected: one exported definition for every named helper and no native-file
changes in `git status --short`.

### Task 2: Grasses and Flowers

**Files:**
- Create: `projects/world_demo/objects/AlpineGrass.js`
- Create: `projects/world_demo/objects/AlpineFlower.js`

**Interfaces:**
- Consumes: `vegetationParams`, dryness palette utilities, and direct-geometry
  helpers from `shared-lib/vegetation`.
- Produces: `AlpineGrass` forms `0..3` and `AlpineFlower` forms `0..2`.

- [ ] **Step 1: Author four grass silhouettes**

Create cropped turf, fine meadow grass, tall seed grass, and a dense outward
tussock. Golden-angle or stratified azimuth placement is a useful starting
algorithm; keep enough seeded asymmetry that the clumps do not read as rosettes.

- [ ] **Step 2: Author three flower silhouettes**

Create alpine daisy, hanging bellflower, and clustered clover-like forms.
Radial petals, curved/hanging stem frames, and small clustered heads are useful
starting ideas. Let dryness replace some blooms with seed heads and bent stems.

- [ ] **Step 3: Bake-test the family variants**

Use the gallery integration from Task 5 once available. Before then, verify all
forms route from a clamped `form` and all random choices originate from `seed`.

### Task 3: Shrubs and Ground Cover

**Files:**
- Create: `projects/world_demo/objects/AlpineShrub.js`
- Create: `projects/world_demo/objects/AlpineGroundCover.js`

**Interfaces:**
- Consumes: shared vegetation helpers and `rng`.
- Produces: `AlpineShrub` forms `0..3` and `AlpineGroundCover` forms `0..1`.

- [ ] **Step 1: Author four shrub architectures**

Create a low cushion shrub, rounded broadleaf bush, sparse woody scrub, and a
berry-bearing shrub. A seeded recursive branch skeleton or several biased
radial stems are both appropriate. Preserve an irregular outer contour and
make dryness reveal wood in patches rather than thinning every branch equally.

- [ ] **Step 2: Author two ground-running forms**

Create a creeping leafy vine and a flowering runner. Use multiple low branching
polylines with alternating leaves, occasional lifted tips, and visibly rooted
nodes. Dryness may terminate live sections early while leaving brown runner
segments in place.

- [ ] **Step 3: Inspect grounding and continuity**

Confirm woody bases extend slightly below `y=0`, runners remain close to the
ground, and no leaf clusters are disconnected from a stem.

### Task 4: Conifer and Deciduous Trees

**Files:**
- Create: `projects/world_demo/objects/AlpineConifer.js`
- Create: `projects/world_demo/objects/AlpineDeciduous.js`

**Interfaces:**
- Consumes: shared vegetation helpers and `rng`.
- Produces: `AlpineConifer` forms `0..2` and `AlpineDeciduous` forms `0..2`.

- [ ] **Step 1: Author three conifer crown structures**

Create irregular multi-stem mountain pine, tapered drooping Norway spruce, and
layered narrow silver fir. Whorled branch levels with seeded phase offsets are
a useful base algorithm. Represent needle masses as compact clustered geometry,
not individual needles.

- [ ] **Step 2: Author three deciduous crown structures**

Create dense rounded European beech, open pale-trunk birch/aspen, and broad
irregular sycamore maple. Use different branch angles, crown envelopes, leaf
sizes, and trunk treatments so the forms remain recognizable in silhouette.

- [ ] **Step 3: Apply nonuniform dryness**

Hash branches into live, stressed, and dead groups using the seed plus branch
index. Increase canopy gaps and dead tips continuously while preserving trunks
and major structure.

- [ ] **Step 4: Check cost and topology**

Keep branch recursion bounded and leaf/needle cluster counts explicit. Confirm
every foliage cluster connects visually to the crown and each tree is rooted
slightly below `y=0`.

### Task 5: Standalone Gallery

**Files:**
- Create: `projects/world_demo/objects/VegetationGalleryGround.js`
- Create: `projects/world_demo/objects/VegetationGallery.js`
- Create: `projects/world_demo/worlds/VegetationGallery.js`

**Interfaces:**
- Consumes: all six asset families.
- Produces: a committed world selectable with
  `MATTER_WORLD=VegetationGallery`.

- [ ] **Step 1: Declare the complete variant set**

`VegetationGallery.static requires` lists every family/form combination at
dryness `0.0`, `0.35`, `0.7`, and `1.0` with fixed seeds. Do not generate
requirements conditionally at build time.

- [ ] **Step 2: Lay out three review zones**

Place small plants, shrubs/ground covers, and trees in separate zones. Use four
left-to-right dryness columns and family-specific row spacing. Reuse the same
seed across each four-item comparison.

- [ ] **Step 3: Add presentation geometry and world settings**

Use a matte neutral ground with subtle row pads or markers, fixed daylight,
gentle sky fill, and a default camera that establishes the gallery without
making the plants tiny.

- [ ] **Step 4: Verify world discovery and initial bake**

Launch the existing editor from `MatterEditor/` with:

```powershell
$env:MATTER_WORLD='VegetationGallery'
$env:MATTER_SCREENSHOT='<absolute-output-path>\overview.png'
.\build\windows\editor.exe
```

Expected: the world is discovered, the bake completes without script errors,
the screenshot is written, and the process exits.

### Task 6: Screenshot-Driven Visual Tuning

**Files:**
- Modify: the JavaScript files above as visual evidence requires.

**Interfaces:**
- Consumes: the complete gallery and existing editor capture controls.
- Produces: reviewed screenshots for small plants, shrubs/ground cover, and
  trees.

- [ ] **Step 1: Capture fixed views**

Capture an overview plus one close view per zone. Keep camera, lighting, seed,
and dryness columns fixed between iterations.

- [ ] **Step 2: Run a visual review**

Check family distinction, dryness progression, silhouette, grounding,
connections, card artifacts, repetition, intersections, and plausible Swiss
alpine character. Record specific visual defects rather than making broad
untargeted changes.

- [ ] **Step 3: Tune and reload**

Prioritize JavaScript constant changes and live reload. Aim for three passes;
continue only for a named defect visible in the latest screenshot.

- [ ] **Step 4: Run final verification**

Confirm `git diff --check`, inspect every final screenshot, and verify
`git status --short` contains only intended JavaScript assets, the gallery
world, the design, and this plan.
