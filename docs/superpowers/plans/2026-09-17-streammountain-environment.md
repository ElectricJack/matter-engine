# Streaming Mountains environment

Source goal: `/home/jkern/.codex/attachments/5101d9aa-3c24-406a-92e9-f90a24c0c670/pasted-text-1.txt`.
This expands the existing layered-surface plan; completed foundations are reused.
Visual development remains first, with correctness and measured cost preserved.

## Work and acceptance

1. **Terrain continuity and art.** Implement cross-sector POM from the existing
   two-sector/reference-mesh proposal, including unequal LODs, retained neighbor
   inputs, edits, eviction and feedback. Replace plate-like cliffs, refine quiet
   soil/moss/snow detail, and resolve raster/RT lighting differences. Require
   fixed and moving-camera native raster/RT evidence at actual sector borders.
2. **Rocks and boulder fields.** Reusable JS geometry families with fractures,
   worn edges, varied silhouettes, efficient instancing and suitable LODs.
   Correlate material microrelief with geometry and bake it into reusable VT
   pages with POM. Terrain-conditioned placement supplies erratics, partially
   buried boulders, talus clusters and scree. Verify collision geometry, contact,
   stable ownership across tiles and coherent distant appearance.
3. **Vegetation.** Extend current evergreen assemblies with shrubs, grass and
   ground cover, varied habitat-driven clusters and clearings. Shared placement
   constraints keep trees and ground cover out of rocks and infrastructure.
   Preserve voxel/instance reuse and stable placements across streaming tiers.
4. **Infrastructure.** Define a shared JS route/site layout before independent
   generators place assets. Roads use terrain grading and blended shoulders;
   simple houses reuse brick walls with foundations, roofs, openings and
   weathering; tunnels carve real traversable density openings and have matching
   interiors/portals; poles and sagging wires follow routes with clearance.
   The same layout drives terrain, surfaces, collision and ecological exclusion.
   Native acceptance must cross sector/LOD boundaries and inspect tunnel interiors.
5. **Layers and contacts.** Complete sparse overrides, spatial filtering, local
   invalidation and world overlays on shared geometry-baked bases. Demonstrate
   dirt/dampness/moss across rocks, terrain, houses, roads and portals, with
   leakage controls and remaining wall seams resolved. Shared bases remain shared.
6. **Cost and final acceptance.** Measure startup, generation, streaming, memory,
   frame and edit latency; address remaining bottlenecks without reducing quality.
   DSP/SDF and analytic geometry/placement lead; optional physics is bounded.
   Obtain visual approval for the integrated scene through close/distant/grazing,
   moving-camera and native raster/RT comparisons. All original acceptance
   requirements remain; voxel silhouettes/shader integration are deferred.

## Terrain continuity checkpoint: independent-owner reference

The native `vt-sector-seam` acceptance fixture now compares a connected
two-chart owner with two independent owners using translated local coordinates
and the identical world recipe. It also covers unequal edge tessellation.
Constant/sloped height have analytic depth and channel oracles in raster and
secondary RT; GPU readback proves distinct slots and fully resident mip zero.
The connected reference passes, while 32 of 80 split samples lose their
displacement. This gate intentionally remains red until production cross-owner
sampling works. Existing connected-chart/fold/gap tests still pass.

[Measurements, source links and next implementation constraints](../../agent/evidence/2026-09-17-sector-pom/README.md).
This establishes a reproducible failure in owner transitions and filtering;
actual streamed terrain, LOD switching, edits and eviction remain separate
acceptance requirements. Next implement explicit domain/link publication,
retained neighbor inputs, frame conversion and feedback in the production
walker. Preserve physical depth and genuine-gap rejection throughout.

The first production dependency is implemented and native-tested: preparation
extracts real open edges from the retained GPU triangle order, the compositor
reuses those boundaries with geometry, and residency exposes current tail
sources whose leases survive edits/release without authorizing stale reuse.
The bounded pair compiler handles coarse/fine edge intervals in rigid world
frames and rejects ambiguous, disconnected or unauthorized pairs. Native slot
recycling also rejects the old owner's leases. Shader consumption, material-bank
retention for links and terrain-domain publication remain the next stage; this
checkpoint does not change the visible sector seam or complete terrain work.

The subsequent GPU integration (`sector-links-v1`) builds natively and now
passes equal-mesh owner crossing in raster and secondary RT, including depth,
color, roughness and normal registration. The strict gate still fails unequal
tessellation (12 assertions; zero Vulkan validation errors). A diagnostic repeat
confirms the four coarse/fine edge intervals are published in both directions.
Investigate shader walk/filter routing near fine-edge endpoints next, retaining
the unchanged physical relief and acceptance tolerances. No automatic terrain
neighbor/LOD assignment has been installed, and the broader streaming/edits/
pressure/performance matrix is still required. The passing builds do not imply
those remaining regression suites passed: the runner stopped at the seam gate.

The endpoint correction now passes the unchanged `sector-links-v3` native gate:
equal and unequal meshes preserve all 80 split-owner samples in raster/RT,
with maximum position error 0.01689 mm. The compositor, folds/genuine gaps,
POM work/source lifetime and feedback regressions pass with zero Vulkan errors.
The `sector-links-v4` incremental connection cache also passes native local
edit/removal/slot-reuse checks: one edited pair uploads two owner tables while
an independent join retains its addresses. Six focused suites and the editor
build pass. Actual
terrain publication must connect runtime weld strips as well as sector meshes;
the coarse and fine voxel boundaries themselves do not always coincide.
`build_weld_part` currently bypasses VT entirely, so give those strips charts,
world-space recipe/field inputs and demand-driven registration before linking
their actual edges. Reuse the drawn-sector face index for bounded neighbor
updates. The
full streaming/LOD/edit/pressure matrix and visual acceptance remain open.

## First implementation: rock geometry and placement

`MountainRock` uses twelve JS-authored clipped convex meshes: fractured blocks,
bedded slabs and weathered boulders. Secondary planes wear actual fracture edges.
The mesh is emitted directly through generic attributed triangles, without
voxel meshing, raycasts, retopology or physics. Instance size/pose is independent
of the prototype identity. Closed-mesh and native bake checks are required.

`planMountainRocks` uses stable world grids and finite-support cluster anchors.
Terrain height/slope gates placement; local height samples orient and partially
bury instances. A halo supplies neighboring boulder footprints for forest
exclusion. The `available(x,z,radius)` predicate will consume the shared road,
site and portal clearance fields. Half-open ownership and exact subdivision
tests guard tile-size changes. Small scree is a separate tier and does not
change forest placement.

This first step does not complete rock appearance: the new geometry initially
uses the existing rock material. Dedicated reusable VT/POM microdetail and
world contact overlays are still required. Terrain POM seams, additional
vegetation, infrastructure and full-scene performance are also unfinished.

## Reusable procedural materials on ordinary parts

Add a generic `static surface(p)` declaration returning
`{version:1, material, recipe:s=>({...})}`. The recipe uses the existing surface
language in part-local metres. It is evaluated independently of `build()` on
both cold and cached geometry paths. It supplies color, roughness, metallic,
AO and bounded physical height; the existing VT compositor derives normals and
POM from that height. No finite solid, source image or physics job is required.

The first contract is one complete local source on a standalone part. Reject
world/field inputs, receiver-dependent height, ambiguous simultaneous
`finiteSurface` declarations, malformed metadata, stale generations and
unbounded evaluation. Existing finite/periodic sources retain their contracts.
Publish one immutable prepared recipe under the resolved part identity, bind it
on demand and eager VT registration, and reuse that binding during export.
Geometry edits and recipe edits retain the existing content-hash invalidation.

Rock instances share prototype pages. Instance scale also scales authored
surface features, so close inspection of small and landmark rocks must verify
effective texel density and relief size before acceptance; prototype-scale
materials alone do not prove consistent world-space microdetail. World contact
overlays remain separate work and must preserve this shared base.

Validation must exercise cold/warm provider publication without projection or
material-image baking, rejected recipes, atomic failure, ordinary non-planar
receivers, and actual native raster/RT POM controls. Inspect raw material
channels as well as lighting before judging the rock appearance.

The first material gallery exposed a parent-flattening integration gap: all
twelve rocks were combined under one owner and their individual recipes were
lost. Place material-bearing children with
`{instanced:true, inlineBelowPx:0}` to preserve their local geometry/material
identity through every distance. Explicit zero is a permanent reference;
omitting the threshold retains the existing 64-pixel cutover. Permanent
references must stay outside other children's shared coarse cutover. Initially
this is supported on the default parent LOD ladder; authored/budget parent
ladders must reject the unsupported combination instead of dropping material
or geometry. Child prototypes may retain their own independent LOD ladders.

### Native checkpoint

Generic local recipes and permanent placement now work in the rock gallery.
Native metadata/provider checks, the full flattening suite and analytical
raster/RT publication checks pass. A rejected gallery exposed the original
parent merge; the corrected repeat retains thirteen owners and shows all
twelve materials in 28 audited images. The 256 t/m proof override does not
establish production density (currently 16 t/m for props), and instance scale
still scales microdetail. Next address authored part density and rock art,
then review the full Streaming Mountains scene with the actual placement path.
Terrain seams and all other environment/overlay/performance requirements remain.

[Evidence and explicit limitations](../../agent/evidence/2026-09-17-mountain-rocks/README.md).

### Authored material resolution

Ordinary parts can declare `static vtTexelsPerMeter = 256` or
`static vtTexelsPerMeter(p) { return p.density; }`. The value is a finite number
from 1 through 2048 in the material owner's **local metres**. Evaluate it with
merged parameters before construction/build, under the existing evaluation
budget. Reject invalid declarations before publishing an artifact. Omitting it
retains the renderer's 16 t/m default. The diagnostic environment override
remains available; an invalid override falls back to the authored setting.

Persist the setting in optional RNDR v3 metadata; unchanged policies retain
their exact v2 bytes. Legacy v1/v2 readers in the current engine use the default.
All flat and compositional chart builders, retained bake staging and memory-only
preparation consume the same owner setting. Terrain continues to use its world
setting. A flat is one root-local material owner: merged geometry uses the root's
setting, while permanently instanced children retain their own. This is not a
per-triangle density override for geometry that has been merged into another part.

The first production request is 256 local t/m for `MountainRock`. Gallery
validation must run without the diagnostic override to prove this path. Atlas
packing limits still apply, and increased virtual resolution is not a promise
that every page stays physically resident. Monitor actual density, resident
pages, startup and frame times in Streaming Mountains.

### Physical rock size classes

Enlarging a unit prototype also enlarges its grain and physical height while
lowering its world texel density. Use four physical reference sizes, 0.5, 2, 8
and 32 metres, for each of the twelve existing silhouettes. This makes 48
reusable geometry/material owners, shared by all placements. The geometry is
authored at its reference size; grain, pits, weathering and fissure frequencies
and relief depths stay in local physical metres. Only broad mineral variation
follows the overall silhouette. No physics settling or source-image bake is
needed, and no material is generated per individual rock.

Choose the reference size from the existing deterministic placement's requested
size, then apply the residual scale `requested/reference`. It stays between
0.5 and 2 over the supported size range. Preserve shape/seed selection, world
geometry, rotation, burial, clearance and placement identity exactly. This
bounds, but does not eliminate, instance scaling of the microdetail. Contact
overlays are still required independently.

Request 192 texels per physical local metre for these owners. Validate actual
chart density rather than trusting the request: the initial native trial keeps
192 for the first three classes, but the 32-metre class falls to 96 under the
current atlas-halving policy and fails the 128 t/m minimum check. Resolve the
excessive density drop within the existing virtual-atlas limit; do not enlarge
the physical page pool or hide the failure with an environment override.

The chart builder now keeps an unchanged requested-density fast path. For an
oversized request it halves to a fitting density, then makes six bounded
intermediate packing attempts between that fit and the last failure. Restore
the final verified packing before writing UVs, including any material-grid
origins. Keep the 16K virtual-atlas cap, page geometry and physical pool fixed.
Shelf packing and page rounding need not be monotonic, so this recovers a
verified higher resolution without claiming a globally optimal pack. Cover
both single-chart extent limits and multiple-chart packing limits, aligned and
compact origins, deterministic repeat builds, borders, overlap and UV mapping.

`RockScaleProof` places all four physical sizes without additional instance
scaling. Its near and grazing cameras sit at fixed physical distances from a
real surface face, so enlargement cannot disguise oversized grain. Review
lit, raw albedo, normals, actual density and POM-on/off images in raster and
native RT. Follow with the real streamed placement path and compare source
count, startup, resident pages, indirection memory and frame times. Equal
material height at matching physical coordinates, a passing unit test, or a
valid renderer capture alone is insufficient visual acceptance.

The first scale capture is technically valid (63 images, 18 native RT) but is
not an art acceptance. Fixed-distance views expose soft close-up detail and
very small raster POM-on/off changes, alongside the previously identified dark
blue lighting. Inspect filtering, relief scale and lighting after correcting
the density clamp; do not treat scale consistency as completion of rock art.
The selected inspection face points away from the comparison sun (normal dot
direction-to-sun = -0.612). Add a separate directly illuminated control at sun
azimuth -120/elevation 25 degrees (dot = 0.833) before attributing its blue shadow
to a lighting bug or changing the material's albedo. Preserve the original
lighting for matched density comparisons.

### Resolved rock relief and geometry levels

Author the surface for the retained 157.5–192 t/m resolution: centimetre-scale
pits and erosion should survive the actual 5–6.5 mm texel footprint, while
sub-centimetre grain filters separately. The current relief envelope is
[-0.018,0] m, with a stable coarse datum at -0.004 m. Validate material bounds,
scale consistency and at least 3 mm of remaining height variation at a 6.5 mm
footprint; then inspect actual POM-on/off images in both renderers. Stronger
height alone is not proof of more realistic art.

The v12 scene trace found the three procedural budget levels rebuilt identical
analytic geometry. Replace that unused budget ladder with two `LOD.decimate`
generators, using radius-relative errors (divisors 128 and 32) and the existing
derived switch distances. Keep the full-resolution mesh and deterministic
placement unchanged. Read the actual flat artifact in native tests: its three
levels must share one source bake, reduce geometry and retain useful charts.
Production currently charts each level independently; the engine-wide
`MATTER_VT_UNIFY` experiment remains off. Measure the resulting residency and
motion before accepting the LOD transition behavior, and keep cross-LOD texture
reuse as unfinished work rather than assuming shared recipes imply shared pages.

### Streaming installation must prepare materials

The v10 full-scene capture exposed an integration gap hidden by the gallery:
`install_world` recursively bakes streamed assets through `HostBaker`, bypassing
the provider's ordinary graph and its material preparation. Their geometry and
density metadata were present, but raw albedo showed the plain fallback.

Use `LocalProvider::ensure_part_surface` in that dependency walk after geometry
is available, including disk-cache hits. The provider owns prepared sources;
its existing GPU queue publishes them before renderer/store reset and streaming
admission. Report a material preparation failure as an installation error.
World-coordinate receiver declarations must reject both finite and direct local
part surfaces, avoiding two conflicting material owners. Native coverage must
exercise an asset absent from the ordinary bake plan on cold and warm installs,
and the actual full-scene repeat must show the intended material in raw channels.

The v11 repeat verifies twelve shared source publications and the expected
mineral variation in streamed rocks; a terrain control is unchanged. Native
provider checks and 37 audited full-scene captures pass. It remains an art
iteration: large-instance feature scale, ground contacts, terrain seams, lighting
and the rest of the environment are unfinished. Frame-time results are mixed,
including a 10.1% grazing G-buffer increase versus v9; investigate with matched
POM timing controls while continuing visual work. See the evidence linked above.

## Shared route and site authoring checkpoint

`shared-lib/mountain_routes.js` now compiles world-space centreline points into
immutable road/shoulder ribbons, tunnel portal frames and oriented house sites.
Crossfall, station distances and shared bend vertices are computed once. A
spatial index supplies local grade/clearance queries; a distinct half-open query
assigns each section to one streaming owner. Tile queries never resample a route
or change its elevation. The existing rock planner accepts its clearance
predicate without moving unaffected instances.

Node checks pass for analytic grade/crossfall, smooth shoulder/end blending,
mesh-to-grade agreement, curved and right-angle joins, world translation,
tile subdivision, portal records and rock exclusion. The module is not yet
imported by the live scene. Native terrain, collision, rendering and visual
acceptance remain required. [Evidence and limitations](../../agent/evidence/2026-09-17-mountain-routes/README.md).

### Required integration

1. Fit a finite route through the actual mountain using the unmodified terrain
   height/gradient. Retain explicit world-space route elevations and validate
   grade, curvature, cut/fill and portal cover. All later consumers use that
   canonical layout. Do not independently fit each streaming tile or house.
2. Install spatially indexed grading data alongside the existing field. The
   field parser has a 96-operation limit: one scalar-expression expansion per
   road segment is unsuitable. Reuse the immutable overlay integration where
   possible; preserve the field-only fast path outside modified bounds. The
   current `HeightOverlay` is heightfield-only, so real tunnel subtraction
   requires explicit density support and appropriate meshing bounds, not just
   an implementation of `height_at`.
3. Route `height_at`, direct and column-cached density, terrain meshing, collision,
   habitat probes and material field inputs through the same effective terrain.
   Version and hash the modifier data with the world field. Native comparisons
   must exercise both density APIs, and edits must retire stale geometry and VT
   inputs together. Preserve river behavior when extending shared interfaces.
4. Apply road material/shoulder masks in the same world frame and use POM for
   gravel and small road relief. Prefer the graded terrain surface for ordinary
   roads; separate tunnel floor/interior meshes consume the same stations.
   Sparse wear/dampness layers must preserve shared material bases. Avoid two
   nearly coincident road/terrain surfaces fighting for depth.
5. Carve actual tunnel volumes with floors, usable clear width/height and portal
   transitions, then generate matching interiors. Height-only grading intentionally
   leaves deep overburden intact. Preserve vegetation above a buried tunnel but
   exclude it from entrances. Test traversability and absence of portal caps at
   actual voxel sizes and across sector/LOD boundaries.
6. Use site frames for foundation grading and brick-dimensioned house layouts,
   including roof, door/window openings and weathering. Generate poles and
   sagging cables from global route stations; probe span clearance before tile
   ownership queries. Neither endpoint height nor sag may depend on the currently
   loaded sector. Explicit junctions require compatible elevations and joined
   geometry; the current compiler rejects ambiguous intersecting ribbons.
7. Enable the complete route/site layout in Streaming Mountains after native
   checks. Measure startup, bake, query, streaming and frame costs, then review
   close/far/moving cameras and raster/RT. This authoring checkpoint does not
   complete any of those scene-level requirements or replace the pending terrain
   POM seam validation.
