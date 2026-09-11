# CastleStructure fixture

Native fixture World for the castle kit's structural-geometry layer
(`shared-lib/castle_structure.js`): floors (stone flags and oak planks with
their joist/trimmer/header graph), a dog-leg timber stair with landings and
guard rails, the oak-frame bay (posts, braces, mortise/peg/strap joints), and
the pass-through gable/hip/conical roof records, all replayed from the
two-storey fixture manifest in `shared-lib/castle_structure_fixture.js` (a
hall/pantry pair plus a circular guard tower, stacked over a matching upper
floor). Walls are intentionally absent -- masonry is a separate component --
so this reads as a structural cut-away rather than a finished building. Run
it as world `CastleStructure`.

Dimensions are metres. The authoritative op model, wrapper pattern, and
per-family emitters (`emitFloor`/`emitStair`/`emitRoof`/`emitFrame`) are
documented at the top of `shared-lib/castle_structure.js`.

## Files

- `CastleStructure.js` -- the World: a `CastleStructureGround` presentation
  slab plus one `CastleStructureFixturePart` root per structure record
  returned by `structureRecipes(castleStructureFixtureManifest(), ...)`.
- `objects/CastleStructureFixturePart.js` -- thin wrapper: `static
  requires(p)` forwards to `structureChildVariants`, `build(p)` forwards to
  `emitStructure`. Every param is a flat scalar (manifest id, record id,
  material handles, ...); the manifest itself is closed over, not a param.
- `objects/CastleStructureGround.js` -- fixture-only neutral slab. The
  reusable structure kit never emits its own ground plane, so this gives the
  cut-away something to sit on; it is not part of the primitive API.

## Running the tests

```bash
node projects/world_demo/tests/castle_structure_tests.mjs
```

`castle_structure_tests.mjs` compiles both the small `two-room-two-level`
plan fixture and `castleStructureFixtureManifest()`, then checks
determinism, `validateStructure`, stair continuity/headroom, floor
composition, the beam/joint graph, closed 2-manifold mesh output, and
`emitStructure`/`structureChildVariants` agreement. Its roof-op-role section
is written against the target contract (roof-tile/roof-boarding/rafter/
fascia/ridge-cap/hip-cap/gable-infill/finial) and is expected to fail until
`castle_structure.js`'s roof geometry (the `layoutRoofGeometry` seam in
`layoutRoof()`) lands; every other section is expected to pass today.

## Capturing a screenshot

```bash
cd MatterEditor
MATTER_WORLD=CastleStructure MATTER_SCREENSHOT="C:/tmp/castle_structure.png" \
MATTER_SCREENSHOT_SETTLE=90 \
TMP="C:/Users/webde/AppData/Local/Temp" TEMP="C:/Users/webde/AppData/Local/Temp" \
./build/windows-msvc/editor.exe
```

See `docs/agent/qa-cookbook.md` and the root `CLAUDE.md` QA quick reference
for the FIFO-driven multi-shot alternative (`MatterEngine3/tools/drive.py`).
