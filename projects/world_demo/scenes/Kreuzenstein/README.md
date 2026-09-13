# Kreuzenstein castle

A procedural reconstruction of the entrance elevation in the supplied
[Wikimedia photograph](https://upload.wikimedia.org/wikipedia/commons/7/73/Leobendorf_-_Burg_Kreuzenstein_%282%29.JPG).
The masonry is assembled **brick by brick from 32,746 individually placed voxel
stones**. Roof tiles, timber framing, window tracery, ironwork and carved details
are modeled geometry. No image textures or imported meshes are used.

## Open and inspect

Choose **Kreuzenstein** in MatterEditor's world list, or run from the repository:

```powershell
powershell -ExecutionPolicy Bypass -File projects/world_demo/scenes/Kreuzenstein/open.ps1
```

Generate and verify seven native editor screenshots:

```powershell
powershell -ExecutionPolicy Bypass -File projects/world_demo/scenes/Kreuzenstein/capture.ps1
```

`capture.ps1` drives the native MSVC editor through `MatterEngine3/tools/drive.py`.
It captures the reference elevation, entrance, chapel, gallery tower, bridge,
aerial and rear views. It checks process success, screenshot completion sidecars,
and bake/flatten failures, then archives the results in:

- `build/qa/kreuzenstein/final/` — final PNGs, completion markers, timeline and log.
- `build/qa/kreuzenstein/reference/kreuzenstein.jpg` — downloaded original.
- `build/qa/kreuzenstein/` — intermediate captures and investigation logs.

The capture script uses a path without spaces for the editor's `shot` verb,
then copies its results into the repository. The editor executable remains
`MatterEditor/build/windows-msvc/editor.exe`; no engine rebuild is required.

## Photograph analysis and construction plan

The identifying silhouette is asymmetric: a tall square keep and steep hipped
roof, the chapel's pointed gable and delicate bell spire on its left, and the
large round tower with a timber gallery and conical roof on the right. Smaller
roofs overlap the chapel's base. The fortified entrance is recessed between
projecting wall towers, reached across a rising, arched stone bridge.

The construction sequence was:

1. Establish the keep, chapel, spire and round tower as separate objects. Use
   Y up, +Z toward the entrance and approximate metre-scale dimensions.
2. Assemble the enclosing walls, gate, projecting turrets and three bridge
   arches. Build openings from surrounding masonry rather than painting holes.
3. Replace the initial masonry approach with voxel brick assemblies, following
   the user's refinement. Lay staggered courses on flat walls and radial courses
   on the tower; rotate individual voussoirs around the bridge and Gothic arches.
4. Add corbels, quoins, window frames, tracery, dormers, carved finials, portcullis,
   gallery posts and braces, overlapping individual roof tiles and ridge caps.
5. Use subdued limestone, terracotta, wood and iron materials. Add an inferred
   paved courtyard, a well, rear windows and a simple grassy hillside.
6. Iterate through native editor captures: correct silhouette overlap, enlarge
   and raise the chapel, shorten the foreground round tower, broaden the palas,
   fix roof ridge orientation, close foundation gaps, bond the bridge piers,
   enclose the side-wing gables, and inspect close-ups.
7. Verify the final composition from seven viewpoints and retain the commands.

Dimensions and hidden elevations are inferred from one photograph. Sculptural
finials and the Gothic ornament are procedural approximations rather than scans.

## Composition

| Object                   | Geometry                                                                        |
| ------------------------ | ------------------------------------------------------------------------------- |
| `KreuzensteinKeep`       | Square keep, machicolations, dormer, rear palas and side wing                   |
| `KreuzensteinChapel`     | Raised Gothic window, tracery, brick gable, buttresses, timber projection       |
| `KreuzensteinSpire`      | Open four-sided belfry, pinnacles, crockets and finial                          |
| `KreuzensteinRoundTower` | Battered base, radial stone courses, bracket arches, timber gallery, tiled cone |
| `KreuzensteinCurtain`    | Enclosing walls, projecting turrets, covered wall walks, stone trim             |
| `KreuzensteinGate`       | Pointed entrance, recessed portcullis and tunnel, gatehouse and side walls      |
| `KreuzensteinBridge`     | Three open arches, stone piers, rising flagstone deck and parapets              |
| `KreuzensteinCourt`      | Individual paving stones, low well and hall steps                               |
| `KreuzensteinGround`     | Hillside, approach path, foundation rock and shrubs                             |
| `KreuzensteinBrick`      | Reusable voxel-CSG stone, four material/shape variants                          |

## DSL implementation

The brick is a voxel box with three subtractive corner chips, sampled at 0.05
units. A modifier block simplifies its meshed surface to 430 triangles. Every
stone in the masonry is an actual placement of this voxel-generated part,
scaled and rotated to fit its course. Four explicitly defined matte limestone
materials give deterministic color variation; voxel baking does not preserve
per-brush `tint`, so color is supplied through material handles.

The uniquely named `shared-lib/kreuzenstein.js` contains the architectural
helpers. The engine resolver accepts flat shared-lib imports, so it lives at
project level; only this scene imports it. Scene-local object files own the
building composition and dimensions.

Each building is evaluated in two stages. `masonry` emits brick placements and
is an `expand: true` world root, promoting the stones to independent scene
instances. `details` emits roof tiles, timber, glazing and ornament as named
parts. The bridge and courtyard need only the masonry stage. This produces 15
world-root declarations and avoids repeatedly flattening millions of brick
triangles into each building's LODs. The final scene retains full geometry using
one-rung detail and brick budgets. The launcher disables impostors for this
close architectural study.

Part parameters must be flat scalar values; `stoneBase` carries the first
material handle, and the four declared child variants use `stoneBase + seed`.
Keep `static requires` and `placeChild` parameter sets identical when adding
variants. Modern simplification uses `beginModifier()` / `endModifier()`;
the older `this.simplify()` spelling in the onboarding example is unsupported.

## Validation

```bash
node projects/world_demo/tests/kreuzenstein_scene_tests.mjs
```

The test executes all roots and brick variants against the repository's actual
`Part` JavaScript prelude with recorded native calls. It checks finite geometry,
balanced transform/shape/voxel sessions, scalar parameters, declared child
variants and material handles, and that masonry stages contain only voxel brick
placements. Current census: **32,746 voxel stones**, four voxel-CSG variants,
and **96,793 explicit detail triangles**, plus the DSL's solid primitives.

The final native capture completed with exit code 0, seven verified PNGs and
completion sidecars, and zero bake errors. The scene test above passed. The UI
launcher was also exercised and its loaded scene checked with an editor screenshot.
Inspect the seven final PNGs and `editor.log` for visual verification.
Intermediate diagnostic views include raw albedo and several
lighting comparisons; the final settings are saved in `props.json` and repeated
in the capture timeline.
