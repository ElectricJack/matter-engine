# Grid castle kit and local lighting

Design and implementation specification, 2026-09-11. Owner: Astra.
Status: implementation specification; task execution and acceptance tracked in agent queue.

## Outcome

Build a reusable JavaScript architectural system, then demonstrate three furnished,
traversable castles with different plans and silhouettes. Preserve Kreuzenstein as
the original photographic study. New scenes are built from authored floor plans,
not hard-coded exterior facades. A metre is the placement unit, never the detail
resolution. Stones, wood joinery, tracery, gold and glass should reward interior
inspection under real local lights.

## Repository findings and decisions

* There is no exposed architectural scaffold/socket API in this checkout. The
  reusable scaffold will be a pure-JS plan/edge graph, emitting existing Part
  composition. Engine sector grids and terrain lattices have different ownership
  and should not become floor-plan storage.
* Existing `World.roots`, flat scalar part parameters, `requires`/`placeChild`,
  `expand:true`, `defineMaterial`, voxel CSG and modifier blocks are sufficient
  for architecture. Use flat `shared-lib/castle_*.js` imports supported by QuickJS.
* Kreuzenstein demonstrates voxel stones and expanded assemblies. Its helper is
  photographic, not the new kit's topology. Reuse the approach and improve the
  primitives without changing that scene. Voxel `tint` is not preserved; use
  material handles. `endModifier([{simplify:...}])` is the supported simplifier.
* `WorldLight` already expresses point/spot data, and the loader accepts legacy
  point arrays or `lights.spots`. `WorldLights.spots` carries resolved values, but
  the Vulkan renderer does not currently shade local lights. Wire this existing
  authoring/provider path through, preserving compatibility and cache correctness.
* Vulkan already has a material/depth G-buffer and `composite.frag`. Add local
  direct lighting there, not another complete renderer. `rt_lighting.rgen`
  shades secondary hits through `hit_radiance_sunlit`; local lights must reach
  this path as well as primary visible surfaces.
* Existing RT reflection/transmission material support should carry gold and
  glass. They need actual authored metallic/transmission material properties;
  brown and blue tints alone do not satisfy the requirement.

## 1. Grid and topology contract

### Authoring

Use Y up, metres, integer X/Z cell coordinates. Storeys have authored base Y and
height (normally 4m, allowed 2–6m); geometry within them uses unrestricted float
coordinates. One floor cell occupies [x,x+1] × [z,z+1]. Room dimensions describe
these cells; wall thickness reduces clear interior space explicitly.

A plan contains:

* levels: unique id, base Y, height, rooms and optional roof/deck assignments;
* rooms: unique id, use (hall/kitchen/pantry/guardroom/chamber/chapel/stair/court),
  rectangle or explicit cell union, floor type, occupied level;
* edge overrides: grid endpoints, wall/open/door/window/arch, dimensions and
  material/profile; closed windows do not count as circulation portals;
* curves: integer centre/radius and quarter-circle endpoints or complete rings,
  height/thickness and ordered aperture intervals; connector sockets at cardinal
  endpoints include tangent orientation and cross-section;
* stairs: lower/upper level, rectangular footprint, width, rise/run, landing,
  entry/exit direction; compiler reserves floor/ceiling voids;
* beam graph: endpoints, section, joint family, role; floor joists, posts,
  braces and roof trusses are ordinary members of this graph;
* fixtures/furniture: placement, orientation and reserved clearance envelope;
* seed and style: deterministic variation independent of iteration order.

Start with explicit plans plus constrained template parameters. Avoid a random
WFC room generator: a room/circulation graph gives stronger architectural intent.
The matching-edge resolver operates after the floor plan establishes wall locations.

### Compilation and matching edges

Canonicalize each undirected cell boundary exactly once. Neighboring cells of
one room share no wall. Different rooms share ONE partition, overridden by an
explicit door/arch/open connection. Exterior boundaries get exterior wall
profiles. Courts are open-to-sky floor regions, not missing floor data.

At each grid vertex classify incident walls as end, straight, L, T or cross.
Assign corner/junction volume to a unique junction owner and trim connecting
runs consistently; do not double-stamp corner bricks. The wall edge socket
includes section/thickness, storey height, material/bond family and opening
intervals. A resolver chooses compatible straight/corner/end/window/door pieces
and merges compatible runs into 1/2/4/8m modules without crossing a junction,
opening discontinuity or material change. A module retains its source edge ids.

Straight and curved walls share endpoint sockets. Curves have exact cardinal
endpoints on the metre grid but radial geometry between them. Tangent joins use
an explicit transition/junction, never staircase approximation. Circular tower
rooms have radial floor boundaries and door portals connected to a corridor.
Curved stone courses use radial/wedge placement with an appropriate inner/outer
joint allowance and staggered angular joints.

The pure compiler returns a stable serializable manifest: walls, junctions,
floors, curves, beam members, stairs, roofs, fixtures, local lights, occupied
volumes, room graph and walk route. Emit lightweight part recipes from this
manifest. Engine Part params stay scalar (indices/ids/seed/material handles),
not nested plan JSON. IDs and ordering must remain stable under unrelated room
insertion or input reordering.

### Public JS boundaries

Freeze the exact exported API in the first kit task and document an example.
Recommended boundaries (names may be refined once, before downstream work):

* `castle_plan.js`: compilePlan(plan), validatePlan(plan), edge/socket utilities;
* `castle_materials.js`: defineCastleMaterials(), deterministic material variants;
* `castle_primitives.js`: voxel stone, beam, plank and trim emitters;
* `castle_masonry.js`: wall/junction/curve/opening assembly from compiler records;
* `castle_structure.js`: floors, beam joints, stairs and roofs;
* `castle_furnishings.js`: tables, benches, chair, bed, cupboard/chest, sconces,
  chandelier, altar, gold trim and glazed window/tracery helpers;
* `castle_variants.js` and `castle_scene.js`: parameterized authored plans,
  manifest-to-World roots/light lists, user-facing scene orchestration.

Thin reusable object wrappers go in `projects/world_demo/objects/Castle*.js`.
Do not hide the main implementation inside scene-local objects.

## 2. Architectural detail and interiors

Voxel stone primitives: a family of seeded dressed/rubble/quoins/wedge stones,
beveled and chipped corners, shallow face undulation and pits, occasional tool
marks, consistent bed surfaces. Around 0.02–0.04m voxel sampling for a physical
brick; reuse 8–16 deterministic variants. A detail budget can exist, but the
showcase must retain close-range geometry and must not become flat wall textures.
Brick sizes normally about 0.5–0.9m long, 0.2–0.35m high; mortar joints 8–15mm.
Thickness and both wall faces must exist. Wall openings are empty volumes with
jambs, sills, lintels/voussoirs, not dark planes over solid masonry.

Wood: shaped beam/plank cross-sections, bevels, shallow lengthwise checks, grain
relief and knots, pegged mortise/tenon or scarf-joint visuals, end caps and iron
straps. Endpoint graph deduplicates members and joints. 1/2/4/8m beam lengths,
quarter-turn orientations, arbitrary diagonal braces, and sockets at grid nodes.
Joist spacing can be 0.5m; cosmetic work need not obey metre spacing.

Floors: laid stone flags with joints or planks supported by joists; model underside
and occupied thickness. Stairs: physical steps, landing, handrail/posts where
appropriate, real destination opening and at least 2.1m clear headroom. Prefer
risers <=0.2m, treads >=0.25m and passage width >=1.2m. Doors normally >=1.2m ×2.2m.
Guardrails on accessible balconies and wall walks. Roofs have thickness, ridge,
eaves and trusses; attic access is deliberate. Interior ceilings are not roof
backfaces. Furniture must leave door swings and walking routes open.

Visual palette: weathered warm limestone, darker foundations, terracotta/slate
roof alternatives, warm oak, forged iron, polished and aged gold (metallic=1,
roughness ~0.12–0.28), clear and subtly colored glass with transmission/IOR.
Pair each luminous fixture with an analytic light from the same transform so
moving it moves both. Keep emitters small and avoid overexposed white interiors.

## 3. Local lights: shared direct shading and bounced radiance

### Authoring and runtime

Expose `World.lights = { sun, sky, points: [...], spots: [...] }`, maintaining
legacy point-array and spot-object forms. Point: position/color/intensity/range,
optional sourceRadius and castsShadow. Spot additionally direction/inner/outer
half-angles in degrees. Validate finite values, positive range, nonnegative
intensity/radius, normalized nonzero spot direction and ordered valid cone angles.
Document units and a smooth finite-range inverse-square attenuation, softened
near the source radius. A zero light list must preserve existing scene rendering.

Prefer one resolved LocalLight representation shared by authoring/provider,
renderer upload and CPU tests. Preserve serialization semantics with a cache
version bump if the existing resolved record changes. Camera movement must not
rebake geometry. Lights must update on scene reload; light-only edits must not
leave stale descriptors or accumulation history.

### Many-light lookup

Use deferred local shading at the existing G-buffer composite stage. Build a
world-space spatial light index (initially 8m cells, configurable internally)
for finite-range light spheres, with compact offsets/counts and light-index lists.
This is a suitable first implementation for mostly static architecture: build
when the light list changes and reuse for every camera frame. The same index
works at off-screen RT hit positions, unlike camera-frustum-only clusters.

A sparse hashed grid or bounded dense grid plus an explicit large-light list is
acceptable; choose based on existing renderer buffer infrastructure. CPU list
construction is acceptable for hundreds of static lights, while shading is GPU.
No per-pixel scan of every world light in the normal sparse case. No silent
64-light truncation. Define and test oversized lights, negative coordinates,
cell boundaries, dense overlaps and empty scenes. Count candidates/evaluations,
report upload/index size, and provide a diagnostic view or stats. If an index
allocation fails, return a real error or a documented correctness fallback.
GPU per-frame ownership, descriptor lifetime and zero-light fallback matter.

Evaluate the same attenuation, spot cone and energy-conscious diffuse/GGX specular
in raster and RT. Gold must receive local-light highlights. Raster works without
RT hardware; baseline raster local lights may be unshadowed. RT adds traced local
visibility, soft-source sampling where useful, and actual bounced radiance.
Raster shadow atlases are a follow-up only if the implementation budget allows;
do not label unshadowed raster lights as shadowed.

### RT visibility and GI

At a primary surface, evaluate candidate local lights with rays ending at the
sampled light (not infinity). Reuse existing opaque/alpha/transmission visibility
rules. At diffuse/reflection/transmission secondary hits evaluate those same
local lights, with visibility, as outgoing reflected radiance. A ray that hits a
locally lit wall can then bring its color to another surface: this is the requested
first-bounce GI. Do not mutate material emission to implement it, which would
make the wall self-lit even behind an occluder and double-count energy.

Keep local direct and indirect ownership explicit. Raster direct plus RT direct
must never be added twice. Prefer a separate direct local-light output/resolve
lane, or another proved decomposition, so turning off diffuse GI does not turn
off direct point lights. Existing diffuse/reflection/transmission denoisers can
filter bounce radiance; direct shadow denoising/history must not smear material
boundaries. Reset relevant history on light edits. Preserve zero-light appearance.

Test a sealed room, a doorway between rooms, a spotlight cone, a glossy gold
object, glass between a light and receiver, and a colored lit surface whose bounce
is visible around a corner only with GI on. Demonstrate 256+ visible distributed
lights; measure actual GPU/frame time and candidate distribution, rather than
promise a particular FPS before measurements.

## 4. Three castle variations

1. **Courtyard residence**: 36×40m approximate footprint, gate passage -> open
   court -> great hall; kitchen/pantry service side, two upper chambers, chapel,
   gallery, stairs to wall walk. Rectangular towers and a deliberately mixed
   stone ground floor / oak upper floor.
2. **Round-tower keep**: compact roughly 24×28m plan, round stair/guard tower with
   grid-tangent corridor connections, double-height hall and upper gallery,
   circular floors and conical roof; darker stone/slate and gold/clear glass.
3. **Cloister stronghold**: elongated roughly 44×28m plan, arcaded court, chapel
   with stained glass, refectory and dormitory wing, paired stair connections,
   timber roof and furnished cloister bays.

Exact footprints may be tuned for circulation and composition. These must differ
in room graph, footprint, tower geometry and roof form, not merely random tint.
Provide individual scenes and a CastleGallery showing all three with sufficient
separation. Terrain is a simple neutral presentation slab. Include day exterior,
dusk/night exterior, interior hall, stairs/gallery, chapel glass and material
close-up cameras. Keep a clean plan/cutaway diagnostic alongside beauty captures.

## 5. Acceptance and implementation order

A. Kit topology/primitive contracts and light transport plumbing can start in
parallel after this spec. Engine renderer tasks follow one another to prevent
concurrent edits to the same renderer/descriptor code. Architectural component
work follows the frozen plan/primitive API; furnishings can run alongside it.
B. Each child ships focused tests, a usable example/fixture, and committed work
through agent queue. The parent integration task belongs to Astra, who assembles
and debug-renders the final scenes rather than treating completed children as
proof that the castle works.
C. Geometry QA: determinism, shared-wall uniqueness, wall coverage, compatible
sockets, L/T/cross corners, negative grid coordinates, arc-to-straight joins,
opening clearance, floor holes, stair connectivity/headroom, supported floors,
furniture clearance, declared child params and finite closed geometry.
D. Render QA: canonical Windows MSVC editor build; native screenshots in both
raster and RT; no bake/flatten/Vulkan validation errors. Light stress fixture
and zero-light regression. Walk-route eye-level captures must traverse doors,
stairs and upstairs rooms without using a cutaway to hide blocked passages.
The existing editor provides fly navigation; true collision-constrained walking
is an integration decision that must be stated explicitly if unavailable.
E. Deliver source, spec, task graph/results, test evidence, reproducible capture
scripts, floor-plan SVG/JSON, camera routes, screenshots, and measured limitations.
Do not declare the project complete from mocks or a handful of exterior views.


## Parallel worker policy

User refinement: task workers should proactively use subagents for independent
bounded implementation, fixture/test work, and review to shorten elapsed time.
Give each subagent explicit file ownership and the relevant frozen API. Keep
dependent edits and overlapping renderer work sequential. The owning AQ worker
integrates all subagent output, verifies the complete component and delivers its
commit/evidence. Coordinate GPU captures and native builds to avoid contention.

Provider allocation is approximately 70% Codex / 30% standard-intelligence Claude
by leaf task count, including Astra's two Codex integration tasks. Masonry and
structural geometry use Claude standard-high; furnishings use Claude
standard-medium. Plan/compiler, primitives, all three lighting tasks, and final
Astra assembly/acceptance stay on Codex. Subagents should normally inherit their
parent provider. Rebalance future tasks if their scope changes substantially.


## Astra review and rework policy

User explicitly authorizes reviewing all agent work and reopening unacceptable
tasks with feedback. A worker completion is a candidate for acceptance, not proof
that the system works. Astra reviews the actual diff, public contracts, focused
test evidence and native captures where applicable, then checks component
interactions during final assembly. Reopen failed work with a concrete reproducer,
file/behavior findings and the acceptance checks needed to close it. Do not waive
missing geometry, blocked circulation, faux glass/gold, unlit analytic lights,
double-counted direct light or missing bounced-light evidence to keep the graph
moving. Reuse the task's owner for bounded rework when possible.

## Contract clarifications from Astra design review

* Gallery-to-gallery adjacency needs an explicit open edge, since different room
  ids otherwise produce a partition. Validate the full walking envelope along
  routes, not only room-graph connectivity. Circulation portals require occupied
  walkable floor on both sides, except declared exterior entries.
* Distinguish tangent wall sockets from radial room-entry sockets. A circular
  tower only touches a tangent corridor plane at one point. Its doorway needs
  an owned finite-width throat: cut the arc, extend the floor to the grid plane,
  build the two reveals/side returns, and connect both room volumes. No zero-width
  adjacency may count as a portal. Circular floor tiles clip to the real radius.
* Freeze stairs with lower/upper level, direction, rise, run, flight footprint,
  lower/upper landings and destination hole. A 4m rise needs >=20 risers at <=0.2m;
  a 6m flight footprint may work, but landing lengths are additional. Validate
  the swept >=2.1m headroom envelope against slab, joist and beam volumes. Include
  a real staircase in the round tower; its label is not a staircase.
* Double-height spaces need explicit vertical void records. Balcony edges use
  rail/open profiles, not automatically exterior solid walls. Each suspended
  floor declares bearing edges, joist direction and intermediate supports; roof
  and ceiling generation must respect the same shafts/voids.
* Local direct lighting has one owner: raster evaluates the unshadowed local
  BRDF in composite when native local RT is inactive; native RT supplies a
  separate local-direct radiance lane when active. The composite selects one,
  never sums both. The local-direct lane includes diffuse/specular and correct
  transmission weighting, and is independent of the diffuse-GI multiplier.
  Indirect local-light radiance stays in existing bounce/reflection/transmission
  lanes. Clear the direct lane and active flag on RT failure/disable/zero lights;
  invalidation on any light-record/index edit covers dependent bounce histories.
* Analytic-fixture glow proxies are cosmetic and must not independently transport
  the same source energy through GI. Initially use a small separate glow Part
  with `rayTraced(false)`, retaining visible raster emission and normal RT
  visibility for the fixture's wood/metal/glass body. Analytic direct lighting
  supplies the source power; illuminated secondary surfaces supply bounce.
  Generic unrelated emissive materials retain their existing GI behavior. Test
  proxy visibility toggles to establish this source-ownership contract.

## Research references

The chosen approach applies clustered local-light selection to the existing
G-buffer and shares a world-space index with RT hit shading. This is a design
choice for this repository, informed by:

* [Khronos: Forward, Forward+ and Deferred](https://docs.vulkan.org/tutorial/latest/Building_a_Simple_Engine/Advanced_Topics/Forward_ForwardPlus_Deferred.html).
* [Olsson, Billeter and Assarsson: Clustered Deferred and Forward Shading](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf).
* [Khronos ray-tracing sample: shadow visibility](https://github.khronos.org/Vulkan-Site/samples/latest/samples/extensions/ray_tracing_extended/README.html).
