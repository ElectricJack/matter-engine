# Castle connector fixture

Native proof for exact polygon vestibules at +15, +30, +45, and -30 degrees.
Each bay uses clipped flagstone prisms, individually placed masonry with XZ-plan
cut stones at the jambs, a tiled low hip with open tile joints, a closed timber underside/fascia and timber rafters.
Mortar is recessed on every wall face, including angled jamb trim planes. The 8 cm roof shell sits above the
enclosure height, preserving the specified passage headroom. The fixture declares
3.9 m enclosure height and 3.6 m clear passage, reserving structural depth for
its finite 18 by 14 cm rafters. Connector normalization clips each oriented
rafter box against the full clear polygon and rejects insufficient headroom
before producing geometry or collision; low roof rise cannot waive this check. Static collision is
generated from the same solid volumes as the visible layout: rectangular pieces
use oriented boxes and boundary pieces use flat-point convex hulls.

Angled jamb bricks use two reusable `CastleTriangularStone` voxel stocks, fitted
with full affine XZ transforms and a height scale. Their exterior vertical faces
carry dressed chisel marks; their common diagonal is undressed, with no grout
or triangle inset. Whole-brick profile/ownership and collision stay unchanged.
The shader must include the full inverse-transpose normal transform for these
affine instances (bake version 10 or later).

Each connector is two coincident roots: an unexpanded inline mesh layer for
clipped boundary flags/mortar/roof tiles, and an expanded child-only assembly
for reusable interior flagstones, masonry and rafters. This keeps fine voxel
children instanced and below the native static-vertex reserve.

The +30 record preserves the frozen study's world mouth centres `[12,0,6]` and
`[18,0,6]`; its hall origin metadata remains `[16,0,2.5358983849]`. The other
bays exercise signed 15-degree increments without quantizing world coordinates.

Run `capture.ps1` from PowerShell after building the canonical MSVC editor
and obtaining the shared GPU slot. It uses `tools/castle_scene_capture.py` to
wait for complete numeric publication, viewer readiness, clean baking and an
idle acknowledgment before submitting the timeline. The timeline explicitly
sets exposure to zero so saved editor exposure cannot contaminate captures. The archived receipt is
`build/qa/castle-connectors/capture.json`. Captures include the four signed
angle interiors, a raster overview and an RT roof underside. A capture receipt
proves readiness; the resulting images still require visual acceptance.

Native geometry acceptance (2026-09-11): the publication-gated fixture completed
with zero bake errors under the MSVC NormalMat/bake-10/PhysX build. Final RT
inspection confirmed continuous floor support at wall bases, a closed soffit
with rafters below it, and trimmed jambs without coplanar mortar striping.
`build/qa/castle-connectors/final-rt/` contains the final three images, timeline,
log and successful receipt. These are connector geometry evidence; complete
site entrance/stair traversal is accepted separately by the site walkthrough.
