# CastleMasonry fixture

Native acceptance fixture for the grid-castle masonry kit
(`shared-lib/castle_masonry.js`, `shared-lib/castle_plan.js`,
`shared-lib/castle_masonry_fixture.js`). Run it as world `CastleMasonry`.

The world's single non-ground root, `CastleMasonryFixture`, compiles the
authored plan `CASTLE_MASONRY_FIXTURE_PLAN`
(`shared-lib/castle_masonry_fixture.js`) once at module load and places
every record `layoutMasonry()` yields via `emitMasonry()`. In one manifest
this exercises:

* Straight wall-module lengths of 8 m, 4 m, 2 m, and 1 m.
* Windows on exterior walls (`a-south-window`, `b-east-window`).
* Doors and an arch (`front-door`, `hall-c-door`, `c-d-door`, `b-d-arch`).
* Every corner-junction kind: an `L` corner, a `T`, a `cross` (all four
  guardroom/storeroom quadrants meeting at once), and `end` junctions where
  the `a-c-open` boundary cuts the a/c partition.
* A quarter-curve apse rounding the hall's north-east corner, tangent-joined
  to straight walls at both ends (the transitions stay visually closed --
  `castle_masonry_tests.mjs` samples across each seam).
* A ring tower with a radial throat: a circular room bridged into the hall
  through a host arch cut into the hall's west wall, plus three ring windows.

See the header comment in `shared-lib/castle_masonry_fixture.js` for the full
authored-plan rationale, and the top of `shared-lib/castle_masonry.js` for the
masonry API contract (`masonryOptions`, `layoutMasonry`, `emitMasonry`,
`masonryChildVariants`, transform convention).

## This scene's files

* `CastleMasonry.js` -- the World. Two roots: `CastleMasonryGround` (a plain
  slab, not `expand`) and `CastleMasonryFixture` (`expand: true`, since
  masonry is child-only -- every placement under it is a `placeChild` of
  `CastleStone`, `CastleWedgeStone`, `CastleMortarCore`, or `CastleBeam` from
  the project-wide `objects/`, never inline geometry).
* `objects/CastleMasonryFixture.js` -- the child-only Part described above.
  `static requires(p)` returns exactly what `build(p)` places, both computed
  from the same `masonryOptions(p)` object, so the two paths cannot diverge.
* `objects/CastleMasonryGround.js` -- a neutral box slab, top at y=0, sized a
  bit larger than the plan's building footprint (the isolated `bailey-wall`
  stub the plan also declares, at z:[20,21], is deliberately far from the
  building group and outside every camera framing below -- see the comment in
  that file).
* `capture.ps1` -- drives the editor headlessly through both render paths and
  archives the shots (see below).
* `../../tests/castle_masonry_scene_tests.mjs` -- Node-level test for this
  scene (loads the scene scripts through the same `shared-lib/...` ->
  `data:` URL rewrite `castle_masonry_tests.mjs` uses, made recursive since
  these scripts chain through more than one shared module).

## Camera

`CastleMasonry.js`'s `static camera` is the 3/4 elevated overview:
`position: [17, 16, -13]`, `target: [1, 2, 1]`. The building group (hall +
four guardroom/storerooms + ring tower, excluding the far bailey-wall stub)
has an outer footprint of about x:[-6.3, 8.3], z:[-4.3, 6.3]; `[1, 2, 1]` is
that footprint's centre at mid wall-height. The eye sits south-east of the
footprint (positive x and negative z put it toward `b`'s exterior corner) and
well above the 4 m wall height -- this plan has no roof, so from height 16 the
ring tower (the far west side, behind the hall) is not occluded by the hall's
own walls, letting one shot read hall exterior windows, the apse, and the
tower together.

`capture.ps1`'s timeline overrides `cam` per shot; coordinates were computed
from the compiled manifest (`compilePlan(CASTLE_MASONRY_FIXTURE_PLAN)`), not
guessed:

| shot | subject | camera | target | why |
| --- | --- | --- | --- | --- |
| `overview` | whole fixture | `17 16 -13` | `1 2 1` | same reasoning as `static camera` above |
| `apse-join` | quarter-arc tangent joins | `13 4 9` | `7 2 5` | the apse's tangent endpoints are exactly `[8,4]` and `[6,6]` (curve `hall-apse`, center `[6,4]` radius `2`); target `[7,2,5]` sits on the arc's 45-degree bulge between them, camera pulled back outside the NE corner to frame both joins in one shot |
| `tower-throat` | radial throat, from the hall | `4 1.7 3` | `-3 1.7 3` | the throat's `clearanceVolume` (`radial-throat:tower-ring:tower-throat`) is centred close to `x=0.1, z=3`; camera sits inside the hall (`x=4`, well inside the hall's `x:[0,8]` span) at eye height, looking due west through the opening at the tower's centre `[-3,3]` |
| `window-reveal` | `a-south-window` | `0.5 1.8 -6` | `2 1.5 -4` | this window's compiled span is `x:[1.35,2.65]` on the `z=-4` exterior wall (`bottom 1.0`, `top 2.5`); the camera is offset in x from the target so the 0.6 m wall thickness and sill read in perspective rather than face-on |
| `cross-junction` | the `(4,-2)` cross | `8 5 -7` | `4 1.8 -2` | `(4,-2)` is the one `cross` junction (all four of `a`/`b`/`c`/`d` meet there); elevated SE vantage above the 4 m walls (again, no roof) so the quoin coursing at the corner is visible from outside |
| `interior-arch` | `b-d-arch` | `6 1.6 -3.3` | `6 1.6 -1` | this arch's compiled span is `x:[5.1,6.9]` on the `z=-2` boundary between rooms `b` and `d`; camera stands inside `b` at eye height looking north through the arch into `d` |

## Render-path gate

`capture.ps1` takes every shot above under **both** `render_path raster` and
`render_path native_rt` (the two FIFO-selectable render paths --
`docs/agent/control-surface.md`), each into its own filename prefix, so a
regression that only shows up on one path doesn't slip through.

```powershell
projects/world_demo/scenes/CastleMasonry/capture.ps1 [-OutputDir C:\tmp\matter-castle-masonry]
```

Output defaults to `C:\tmp\matter-castle-masonry\` and is archived to
`build\qa\castle-masonry\` (editor log included) after the script confirms no
bake/flatten/validation error appeared in the log and every expected PNG
exists.

## Verified assemblies noted while building this fixture

Nothing in `shared-lib/castle_masonry.js` looked wrong. Both
`node projects/world_demo/tests/castle_masonry_tests.mjs` (unedited, owned
elsewhere) and `node projects/world_demo/tests/
castle_masonry_scene_tests.mjs` (this scene's own test) pass against the
current `shared-lib/castle_masonry.js`. Mid-session that file had in-progress,
uncommitted voussoir/keystone-arch work (a new `CastleCutStone` module) from a
concurrent worker sharing this worktree, which transiently failed
`castle_masonry_tests.mjs`'s own new assertions before landing as commit
`e6f3e147`; this scene's test never exercised that codepath directly and
passed throughout.
