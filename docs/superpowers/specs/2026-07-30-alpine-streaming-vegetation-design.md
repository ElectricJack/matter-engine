# Alpine Streaming Vegetation Design

**Date:** 2026-07-30
**Status:** Approved for specification review
**Scope:** JavaScript-only ecological placement of the alpine vegetation assets in `StreamMountain`

## Goal

Replace `StreamMountain`'s generic `Tree` and `Grass` scatter with the complete
procedural alpine vegetation collection. Placement must look intentional:
altitude, slope, habitat moisture, exposure, and coherent world-space noise
determine what grows at each point. The result should be a lush showcase with
connected forests, visible meadow colonies, subalpine shrub belts, a ragged
treeline, and empty cliffs and high snow terrain.

The integration remains deterministic across streaming sector boundaries and
detail-tier changes. It uses the existing JavaScript DSL and editor executable;
no native engine or renderer changes are planned.

## Constraints

- All integration logic and authored placement remain in JavaScript.
- `StreamMountain` replaces its generic `Tree` and `Grass` scatter.
- Other streaming worlds keep their existing generic vegetation behavior.
- All 19 alpine forms are reachable in the mountain landscape:
  - four grasses;
  - three flowers;
  - four shrubs;
  - two ground covers;
  - three conifers;
  - three deciduous trees.
- Vegetation uses the existing visual dryness states `0.0`, `0.35`, `0.7`, and
  `1.0`.
- Every child parameter combination is declared by `static requires`; placement
  never relies on a missing or dynamically invented asset variant.
- Candidate identity, not loop order, determines placement decisions.
- Cross-sector candidate ownership and transforms are stable.
- Existing boulder and scree scatter remains available.
- Prefer JavaScript rebakes and editor relaunches over native builds.

## Selected Architecture

### Alpine ecology module

Add a focused `projects/world_demo/shared-lib/alpine_ecology.js` module. It owns:

- the fixed streaming asset catalog;
- world-space deterministic FBM/value-noise utilities;
- altitude, slope, moisture, exposure, and patch calculations;
- family and species suitability functions;
- continuous environmental dryness;
- deterministic selection of one of the four baked dryness states;
- placement constants, species routing, and per-family limits.

The module is pure JavaScript. Terrain query values are supplied by
`WorldSector`, keeping native `Part` methods and transform emission at the
integration boundary. Pure suitability functions can therefore be exercised
without launching the renderer.

### Opt-in world profile

`StreamMountain.biomes()` adds a reserved world-level vegetation entry:

```js
__vegetation: {
  profile: "alpine-lush",
}
```

`WorldSector` recognizes this profile. Its `static requires(p)` returns the
alpine catalog instead of the old generic `Tree` and `Grass` variants. Its
`build(p)` dispatches vegetation to the alpine placement path while preserving
terrain, boulders, rocks, and scree.

An absent, malformed, or unknown vegetation profile uses the existing legacy
scatter path. This preserves `StreamMeadow`, proof worlds, fixtures, and future
world definitions that do not opt in.

### Streaming tiers

Vegetation follows the existing scatter rings:

| Detail tier | Radius | Vegetation |
|---|---:|---|
| Far, rung 0 | 1,000 m | Trees and landmark boulders |
| Mid, rung 1 | 500 m | Trees, shrubs, scrub, boulders, and rocks |
| Near, rung 2 | 150 m | All vegetation, rocks, grasses, flowers, and ground cover |

Every family uses `candidatesInRect` with a unique stable kind identifier and
species-appropriate minimum distance. World-space noise gates candidates after
enumeration. A family visible in multiple tiers evaluates the same candidates
and receives the same species, dryness, transform, and scale in every tier.
Adding a near-only family cannot consume randomness that moves a far-tier tree.

## Environmental Model

For each candidate, `WorldSector` supplies:

- `x` and `z` in world coordinates;
- terrain altitude from `heightAt(x, z)`;
- terrain gradient from `slopeAt(x, z)`;
- world seed;
- candidate identity values from the scatter grid.

The ecology module derives:

- **Habitat moisture:** broad 250–350 m noise blended with 40–70 m local
  variation. This is separate from `moistureAt`, because `StreamMountain`
  intentionally makes its native biome moisture channel inert.
- **Exposure:** a broad independent noise field that breaks up equally moist
  sites into sheltered and exposed ground.
- **Environmental dryness:** a clamped continuous combination of inverse
  moisture, exposure, increasing altitude, and a smaller slope contribution.
- **Guild patches:** independent fields for forest mass, forest clearings,
  shrubs, meadow grass, flowers, and creeping ground cover.
- **Forest edge:** a narrow suitability band around the forest field threshold.
  Edge-preferring species use this rather than appearing uniformly inside or
  outside the forest.

Noise is evaluated in world coordinates, never sector-local coordinates, so
patches continue seamlessly across sector boundaries.

### Dryness quantization

Asset geometry is pre-baked at `0.0`, `0.35`, `0.7`, and `1.0`, but habitat
dryness is continuous. For a dryness value between two baked states, a stable
candidate hash chooses the lower or upper neighbor in proportion to the
fractional distance.

For example, environmental dryness `0.56` lies between `0.35` and `0.7`.
Candidates in that habitat deterministically distribute across those two
states, weighted toward `0.7`. This prevents visible contour bands while
keeping the variant catalog finite and stable.

## Ecological Zonation

| Elevation | Character |
|---|---|
| 20–180 m | Lush valley meadow, beech and maple, fir and spruce pockets, flowers, and ground cover |
| 150–320 m | Mixed forest and clearings; conifers become increasingly dominant |
| 280–455 m | Subalpine conifers, mountain pine, woody shrubs, tussocks, and hardy flowers |
| 400–520 m | Treeless alpine vegetation on gentle ground: short grass, cushion shrubs, sparse flowers, and creeping cover |
| Above 520 m | No procedural vegetation |

Altitude ranges overlap deliberately. Suitability curves and patch fields choose
the local composition; the table does not create hard horizontal bands.

### Treeline and vegetation ceiling

- Deciduous forms taper out between approximately 300 m and 360 m, depending on
  species.
- Tree density begins a broad decline at 330 m.
- No tree candidate survives above the hard 455 m treeline.
- Only low alpine vegetation survives from the treeline to 520 m.
- No procedural vegetation survives above the hard 520 m ceiling.

The declining suitability is perturbed by forest and exposure fields before
the hard cap, producing fingers of trees in sheltered locations and lower
treelines on exposed ground without violating the absolute ceiling.

### Slope gates

`slopeAt` returns terrain gradient magnitude. The ecological rules convert the
selected visual angles to gradient thresholds.

- Trees fade after roughly 22 degrees and are hard-rejected around 29–32
  degrees, with small species differences.
- Tall shrubs and flowers are hard-rejected around 34 degrees.
- Cushion shrubs, tussocks, and ground cover may survive to roughly 38 degrees.
- Every family is rejected past its hard slope limit regardless of patch noise.

This creates soft, irregular vegetation margins on ordinary slopes while
guaranteeing empty cliff-like terrain.

## Species Rules

### Trees

- **European beech:** moist lower-elevation forest interiors, mostly below
  290 m; dislikes exposed and steep sites.
- **Sycamore maple:** wetter valley and drainage-like habitat pockets, mostly
  below 300 m.
- **Birch/aspen:** clearing edges, disturbed openings, and the upper margin of
  mixed forest; tolerates moderate exposure and reaches higher than other
  deciduous forms.
- **Silver fir:** cool, moist lower and middle forest.
- **Norway spruce:** dominant connected middle-elevation groves and sheltered
  subalpine forest.
- **Mountain pine:** drier, exposed upper-treeline clusters and sparse subalpine
  fingers.

Tree candidate spacing is species-aware but shares a forest-mass field so
compatible species form mixed stands. A separate clearing field opens meadow
windows. Dense spruce interiors suppress most close ground detail; open birch
and forest edges retain grass, flowers, and berry shrubs.

### Shrubs and ground cover

- **Cushion shrub:** exposed upper meadow and alpine ground; broad dryness
  tolerance.
- **Broadleaf bush:** moist lower clearings and open woodland.
- **Woody scrub:** dry middle and upper slopes.
- **Berry shrub:** moist forest edges and lower clearings.
- **Creeping leafy vine:** connected sheltered and moist mats.
- **Flowering runner:** moist ground-cover patch edges and open meadow pockets.

Shrubs form belts and islands through a medium-scale patch field. Ground cover
uses a smaller patch wavelength and a tighter threshold so runners read as
connected mats rather than evenly spaced dots.

### Grasses and flowers

- **Cropped turf:** moist, grazed-looking valleys and open forest margins.
- **Fine meadow grass:** ordinary moist meadow and clearing matrix.
- **Tall seed grass:** deeper fertile meadow patches at lower elevations.
- **Dense tussock:** drier slopes and upper meadow.
- **Alpine daisy:** open, moderately moist lower-to-middle meadow colonies.
- **Bellflower:** cooler and wetter meadow or forest-edge colonies.
- **Clover-like flower:** lush lower meadow patches.

Each flower form receives its own colony field layered over meadow suitability.
Colonies may overlap at their edges but do not become uniform multicolor noise.

## Density and Performance Safeguards

The target is a lush showcase. Density comes from tight candidate spacing
inside suitable patches, not uniform random attempts across every surface.

Hard deterministic placement ceilings per 64 m sector are:

| Family | Maximum placements |
|---|---:|
| Trees | 36 |
| Shrubs | 96 |
| Ground cover | 72 |
| Flowers | 96 |
| Grass | 900 |

Normal counts are lower after altitude, slope, moisture, exposure, patch, and
species gates. The near-sector total remains comparable to or below the former
1,400 generic grass attempts, while each new asset represents a richer clump.

Candidate loops stop at the family ceiling. Invalid environmental values fail
closed for alpine vegetation. Placement sizes use bounded family-specific
transform scales and small grounding offsets; tree trunks and plant crowns are
never resized through asset parameters at runtime.

## Data Flow

1. The world evaluator serializes `StreamMountain.biomes()`, including the
   `alpine-lush` marker, into every sector request.
2. `WorldSector.static requires(p)` parses the marker and returns the fixed
   alpine vegetation catalog plus the existing non-vegetation scatter assets.
3. A sector bake emits terrain and determines its scatter tier.
4. The alpine placement path enumerates cross-sector candidates for the
   families enabled at that tier.
5. It samples altitude and slope, derives the world-space habitat signals, and
   applies hard environmental gates.
6. Suitability functions select a species/form or reject the candidate.
7. Continuous environmental dryness maps deterministically to a declared baked
   dryness state.
8. Candidate identity selects the declared seed variant, yaw, scale, and
   grounding offset.
9. `WorldSector` places the child instance at `heightAt(x, z)`.
10. Streaming publishes or evicts the completed sector through the unchanged
    native pipeline.

## Error Handling

- Missing or unknown vegetation profiles use the legacy placement path.
- Invalid profile data cannot enable partially declared alpine assets.
- Non-finite height, slope, or derived habitat values reject the candidate.
- Dryness is clamped to `[0, 1]` before baked-state selection.
- Species selection returns no placement when every suitability is zero.
- Every selected module/parameter pair comes from the exported fixed catalog.
- Family counts and transform scales have explicit minimum and maximum values.
- Hard altitude and slope exclusions run after suitability so noise can never
  override them.

## Testing

### Pure JavaScript ecology tests

Exercise the ecology module with fixed inputs and seeds:

- identical inputs return identical habitat and asset selections;
- different sector decompositions enumerate the same candidates;
- dryness state selection uses only declared values;
- higher moisture never directly makes the selected state drier;
- increasing altitude/exposure trends toward drier states;
- every family and all 19 forms are reachable;
- forest-edge species peak near the edge band;
- malformed values reject safely;
- hard tree, vegetation-ceiling, and slope gates cannot be overridden.

### Sector integration tests

Update the sector bake coverage to test both profiles:

- legacy worlds retain generic asset requirements and behavior;
- `alpine-lush` requires all expected alpine variants and no generic `Tree` or
  `Grass`;
- every emitted child key exists in the requirements map;
- identical sector inputs produce identical hashes;
- persistent families retain identical transforms across rung 0, 1, and 2;
- adjacent-sector enumeration has neither duplicate ownership nor seams;
- terrain and existing rock placement continue to bake successfully.

### Fast visual loop

Use the existing editor executable and relaunch `StreamMountain` after
JavaScript changes. Capture fixed views of:

1. lush valley and meadow;
2. mixed lower forest;
3. conifer-dominant upper forest and ragged treeline;
4. alpine shrubs and grass above treeline;
5. a steep wall proving vegetation exclusion;
6. close flowers, runners, shrubs, and mixed dryness.

Tune minimum distances, patch thresholds, family scales, grounding offsets, and
species curves from screenshots. Native builds are unnecessary unless testing
reveals an engine limitation outside the planned JavaScript integration.

## Acceptance Criteria

- `StreamMountain` uses no generic `Tree` or `Grass` instances.
- All 19 new vegetation forms appear in ecologically plausible habitat.
- Forests, clearings, flower colonies, shrub belts, and ground-cover mats are
  spatially coherent across sector boundaries.
- The world reads as intentionally lush without becoming uniformly filled.
- Trees fade into a ragged treeline and never appear above 455 m.
- Low vegetation never appears above 520 m.
- Family slope limits leave steep walls and cliffs empty.
- Dryness responds coherently to moisture, exposure, altitude, and slope without
  visible horizontal quantization bands.
- Streaming tier changes do not move persistent vegetation.
- No sector bake reports missing child variants or JavaScript errors.
- Final screenshots demonstrate valley, forest, treeline, alpine, cliff, and
  close-detail behavior.

## Out of Scope

- Native C++, renderer, material, or terrain changes.
- New terrain query verbs.
- Changes to the terrain surface classifier or chart virtual texturing.
- Vegetation wind animation, seasons, growth simulation, or physics.
- Imported meshes, textures, photogrammetry, or non-JavaScript assets.
- Replacing generic vegetation in worlds other than `StreamMountain`.
