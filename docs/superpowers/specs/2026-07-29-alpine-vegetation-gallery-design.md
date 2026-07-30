# Alpine Vegetation Gallery Design

**Date:** 2026-07-29
**Status:** Approved for specification review
**Scope:** JavaScript DSL assets and a standalone gallery world only

## Goal

Create a reusable collection of realistic procedural vegetation assets suited
to Swiss mountainous terrain. The collection covers grasses, flowering plants,
shrubs, ground vines, conifers, and deciduous trees. Every asset exposes a
continuous `dryness` parameter so future landscape scatter can choose
appropriate-looking vegetation for local environmental conditions.

This cycle also adds a standalone gallery world for close visual comparison and
rapid screenshot-driven tuning. It does not integrate the assets with
`StreamMountain` or change streaming placement.

## Constraints

- All authored geometry and gallery composition use the existing JavaScript DSL.
- No C++, engine, renderer, terrain, or `StreamMountain` changes are planned.
- Prefer the existing editor executable and JavaScript rebakes over native
  rebuilds.
- Assets are deterministic for identical parameters.
- Gallery variants are declared up front so child hashes and cache behavior are
  stable.
- Geometry must remain detailed enough for close gallery views without creating
  impractical bake times or triangle counts.

## Architecture

### Shared vegetation helpers

Add a focused helper module under `projects/world_demo/shared-lib/` for:

- clamping and normalizing vegetation parameters;
- deterministic palettes derived from dryness and seeded variation;
- tapered grass blades and stems;
- leaf and petal meshes;
- flower centers and dried seed heads;
- branch-frame and crown-placement helpers;
- small, deterministic natural variation in angle, scale, lean, and color.

The helper module emits geometry only through methods supplied by a `Part`
instance. It does not own world placement or depend on a particular gallery.

### Asset families

Create separate JavaScript `Part` classes for these families:

| Family | Forms |
|---|---|
| Grass | cropped turf, fine meadow grass, tall seed grass, dense tussock |
| Flower | alpine daisy, bellflower, clustered clover-like bloom |
| Shrub | cushion shrub, broadleaf bush, woody scrub, berry-bearing shrub |
| Ground cover | creeping leafy vine, flowering runner |
| Conifer | mountain pine, Norway-spruce form, silver-fir form |
| Deciduous tree | European-beech form, pale-trunk birch/aspen form, sycamore-maple form |

Each family exposes:

```js
static params = {
  seed: 0,
  dryness: 0.35,
  size: 1.0,
  form: 0,
};
```

`form` selects one of the named forms in that family. `seed` changes natural
variation without changing the form's identity. `size` provides bounded
proportional scaling for future reuse. `dryness` is continuous from `0.0` to
`1.0` and is clamped by the asset.

Direct triangle geometry is used for blades, leaves, petals, and needles.
Tapered DSL lines or tubes form stems, branches, and trunks. Voxel sessions are
avoided unless screenshot evidence shows that a substantial woody form cannot
read convincingly with the faster direct-geometry path.

### Gallery composition

Add a `VegetationGallery` part that declares all displayed family/form/dryness
combinations in `static requires`. It lays out three separated zones:

1. small plants: grasses and flowers;
2. shrubs and ground cover;
3. conifers and deciduous trees.

Within each row, the same form and seed appear at dryness values `0.0`, `0.35`,
`0.7`, and `1.0`. This isolates dryness as the visible difference between
columns. Spacing is family-specific: compact for herbs, wider for shrubs, and
wide enough for mature tree crowns.

Add a standalone `VegetationGallery` world with fixed lighting and camera
defaults. The gallery uses a simple neutral ground and unobtrusive background
so foliage color, silhouettes, and grounding remain readable.

## Dryness Model

Dryness affects several traits together rather than acting as a color-only
filter:

| Dryness | Appearance |
|---:|---|
| `0.0` | Lush saturated foliage, fuller blade and leaf counts, upright growth, strongest flowering |
| `0.35` | Healthy ordinary alpine vegetation and the default authoring state |
| `0.7` | Yellowed tips, reduced foliage, more lean or curl, fewer flowers, more exposed wood |
| `1.0` | Straw and brown tones, sparse or dead leaves, drooping blades, bare branch tips, dried flower heads |

The transition is continuous. Family-specific response curves may delay or
accelerate individual effects—for example, conifers retain more foliage than
flowers at the same dryness—but the endpoints and direction remain consistent.

Seeded micro-variation may alter individual hues and shapes slightly. It must
not obscure the left-to-right dryness progression in the gallery.

## Visual Form Guidelines

### Grasses

Grass forms use many tapered, bent strips with varied azimuth and height.
Cropped turf stays low and dense; meadow grass is fine and irregular; seed
grass carries narrow stems and seed heads; tussock grass grows from a dense
base with an outward arch. Dryness increases straw-colored blades, curl,
breakage, and droop while reducing fresh green density.

### Flowers

Flowers combine thin stems, small leaves, and species-readable bloom
silhouettes. Daisy forms use radial petals around a contrasting center;
bellflowers use hanging flared blooms; clover-like plants use clustered small
heads. Dryness reduces active blooms and replaces some with dried heads rather
than deleting all vertical structure.

### Shrubs

Shrubs use a visible woody framework plus clustered foliage. Cushion shrubs
remain low and compact; broadleaf bushes form rounded irregular crowns; woody
scrub is sparse and angular; berry-bearing shrubs add restrained colored fruit.
Dryness exposes the framework, creates uneven dieback, and shifts remaining
leaves without uniformly scaling the whole crown.

### Ground cover and vines

Ground covers follow several branching runners close to the ground, with
alternating leaves and occasional raised tips. The flowering form adds small,
sparse blooms. Dryness shortens live runner sections, reduces leaves, and
introduces dry brown segments while preserving a legible creeping silhouette.

### Trees

Conifers have distinct crown profiles: irregular low mountain pine, tapered
spruce, and layered narrow fir. Needle masses are represented by small clustered
meshes rather than individual needles.

Deciduous forms use species-distinct trunks, branch habits, leaf size, and crown
profiles: dense rounded beech, light open pale-trunk birch/aspen, and broad
irregular sycamore maple. Dryness produces patchy canopy loss and dead tips; it
does not shrink the trunk or apply identical thinning everywhere.

## Data Flow

1. The world requests the `VegetationGallery` root.
2. `VegetationGallery.static requires` declares every displayed asset variant.
3. Each asset receives `seed`, `form`, `size`, and one of the four dryness
   values.
4. The asset clamps parameters, creates a deterministic RNG, and derives its
   family-specific dryness state.
5. Shared helpers emit seeded direct geometry through the asset's `Part` DSL.
6. The gallery instances the baked variants at fixed comparison positions.
7. The editor renders each gallery zone from fixed cameras for screenshot
   comparison.

## Validation and Iteration

### Fast visual loop

Use the existing editor executable when available:

1. Launch `VegetationGallery` with live edit and command control enabled.
2. Wait for the initial asset variants to bake and publish.
3. Capture fixed-camera screenshots of the small-plant, shrub/ground-cover, and
   tree zones.
4. Inspect silhouettes, grounding, colors, branching proportions, foliage
   density, and visible repetition.
5. Tune JavaScript constants, reload, and recapture only the affected views.

The target is approximately three visual tuning passes. Additional passes are
driven by specific screenshot evidence. The viewer process must be
self-terminating or explicitly stopped after the final capture.

### Mechanical checks

- Evaluate and bake the new world without JavaScript errors.
- Verify identical parameters produce identical geometry and transforms.
- Verify out-of-range dryness values clamp to `0.0` or `1.0`.
- Verify every gallery child is present in the fixed requirements list.
- Confirm no native source or build-system change is needed.

### Visual acceptance

Final screenshots must show:

- clearly distinguishable forms within every family;
- believable Swiss/alpine vegetation silhouettes;
- a smooth, readable progression across the four dryness columns;
- nonuniform, organic foliage loss rather than simple whole-object scaling;
- grounded stems, trunks, and runners with no obvious floating geometry;
- no disconnected foliage, severe intersections, or prominent flat-card
  artifacts from the intended viewing distances;
- sufficient close-range detail without excessive repeated geometry.

## Error Handling

- Missing parameters use the declared defaults.
- `dryness` is clamped to `[0, 1]`.
- `size` is clamped to a small positive family-appropriate range so invalid
  input cannot invert or collapse geometry.
- Unknown `form` values fall back to the first form in that family.
- All count calculations enforce a minimum required to preserve the form's
  silhouette, including at full dryness.
- Invalid generated values are avoided at the helper boundary by bounded
  denominators and normalized-vector fallbacks.

## Out of Scope

- `StreamMountain` integration or changes to `WorldSector`.
- World-space dryness, moisture, elevation, slope, or biome fields.
- Automated vegetation scattering.
- Wind animation, seasonal animation, growth simulation, or physics.
- New native DSL verbs, renderer features, materials, or textures.
- Photogrammetry, imported meshes, or non-JavaScript asset sources.
