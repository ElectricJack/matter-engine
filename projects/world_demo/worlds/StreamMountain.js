// StreamMountain — colossal alpine streaming terrain.
//
// Broad warped massifs establish whole ranges, ridged noise forms the alpine
// profile, and elevation masks keep lower slopes calmer while carving the
// middle elevations and sharpening the summits. Fine 6/3/1.5 m ridged detail
// roughens upper slopes without adding noise to the valley floor.
//
// APPEARANCE (chart-VT spec Phase 4) comes from the four materials declared
// below plus the surfaces() classifier tape at the bottom of this file:
// green-brown valley floors, gray walls on the steep faces, scree fans on the
// mid-steep aprons under those walls, and a ragged snow line across the
// 450-650 m crests. The geometry band table in WorldSector.js is untouched —
// terrain still meshes as one all-dirt bucket; only texturing is classified.

// ---------------------------------------------------------------------------
// Materials (chart-VT Phase 3). Each names a Wang detail tileset the page
// compositor triplanar-samples at page-bake time. A `detail:` module that does
// not exist yet is fail-closed BY DESIGN: the material renders with its scalar
// albedo/roughness below and the tileset phase reports it once, so this world
// is correct before and after the Alpine* detail scenes land.
//
// Declaration order is load-bearing twice over:
//   * the detail-bake plan (detail_bake_plan.h) walks deprecated `tileset:true`
//     roots first, then materials in declaration order, and STOPS at the first
//     settle failure — AlpineGround/ForestFloor is declared first so the one
//     detail scene that exists today always gets its slot;
//   * material 0 of the tape is the fallback when every weight is 0
//     (SurfaceRuntime::classify_vertices), and AlpineGround is the right
//     answer for "unclassified terrain".
// ---------------------------------------------------------------------------

// Valley floors and gentle lower slopes: the existing baked litter scene.
const ALPINE_GROUND = defineMaterial("AlpineGround", {
  albedo: [0.32, 0.30, 0.22],
  roughness: 1.0,
  detail: "ForestFloor",
});
// Steep faces at any altitude — the gray alpine walls.
// The albedo is deliberately NEUTRAL (r ~ g ~ b) rather than the warm gray it
// used to be. The tape's hue lane swings R against B by up to +/-0.32 around
// whatever this base is, and a base that already leans tan can only ever produce
// more tan — which is exactly how it read. A neutral base lets the same lane
// reach cool blue-gray on one side and warm oxide on the other.
// Luminance is unchanged, so scene brightness and the snow/rock contrast hold.
const ALPINE_ROCK = defineMaterial("AlpineRock", {
  albedo: [0.44, 0.43, 0.42],
  roughness: 0.95,
  detail: "AlpineRockDetail",
});
// Talus aprons under the walls, and stony ground above the green belt.
const SCREE = defineMaterial("Scree", {
  albedo: [0.48, 0.47, 0.46],
  roughness: 1.0,
  detail: "ScreeDetail",
});
// Snowfields on the crests and high benches.
const ALPINE_SNOW = defineMaterial("AlpineSnow", {
  albedo: [0.90, 0.90, 0.95],
  roughness: 0.75,
  detail: "AlpineSnowDetail",
});
// Meadow turf: the green clumps on gentle, moist, sub-treeline ground.
// Declared LAST on purpose — declaration order is load-bearing (see the header
// comment above: the bake plan stops at the first settle failure, and material 0
// is the all-weights-zero fallback), so a new material goes on the end and the
// existing four keep their slots and their meaning. AlpineMeadowDetail does not
// exist yet; that is fail-closed by design and this renders as flat green until
// the tileset lands.
const ALPINE_MEADOW = defineMaterial("AlpineMeadow", {
  albedo: [0.20, 0.34, 0.14],
  roughness: 0.95,
  detail: "AlpineMeadowDetail",
});

class StreamMountain extends World {
  static params = { worldSeed: 20260722 };
  static world = { sectorSize: 64, yMin: -96, yMax: 704 };

  static camera = {
    position: [20.0, 760.0, 350.0],
    target:   [0.0, 420.0, 0.0],
  };

  static fog = {
    density:    0.180,
    minHeight:  140.0,
    maxHeight:  165.0,
    noiseScale: 0.00022,
    color:     [0.90, 0.92, 0.95],
    wind:      [0.12, 0.0, 0.04],
  };

  // 2026-07-29 alpine tuning pass: with the heightfield terrain-LOD ladder,
  // distant sectors cost a handful of triangles, so both the scatter rings
  // and the terrain bands reach much farther than the old full-detail-only
  // streamer could afford (which OOMed at 4800 m of voxel sectors).
  static streaming = {
    rings: [
      { radius: 368.0, rung: 2 },
      { radius: 1115.0, rung: 1 },
      { radius: 2922.0, rung: 0 },
    ],
    // Heightfield terrain LOD bands (radius -> LOD, 5 = native voxel mesh,
    // 0 = one quad). Hand-tuned in the editor's LOD Settings window.
    terrainBands: [
      { radius: 961.0, lod: 5 },
      { radius: 1486.0, lod: 4 },
      { radius: 2120.0, lod: 3 },
      { radius: 2862.0, lod: 2 },
      { radius: 5943.0, lod: 1 },
      { radius: 10095.0, lod: 0 },
    ],
  };

  // Editor volumetrics defaults for this world (adopted into the live
  // volumetrics controls on world load). Thin fog multiplier + strong
  // falloff keep the long alpine sightlines readable.
  //
  // Off by default (2026-07-29). The tuned multipliers below are kept so the
  // viewer's Volumetrics "Enable" checkbox restores this look in one click.
  static volumetrics = {
    enabled: false,
    phaseG: 0.30,
    temporalBlend: 0.85,
    fogDensityMul: 0.06,
    fogFalloffMul: 3.44,
  };

  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  // `tileset: true` is the deprecated pre-defineMaterial alias, and it is kept
  // deliberately. It plans a ForestFloor bake bound to material 16 (DIRT) —
  // the bucket the geometry band table still emits — and plan_detail_bakes()
  // MERGES it with AlpineGround's `detail: "ForestFloor"` because both agree on
  // (module, params "{}", density 0). One settle, one .gtex, one slot, two
  // bound materials: no double bake and no wasted slot (4 declared detail
  // scenes -> 4 of the 8 slots). Dropping the root would only cost DIRT its
  // binding, which is what the legacy non-VT path and any part that bakes
  // without charts still sample.
  static roots = [
    {
      module:    "ForestFloor",
      transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
      tileset:   true
    },
  ];

  field(p) {
    // Whole ranges: a 1.8 km control field, bent by a 2.4 km warp.
    const massifBase = noise2(p.worldSeed ^ 0x31, 1/1800, 3);
    const massif = warp2(
      massifBase, p.worldSeed ^ 0x32, 1/2400, 260
    ).smoothstep(-0.45, 0.35);

    // Main alpine ridges. Nonlinear remapping compresses the lowlands into
    // narrow valleys while preserving wide connected mountain bodies.
    const primaryBase = ridge2(p.worldSeed ^ 0x41, 1/950, 5, 0.55, 2.0);
    const primary = warp2(
      primaryBase, p.worldSeed ^ 0x42, 1/1700, 240
    ).add(1).mul(0.5).smoothstep(0.15, 0.85);
    const body = primary.mul(massif);

    const cragNoise = ridge2(p.worldSeed ^ 0x51, 1/250, 4, 0.52, 2.0);
    const channelNoise = ridge2(p.worldSeed ^ 0x61, 1/190, 3, 0.5, 2.0)
      .add(1).mul(0.5).smoothstep(0.55, 0.90);
    const valleyNoise = noise2(p.worldSeed ^ 0x71, 1/320, 2);
    const broadNoise = noise2(p.worldSeed ^ 0x81, 1/520, 3).mul(24);

    // Three octaves at 6 m, 3 m, and 1.5 m. The 2 m terrain lattice captures
    // the coarse part geometrically while the finest octave breaks up normals.
    const surfaceNoise = ridge2(
      p.worldSeed ^ 0x91, 1/6, 3, 0.55, 2.0
    );

    const baseHeight = body.mul(590).add(broadNoise).add(-22);

    const upperMask  = body.smoothstep(0.48, 0.78);
    const crags      = cragNoise.mul(68).mul(upperMask);

    const surfaceMask      = body.smoothstep(0.30, 0.62);
    const surfaceRoughness = surfaceNoise.mul(6.0).mul(surfaceMask);

    const lowerGate  = body.smoothstep(0.12, 0.40);
    const upperGate  = body.smoothstep(0.58, 0.85);
    const middleMask = lowerGate.mul(upperGate.mul(-1).add(1));
    const channels   = channelNoise.mul(-58).mul(middleMask);

    const valleyMask  = body.smoothstep(0.08, 0.25).mul(-1).add(1);
    const valleyFloor = valleyNoise.mul(9).mul(valleyMask);

    const height = baseHeight
      .add(crags)
      .add(surfaceRoughness)
      .add(channels)
      .add(valleyFloor);

    // Emit biome controls after height so height_at() does not evaluate them.
    //
    // HISTORICAL NOTE: these two noise channels used to be the tape's only
    // world-space noise — surfaces() read them back as s.moisture / s.relief
    // before s.noise2World existed. The tape now samples the SAME fbm (same
    // seeds, same tuning) directly, so these are dead for appearance; they are
    // kept BYTE-IDENTICAL anyway because any edit here re-hashes the field
    // program and re-bakes every sector part for zero visual gain. Retire them
    // (plain constants) the next time the field changes for real reasons.
    // For scatter they remain inert: both stay inside [-1, 1], far below the
    // (deliberately unreachable) 2.0 biome thresholds above, so every sector
    // still classifies as `foothills` exactly as before.
    const relief = noise2(p.worldSeed ^ 0xA1, 1/420, 4, 0.55, 2.0);
    const moisture = noise2(p.worldSeed ^ 0xA2, 1/260, 3, 0.55, 2.0);
    return {
      density: heightToDensity(height),
      moisture,
      relief,
      seaLevel: -80.0
    };
  }

  // -------------------------------------------------------------------------
  // The classifier tape (chart-VT Phase 4). Compiled native and evaluated per
  // chart vertex; world inputs (altitude, world-frame noise) are legal because
  // terrain sectors are world-anchored variants — one instance per sector.
  //
  // Weights are NORMALIZED by the runtime and the compositor keeps the top 2,
  // so what matters is each material's weight RELATIVE to its neighbours; the
  // oneMinus() products below are what make one class actually displace
  // another instead of averaging with it. (Budget note: the tape is capped at
  // 64 EMITTED ops, but the parser dedups identical consts and oneMinus is a
  // native single op, so no hoisting tricks are needed.)
  // -------------------------------------------------------------------------
  surfaces(s) {
    const seed = StreamMountain.params.worldSeed;

    const slope = s.slope;          // 1 - normal.y: 0.13 @30deg, 0.29 @45deg,
    const up = s.normalY;           //               0.50 @60deg, 0.71 @75deg
    const alt = s.altitude;         // world y (sectors are world-anchored)
    // World-frame fbm, sampled directly. macro/patch carry the exact seeds and
    // tuning of the field()'s relief/moisture channels they used to be smuggled
    // through, so their values are bit-identical to the pre-migration tape —
    // but appearance edits here now touch only the tape hash (VT pages), never
    // the field program (sector parts).
    //   macro — 4 octaves at 420/210/105/52 m: breaks the snow line into
    //           tongues and fingers and roughens the rock/scree boundary.
    //   patch — 3 octaves at 260/130/65 m: separates scree fans from the
    //           grassier aprons between them.
    //   fine  — 17/8.5 m snow-line grain; world-frame so it no longer repeats
    //           with the 64 m sector period the old part-local sampling had.
    const macro = s.noise2World(seed ^ 0xA1, 1 / 420, 4, 0.55, 2.0);
    const patch = s.noise2World(seed ^ 0xA2, 1 / 260, 3, 0.55, 2.0);
    const fine = s.noise2World(0x5EEDA1, 1 / 17, 2, 0.5, 2.0);

    // --- rock: steep faces, with a ragged rather than iso-slope boundary ----
    // +/-0.055 of slope is about +/-4 deg of wobble on the wall/apron edge, so
    // the wall/talus edge follows the massif instead of tracing an iso-slope.
    const steepN = slope.add(macro.mul(0.055));
    const rock = steepN.smoothstep(0.30, 0.55);             // ~46deg..~63deg
    const invRock = rock.oneMinus();

    // --- snow: altitude-driven, noise-broken, shed from the steep faces -----
    // The snow line is a 400-520 m fade on the RAW altitude, but the altitude
    // fed to it is displaced by +/-55 m of 4-octave world noise and +/-14 m of
    // fine local noise, so the line onsets anywhere in ~345-455 m and completes
    // in ~465-575 m — tongues and patches, never a contour. Summits run
    // 450-650 m, so crests go white while the shoulders stay mottled.
    const snowAlt = alt.add(macro.mul(55)).add(fine.mul(14));
    const snowBand = snowAlt.smoothstep(400, 520);
    // Retention: full on ground flatter than ~31deg, nothing past ~66deg.
    const snowKeep = up.smoothstep(0.41, 0.86);
    const snow = snowBand.mul(snowKeep);
    const invSnow = snow.oneMinus();

    // --- scree: talus aprons around the walls, plus stony high ground -------
    // apron: the talus slope band (~23-37 deg, the real angle of repose),
    // fading out exactly where rock fades in, so the wall/talus handoff is one
    // boundary and not two. dry: the moisture channel carves the fans apart,
    // leaving grassier gullies between them, but only down to 0.4 — a fan is
    // patchy, never absent. highs: no talus on the green valley floors.
    const apron = steepN.smoothstep(0.10, 0.24);
    const dry = patch.smoothstep(-0.45, 0.05);
    // dryFan is written as a lerp rather than `dry.mul(0.6).add(0.4)`: the same
    // value to the bit (0.4 + 0.6*dry), one emitted op cheaper, and that op is
    // what let the appearance block below keep its bedding term.
    const dryFan = s.blend(0.4, 1.0, dry);
    const highs = alt.smoothstep(60, 200);
    const fans = apron.mul(invRock).mul(dryFan).mul(highs);
    // The tree line: above 300-470 m even gentle ground is stone, not turf.
    // This band does double duty. It is what makes the strip between the last
    // grass and the snow line read as alpine rubble, and it is a rung-
    // INDEPENDENT handle: `slope` comes from MESH normals, and a 2 m-voxel
    // rung-0 sector reads visibly gentler than the 0.5 m-voxel rung-2 build of
    // the same face, so a purely slope-driven classifier drifts toward turf with
    // distance. Altitude does not flatten with the LOD ladder, so the far field
    // stays stone because it is HIGH even when its normals say it is gentle.
    // (s.fieldSlope / s.fieldCurvature are the other rung-independent handles
    // now available — swapping the classifier onto them is a retuning pass with
    // visual acceptance, deliberately not part of this migration.)
    const altHigh = alt.smoothstep(300, 470);
    // "Neither wall nor snowfield" — hoisted because the wetness lane at the
    // bottom of the tape gates on exactly the same pair, and a shared register
    // is one op instead of two.
    const bare = invRock.mul(invSnow);
    const stony = bare.mul(altHigh);
    const scree = fans.max(stony);

    // --- alpine ground: turf, and only where turf grows ----------------------
    // Positively defined rather than "whatever is left": gentle (full below
    // ~20 deg, gone by ~35 deg), below the snow, below the stony belt. Anything
    // steeper or higher belongs to rock/scree, and an all-zero sample falls back
    // to this material anyway (it is tape material 0).
    const gentle = up.smoothstep(0.82, 0.94);
    // altLow is hoisted because the wetness lane below gates on it too — one
    // oneMinus, two readers, no extra op.
    const altLow = altHigh.oneMinus();
    const ground = gentle.mul(invSnow).mul(altLow);

    // --- alpine meadow: green clumps where turf actually gets water ----------
    // A fifth CLASS, not a green tint: the user reads clumps, and a clump is a
    // material that WINS, not a hue applied to stone. Three factors, two of them
    // free (the registers already exist):
    //   ground — gentle, unsnowed, sub-treeline. Meadow is a subset of turf by
    //            construction, so slope/altitude/snow gating comes for nothing.
    //   moist  — `patch` through an INVERTED smoothstep, which is the same
    //            260/130/65 m fbm the scree fans read (it carries field()'s old
    //            moisture seeds), so this is literally the world's moisture
    //            prediction with the sign that says wet: fans stay stony, the
    //            ground between them greens up. The edges are deliberately
    //            generous — a first pass used the `dry` register's complement
    //            and topped out near 0.4, which meant meadow never actually WON
    //            anywhere and every clump came out as a faint green wash over
    //            dirt. A displacing class has to reach 1 or it does nothing.
    //   clump  — 9 m 3D world noise through a TIGHT smoothstep. The narrow
    //            (-0.10, 0.10) window is what turns a wash into clumps: most of
    //            the noise range saturates at one end or the other, so clump
    //            interiors are solid 1 and only the ragged 9 m-scale boundary is
    //            partial. Those edges resolve per texel, which is the whole
    //            point of the texel-rate tape — at vertex rate this mask would
    //            be a smear a metre wide.
    const clump = s.noise3World(seed ^ 0x2A, 1 / 9, 2, 0.5, 2.0);
    const moist = patch.smoothstep(0.35, -0.15);
    const meadow = ground.mul(moist).mul(clump.smoothstep(-0.10, 0.10));
    // DISPLACEMENT, the same idiom rock/snow/scree use on each other: turf is
    // multiplied by the meadow's complement, so a clump core is meadow at weight
    // 1 against ground at 0 — the height blend's w=1 identity means those texels
    // composite as PURE meadow, with the blend confined to the clump edge where
    // grass and stone actually interpenetrate. Without this the two would simply
    // average and every clump would read as green-tinted dirt.
    const invMeadow = meadow.oneMinus();

    s.weight(ALPINE_GROUND, ground.mul(invMeadow));
    s.weight(ALPINE_ROCK, rock);
    s.weight(SCREE, scree);
    s.weight(ALPINE_SNOW, snow);
    s.weight(ALPINE_MEADOW, meadow);

    // -------------------------------------------------------------------------
    // APPEARANCE LANES (texel-tape spec section 5). These do NOT classify: they
    // modulate the COMPOSITED texel after the top-2 height blend, so nothing
    // below can move a rock/scree/snow boundary. They exist because the four
    // materials above are flat scalar albedos until the Alpine* detail scenes
    // land — a 600 m wall of one gray is the single biggest "this is a computer"
    // tell in the frame, and strata plus a slow mineral drift fix it in the tape
    // rather than waiting on a tileset.
    // -------------------------------------------------------------------------

    // Three 3D world-noise fields carry everything below. 3D and world-frame on
    // purpose: 2D (x, z) noise is CONSTANT down a vertical face, which is the
    // one surface in an alpine range that most needs breaking up.
    //   N — 5 octaves at 150/68/31/14/6.4 m (gain 0.6, so the fine octaves keep
    //       real amplitude). One op buys the whole scale ladder the eye reads as
    //       "rock": massif-scale value drift, face-scale patches, metre-scale
    //       grain. It drives BOTH the albedo value and the bedding warp, which
    //       is right — the same body of rock that is paler is the one whose beds
    //       bulge.
    //   F — RIDGED at 4.5 m, 2 octaves. Ridged fbm peaks along thin connected
    //       crests, which is exactly the topology of a joint set; thresholded
    //       high it is a fracture network and not marbling.
    //   H — 3 octaves at 40/20/10 m: the MINERAL field. Independent of N so a
    //       pale face is not automatically a warm face; this is what makes one
    //       buttress read blue-gray and the next one tan. It doubles as the
    //       wetness lane's seep pattern at the bottom of the block.
    // Two more noise fields were authored and cut for register space — a second
    // 8 m crack scale and a 0.5 m granular speckle; see the budget note at the
    // bottom of this method.
    const N = s.noise3World(seed ^ 0xC4, 1 / 150, 5, 0.6, 2.2);
    const F = s.ridge3World(seed ^ 0xF2, 1 / 4.5, 2, 0.5, 2.0);
    const H = s.noise3World(seed ^ 0xD7, 1 / 40, 3, 0.5, 2.0);

    // --- base value: macro drift ----------------------------------------------
    // +/-32% of value at the extremes, typically +/-11%. Written as 1 - (-0.32*N)
    // because oneMinus is one op where a `const 1` plus an `add` is two, and
    // this block lives inside the 64-op tape cap with nothing to spare.
    const value = N.mul(-0.32).oneMinus();

    // --- fracture cracks ------------------------------------------------------
    // The crack threshold sits high on the ridge so only the crests survive:
    // thin connected lines, not broad veining. The first pass at (0.50, 0.76)
    // SATURATED over most of the wall — ridged fbm spends a lot of its range
    // near the top — and the result read as a dense crazed network rather than
    // as fractures. (0.62, 0.86) puts coverage near 8%.
    const hairline = F.smoothstep(0.62, 0.86);
    // Gated by `rock` — the same register the ALPINE_ROCK weight uses — so the
    // fractures live on the walls and stop dead at the talus, the turf and the
    // snowfields instead of scoring the whole world.
    const lines = hairline.mul(rock);
    // 0.86 in the line cores. An earlier pass ran 0.6 with wide bands and read
    // as painted corduroy at overview range; the fix was both narrower masks and
    // a much lighter hand. Pull it lighter still once AlpineRockDetail lands and
    // the rock has real surface texture of its own to carry.
    const V = s.blend(value, value.mul(0.86), lines);

    // --- mineral hue: one chroma axis, R against B ----------------------------
    // The 0.32 scale is SHARED with the value drift above (one `const -0.32`
    // register, so both got bolder for free when a first pass at 0.20 read as
    // monochrome). R and B are pushed in OPPOSITE directions while G holds the
    // lane is a pure warm/cool axis that never changes brightness: h > 0 is
    // oxide tan, h < 0 is cool blue-gray, h = 0 is the neutral base albedo. With
    // AlpineRock's albedo now neutral (see the material block up top) the swing
    // is symmetric — the same lane over the old warm-gray base could only ever
    // make tan more tan, which is exactly how it read.
    // Two sources ride the one axis:
    // Driven by H alone. An oxide-staining term rode the same axis for a while
    // (`H.sub(hairline)`, so fractures came out +0.32 warm — rust weeps out of
    // joints on every real alpine wall, and the crack mask was already in a
    // register), but its one `sub` is what paid for the sparse metallic mask
    // above when that lane turned out to need two ops instead of one. Rust is
    // one op away whenever the cap moves.
    const h = H.mul(-0.32);
    s.tint(V.add(h), V, V.sub(h));

    // --- metallic: ore flecks in the fractures --------------------------------
    // Metalness is an ENDPOINT channel — a texel is metal or it is not — so it
    // wants a sharp, SPARSE mask. Thresholded off the RAW ridge field rather
    // than off `hairline`: `hairline` saturates at 1 across the whole crack
    // network, so any threshold applied to it selects the entire network, and
    // the first attempt at that turned roughly a fifth of every wall into black
    // metal (a metal texel has no diffuse, and with nothing bright to reflect it
    // renders black — the failure mode is loud and total, so this mask earns its
    // second op). F above 0.90 is the innermost core of a fracture only, near 1%
    // of rock area, which reads as dark ore specks in the joints.
    s.metallic(F.smoothstep(0.90, 0.99).mul(rock));

    // --- gully wetness: damp drainage, and damp in PATCHES ---------------------
    // fieldCurvature is metres of height deficit against the 4-neighbour ring at
    // `radius`, so it marks the concave lines water actually collects in. It is
    // a FIELD query, evaluated per vertex and interpolated (spec section 4.3),
    // which is right: a drainage line is smooth at metre scale. It is the tape's
    // only field lane (cap 8).
    // RADIUS 8, not the spec sketch's 4: the deficit of a valley of depth d and
    // half-width W scales as d*r^2/(2*W^2), so a 30 m-wide, 8 m-deep drainage
    // reads ~0.1 m at r=4 and ~0.4 m at r=8. Measured by mean frame luminance in
    // an A/B, r=4 with the sketch's 0.5-2.5 m edges moved the whole frame by 0.7
    // of a grey level — nothing — and what little it caught was the field's 6 m
    // ridged surface octave rather than any watercourse.
    // The VARIATION is in the edges, not in a second mask. (0.25, 2.00) m spans
    // most of the curvature the terrain actually produces, so wetness comes out
    // as a continuous field — 0.2 in a shallow swale, 0.5 in a proper gully,
    // saturating only in the few hollows deep enough to be a streambed — instead
    // of a binary damp/dry stamp. That doubles as the peak limiter: the lane's
    // full 1.0 is standing water and almost nothing reaches it.
    // A separate 40 m `seep` mask (H remapped, so a gully seeps where the rock
    // is right and runs dry between) was authored and cut for two ops when the
    // meadow class landed; it is the next thing back after the joints.
    // `bare` keeps it off the walls, which shed rather than pool, and out of the
    // snow, which does not gloss.
    const gully = s.fieldCurvature(8).smoothstep(0.25, 2.00);
    s.wetness(gully.mul(bare));

    // --- what the 64-op cap cost ----------------------------------------------
    // The tape is at exactly 64 emitted ops (the classifier's 39 plus 25 here).
    // Three authored terms were written, seen working on screen, and cut for
    // register space — in the order they should come back if the cap moves:
    //   MAJOR JOINTS — a second `s.ridge3World(seed ^ 0xF9, 1/8, 2, 0.5, 2.0)`
    //     thresholded at (0.54, 0.82) and MAXed into `lines`, so the fracture
    //     network has a sparse heavy set with hairlines hanging off it instead
    //     of one uniform scale. Three ops.
    //   GRANULAR SPECKLE — `s.noise3World(seed ^ 0xE3, 1/0.5, 2, 0.5, 2.0)` at
    //     +/-5.5% of value, folded into the drift accumulator. Three ops. Ranked
    //     last of the three because AlpineRockDetail's tileset supersedes it
    //     outright — it was only ever a bridge to real cm-scale grain.
    //   ROUGHNESS BIAS — the roughbias lane is still unused here; bedded and
    //     fractured rock should read rougher than the faces between, and
    //     `lines` is already the register that would drive it. Two ops.
  }

  biomes() {
    return {
      __terrain: { material: "dirt" },
      foothills: { grass: 1400, pebbles: 90, rocks: 16 /*, trees: 5 */ },
      meadow:    { grass: 1400, pebbles: 90, rocks: 16 /*, trees: 5 */ },
      mountains: { rocks: 4 },
      ocean:     {},
    };
  }
}
