# ConiferLab

Select **ConiferLab** in the editor. The left tree is Scots pine; the right is
silver fir. Six pine bough samples and life-size needle/cone samples sit in front.
The saved daylight setup puts the sun at 48 degrees.

**RedwoodGrove** uses the same generator for a 62 m coast redwood and a
three-stem clump. Its stems grow from one isosurface root crown; see
[`RedwoodGrove`](../RedwoodGrove/README.md) for the clump controls.

The generator is [`shared-lib/conifer.js`](../../../shared-lib/conifer.js).
`ConiferTree` is the object wrapper. Its six bough skeletons are reused by size
and variant: two 0.70 m, two 1.45 m, and two 2.65 m sources. Each has a curved
primary limb, secondary branches, tertiary twigs, and retained needle sprays.
Placement rotates and scales the woody skeleton; needle and cone dimensions
remain independent of that scale. Four needle-spray sources per species and
two cone sources are shared across the crown. Each foliage source contains five
overlapping shoots (one terminal and four lateral), baked together to retain
branch volume at distance. Standalone needle samples show a single shoot.
Fir and redwood needles use broad, nearly parallel blade edges with a short
tip taper and a separately shaded lower face. Their current-year shoot axes
are 1.3 mm across at the base; pine axes are 2.4 mm. Needle length and maximum
width remain the species dimensions below. The palette is specified in linear
colour, with a subdued green to avoid the former pale cyan canopy.

## Authoring

Use an expanded root, as in `ConiferLab.js`:

```js
{
  module: 'ConiferTree',
  params: {
    species: 0, seed: 42, branchSeed: 17,
    height: 14, dbh: 0.32, age: 32,
    crownRadius: 3.1, crownRatio: 0.82,
    whorlCount: 26, branchesPerWhorl: 6,
    branchLoss: 0.06, fullness: 1.25,
    needleDensity: 1, coneDensity: 0.35,
    asymmetry: 0.18, lean: 0.025, droop: 0.16, dryness: 0.08,
    barkMaterial, branchMaterial, needleMaterial, coneMaterial,
  },
  transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
  expand: true,
}
```

The material handles come from the world's `defineMaterial` calls. The lab
includes rough bark, restrained needle scattering, and woody cone materials;
bark relief uses the native surface-detail tileset bake.

| Parameter | Meaning |
|---|---|
| `species` | 0 Scots pine; 1 silver fir; 2 coast redwood |
| `seed` | Whorl placement, crown asymmetry, missing branches and cone placement |
| `branchSeed` | The six shared bough shapes; keep constant to reuse the kit |
| `height`, `dbh` | Metres; DBH is trunk diameter at 1.3 m |
| `age` | Years; limits retained whorls and reproductive maturity |
| `crownRadius`, `crownRatio` | Crown reach in metres and fraction of trunk with living branches |
| `whorlCount`, `branchesPerWhorl` | Retained annual tiers and their primary branch count |
| `branchLoss` | Probability of primary branch loss, strongest low in the crown |
| `fullness` | 0.5–1.8; changes twig forks and retained sprays, preserving needle dimensions |
| `needleDensity` | 0.2–1.6; needles per unit shoot length |
| `coneDensity` | 0–1; probability for eligible mature branches |
| `asymmetry`, `lean`, `droop` | Crown irregularity, trunk lean and branch inclination |
| `dryness` | 0–1; green-to-brown needle palette |
| `barkMaterial`, `branchMaterial` | Separate trunk and bough finishes; branch defaults to trunk if omitted |

Height, age, crown ratio and DBH are independent authoring controls, not a
calibrated growth simulator. `treePlan(params).counts` reports the actual
generated primary/secondary/tertiary branches, sprays, needles and cones.
Branch counts are a reproducible structural estimate for the selected age and
crown, not a claim that every tree of that species has the same count.

The object wrappers are shared in `projects/world_demo/objects/` because both
ConiferLab and RedwoodGrove use them. The algorithms remain in the shared library.

## Representations

| Source | Close representation | Coarser representations |
|---|---|---|
| Trunk / six wood boughs | Native SDF isosurface, joined tapered brushes; trunk bark ridges and root flare | Authored 12-, 7-, then 4-sided longitudinal triangle strips at 6 / 22 / 65 m (unit-scale source) |
| Needle spray | Individual tapered needle surfaces; pine fascicles in pairs, fir comb arrangement and pale underside | 2-triangle native view atlas at 4.5 m; 48 views storing coverage, normals, depth and tint |
| Cone | Individual spiral scale surfaces and central axis | 1.5 mm error mesh at 3 m; view atlas at 12 m |
| Bark relief | Coloured cellular plates baked to albedo, height, normals and ORM | Existing surface-detail sampling and mip policy |

`expand: true` publishes the reusable wood and spray leaves directly. It avoids
the segmented assembly path retaining a second, merged copy of the foliage.
The single engine distance rule selects each leaf's representation. Wood never
turns into an impostor. Near sources are authored in metres; instance scaling
adjusts the wood's switch distances through that existing distance rule.

Foliage atlases now upload a mip chain with each view filtered separately.
Normal/depth/tint filtering uses coverage weights; the alpha cutoff area is
preserved as closely as the coarser pixel grid allows. Sampling clamps to the
chosen view at the selected mip, preventing neighboring views bleeding into
the foliage. An occupied view keeps at least one covered texel; equal-coverage
ties are spread spatially so a thin comb cannot disappear in one mip step.
This limits filtering dropouts; native distant foliage still needs further
antialiasing and coverage work. The chain stops at four
texels per view and adds about one third to the atlas GPU memory allocation.

The source mesher now subdivides its spatial cells below the former 15.9 mm
sampling floor, keeping each scratch grid at at most 64 cubed. Branch requests
are 4 mm (achieved dyadic lattice about 3.97 mm); trunks request 18 mm, with
finer bark relief in textures. The minimum spatial cell remains 1/64 m, so
arbitrarily tiny sampling requests are not unbounded. Large redwood trunks
increase the geometry sampling interval with diameter and surface area; their
fine bark detail stays in the texture instead of exceeding the BVH limit.

## Bark generation

[`conifer_bark.js`](../../../shared-lib/conifer_bark.js) generates periodic, warped
cellular plates. The distance between the nearest two cells produces branching
fissures; smooth bevels leave broad plate tops. Each plate has seeded colour,
weathering and small splits. These are relief surfaces used only during the
native texture bake, never extra bark-plate instances on the tree. A dedicated
`ConiferBarkRelief` child supplies coloured geometry to each tileset; emitting
ordinary geometry directly in a Tileset build does not feed the native bake.
`dropChild(module, params, { physics: false })` stamps it at its authored pose,
so thin relief cannot fall through the settling surface. The default drop
behavior still uses physics.

| Profile | Tile size | Relief range setting | Typical plate pitch across / along |
|---|---:|---:|---:|
| Trunk | 0.64 m | 16 mm | 80 / 160 mm |
| Branch | 0.256 m | 2.5 mm | 28 / 64 mm |
| Redwood | 1.28 m | 45 mm | 142 / 640 mm |

Adjust `BARK_PROFILES` for chunk size, fissure depth, seed and resolution. Each
profile bakes a 1024-pixel tile with native height, normals, albedo, roughness
and occlusion. Trunk grain follows the vertical axis; branch grain follows the
source bough's length. Triplanar sampling avoids stretched UVs at root flares.
Fine off-axis twigs share the bough's texture frame, so their grain alignment is
an approximation. The Vulkan bake now preserves authored triangle tint, which
is needed for dark crevices and differently coloured plates in the same material.

## Botanical references

- [Oregon State: Scots pine](https://landscapeplants.oregonstate.edu/plants/pinus-sylvestris): paired needles, 2.5–8 cm; cones 2.5–7 cm; crown habit and bark.
- [Oregon State: silver fir](https://landscapeplants.oregonstate.edu/plants/abies-alba): 15–30 mm needles, 1.5–2 mm width, two pale stomatal bands and branch habit.
- [Gymnosperm Database: Abies](https://www.conifers.org/pi/Abies.php): annual whorls and erect cones.
- [Gymnosperm Database: silver fir](https://www.conifers.org/pi/Abies_alba.php): 10–16 cm cones, 3–5 cm diameter.

The canonical pine needle is 52 mm and its cone 45 mm; fir uses 24 mm needles
and 130 mm cones. Seeded variation stays within the cited needle bounds.
Fir cones point upward on the upper mature crown; pine cones hang downward.

## Validation

```bash
node --experimental-vm-modules projects/world_demo/tests/conifer_tests.mjs
./tools/build-windows-from-wsl.sh RelWithDebInfo conifer_lod_provider_tests
./MatterEditor/build/cmake/windows-msvc/relwithdebinfo/conifer_lod_provider_tests.exe
./tools/build-windows-from-wsl.sh RelWithDebInfo matter_editor
```

The Node suite checks deterministic anatomy, physical dimensions, six-prototype
reuse, fullness, dependency declarations, transform/DSL balance and decreasing
strip complexity. The native test exercises installed/deferred/cached LOD
publication, real needle-atlas generation, and a millimetre twig through the
actual SDF mesher with preserved bark material identity, and fractional-metre
texture allocation (including invalid and overflowing extents).

`qa.timeline` contains the whole-tree, branch, needle, cone, bark and distance
views for the engine's FIFO driver. Create `C:/tmp/conifer-qa` first. During a
live review omit its final `quit`. Inspect the captures as well as the native
test results; passing authoring tests alone is not visual acceptance.

The broader async-bake suite currently has a stale assertion expecting exactly
three root trace spans; the provider also emits `resolve-cache.load`. Its other
checks pass when run from `MatterEngine3/tests` (its fixture paths depend on
that working directory). The focused conifer provider test passes.
