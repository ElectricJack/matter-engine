// StreamMountain — colossal alpine streaming terrain.
//
// Broad warped massifs establish whole ranges, ridged noise forms the alpine
// profile, and elevation masks keep lower slopes calmer while carving the
// middle elevations and sharpening the summits. Fine 6/3/1.5 m ridged detail
// roughens upper slopes without adding noise to the valley floor.
//
// The range is then PLANED by a glacial-trough remap at the end of field()
// (2026-07-30): broad flat valley floors under a smooth concave ramp, with
// everything above the trimline left bit-identical. See the block comment there.
//
// APPEARANCE (chart-VT spec Phase 4) comes from the five materials declared
// below plus the surfaces() classifier tape at the bottom of this file:
// green-brown valley floors, meadow clumps on the gentle moist ground, gray
// walls on the steep faces, scree fans on the mid-steep aprons under those
// walls, and a ragged snow line across the 450-650 m crests. The geometry band
// table in WorldSector.js is untouched — terrain still meshes as one all-dirt
// bucket; only texturing is classified.

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
// existing four keep their slots and their meaning. AlpineMeadowDetail now
// bakes and loads (slot 4), so the clumps carry real multi-green tufts rather
// than the flat green this rendered as when the material was first declared.
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

  // 2026-07-31 (issue 80c66789): the volumetrics fog multipliers were deleted
  // and folded into these authored values, so the numbers here are now the
  // ones the shader actually sees. What changed and why it looks the same:
  //   density    0.180  * fogDensityMul 0.03  = 0.0054
  //   maxHeight  140 + (165 - 140) * fogFalloffMul 3.44 = 226
  // (fogFloorOffset was 0). The deck was never 25 m thick — the 3.44
  // multiplier made it 86 m and nothing in the file said so, which is exactly
  // the confusion the de-duplication removes.
  static fog = {
    density:    0.0054,
    minHeight:  140.0,
    maxHeight:  226.0,
    noiseScale: 0.00022,
    color:     [0.90, 0.92, 0.95],
    wind:      [0.12, 0.0, 0.04],
  };

  // 2026-07-29 alpine tuning pass: with the heightfield terrain-LOD ladder,
  // distant sectors cost a handful of triangles, so both the scatter rings
  // and the terrain bands reach much farther than the old full-detail-only
  // streamer could afford (which OOMed at 4800 m of voxel sectors).
  //
  // Rings pulled in on 2026-07-30 (was 368/1115/2922). Scatter is the
  // INSTANCE cost, not the triangle cost, and it does not get the ladder's
  // discount with distance the way terrain does — a 2922 m tier-0 ring is
  // millions of resolved instances for grass nobody can resolve. 150/500/1000
  // keeps dense scatter where it reads and stops it at 1 km; terrain keeps
  // going to 10095 on the heightfield rungs, which is where the sightlines
  // actually come from.
  static streaming = {
    // THE RINGS BOUND RESIDENCY, not just scatter density. A sector past the
    // OUTERMOST ring gets desired_rung -1 from desired_rung_for_dist, and
    // sector_streamer skips it entirely -- it is never requested, never baked,
    // never resident. So the terrain bands below could name radii out to
    // 10 km and nothing beyond the last ring here would ever be built; the
    // world simply stopped about 1 km out (plus hysteresis) no matter what
    // the bands or the resolver's activation radius said.
    //
    // The outer ring now reaches the last terrain band so the two agree. Rung
    // 0 is the cheapest scatter tier (landmark boulders and trees only), so
    // extending it costs residency bookkeeping and terrain, not dense scatter
    // -- the dense tiers still stop at 150 m and 500 m exactly as before.
    rings: [
      { radius: 150.0, rung: 2 },
      { radius: 500.0, rung: 1 },
      { radius: 10095.0, rung: 0 },
    ],
    // Terrain LOD bands (radius -> LOD). ALL VOXEL as of the all-voxel ladder:
    // LOD 5 meshes at 2 m voxels and each step down doubles the voxel, to 64 m
    // at LOD 0. These used to select a heightfield representation for 4..0,
    // which is what put a seam between near and far terrain.
    // Hand-tuned in the editor's LOD Settings window.
    //
    // Retuned 2026-07-30 (was 961/1486/2120/2862/5943/10095). The native-voxel
    // band pulls in hard, 961 -> 318, and everything from LOD 3 out pushes
    // further away: the expensive rung is the one that has to be small, and
    // once it is, the cheap rungs can afford to cover far more ground. The
    // outer band is unchanged at 10095 and still sets the far plane.
    terrainBands: [
      { radius: 318.0, lod: 5 },
      { radius: 1186.0, lod: 4 },
      { radius: 2605.0, lod: 3 },
      { radius: 4702.0, lod: 2 },
      { radius: 7753.0, lod: 1 },
      { radius: 10095.0, lod: 0 },
    ],
  };

  // Editor volumetrics defaults for this world (adopted into the live
  // volumetrics controls on world load) — how the froxel volume is MARCHED.
  // What is in it is `static fog` above.
  //
  // ON by default as of 2026-07-30, reversing the 2026-07-29 opt-in default:
  // the aerial perspective is what gives the range its depth, so the world
  // should load looking like this rather than needing a checkbox first.
  // phaseG nudged 0.30 -> 0.34 for slightly tighter forward scattering around
  // the sun. The fogDensityMul/fogFalloffMul that used to live here folded
  // into `static fog` on 2026-07-31 — see the note there.
  static volumetrics = {
    enabled: true,
    phaseG: 0.34,
    temporalBlend: 0.85,
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

    // The FLUVIAL surface — everything above this line is the V-profile range
    // the world had through round 1. The glacier goes over it next.
    const raw = baseHeight
      .add(crags)
      .add(surfaceRoughness)
      .add(channels)
      .add(valleyFloor);

    // ---- GLACIAL TROUGH (2026-07-30) ----------------------------------------
    // The range above has no flat ground anywhere. Probed over a 6 km box at
    // 40 m spacing, the median |grad h| was 1.12 (48 deg) and even the sub-60 m
    // "valley floors" ran a 29 deg median — 11.6% of the world passed the
    // classifier's `gentle` gate, which is why meadow and turf never had
    // anywhere to live and every camera pointed at rock. That is what a purely
    // fluvial heightfield looks like: V-shaped notches all the way down.
    //
    // A glacier does not carve a new landscape, it PLANES the one that is
    // there — it grinds the valley floors flat and steepens the walls above
    // them, and it stops at the trimline, leaving the aretes and summits it
    // never reached untouched. So this is a REMAP of `raw`, not a new field:
    //
    //   out = lerp(floor(raw), raw, smoothstep(110, 380, baseHeight))
    //   floor(raw) = 30 + 0.20 * (raw - 30)
    //
    // Keying the blend on ALTITUDE (not on slope, not on a mask) is what makes
    // the trimline behave. Where the key saturates at 1 the field is
    // BIT-IDENTICAL to the pre-glacial one: every summit, arete and cliff over
    // the trimline survives exactly as authored, which matters because the snow
    // line (400-520 m) and the stony belt (300-470 m) are altitude-keyed and
    // would otherwise all have to be retuned.
    //
    // The key is `baseHeight` — the SMOOTH massif surface — and not the full
    // `raw`, and that distinction was worth a bake to learn. Keyed on `raw`, an
    // isolated crag sitting in the middle of a flattened floor keys its OWN
    // ramp: it climbs out of the compression while the ground around it stays
    // planed, and prints as a lone pyramid on a plain. The first bake of this
    // remap did exactly that, and the render showed a floor stubbled with white
    // cones. Slope quantiles missed it entirely (the pinnacle COUNT is
    // unchanged either way, ~5% of samples); what caught it was measuring
    // PROMINENCE — a local maximum's height over its own 40 m ring:
    //     fluvial baseline   0.67% of samples > 15 m, p90 prominence 17.4 m
    //     keyed on raw       0.83%                    p90 20.6 m   <- pyramids
    //     keyed on baseHeight 0.61%                   p90 18.5 m
    // Keyed on the smooth base, a crag on the floor is compressed WITH the
    // floor instead of lifting out of it, so the world ends up with FEWER
    // freestanding spikes than the fluvial terrain it replaced, while the floor
    // (45% of area at a 10.8 deg median) and the gentle-gate coverage (37.5%)
    // are within a point of the raw-keyed version. Steep-face coverage drops
    // too: samples over 68 deg go 18.2% -> 14.8% against a 12.3% baseline.
    // It is also the right model — a trimline is a smooth line drawn on a
    // massif by an ice surface, not a contour that detours around every crag.
    //
    // Below 110 m the floor compresses relief 5:1 about a 30 m pivot. 0.20 was
    // chosen by measurement, not taste: `primary` is a 5-octave ridged field
    // whose finest octave is 59 m, and ridged fbm has CUSPS, so the floor's
    // residual roughness is set by how far that octave is scaled down. At 0.28
    // the floor still ran a 13 deg median; at 0.20 it runs 10.9 deg with 75% of
    // it passing the gentle gate; below 0.20 the floor stops improving (0.14
    // gave 8.8 deg) because the extra flattening only drags more marginal
    // terrain into the floor band. 0.20 is the knee.
    //
    // The ramp between is the U. The remap's derivative runs 0.20 on the floor,
    // crosses 1.0 near raw 195, peaks at 1.79 near raw 300 and returns to
    // exactly 1.0 at the trimline (C1 both ends — a discontinuity here would
    // print as a terrace right around the world). Slopes just under the
    // trimline therefore come out ~1.8x steeper than they were, which is the
    // point: a trough wall IS steeper than the fluvial slope it replaces.
    // Probed transect at z=-400: floor at 41-46 m holding 10-20 deg out to
    // x=-450, then 340 m of wall in 210 m of run.
    //
    // Whole-world effect (6 km box, 40 m sampling): the `gentle` gate goes
    // 11.6% -> 37.5%, median slope 48 deg -> ~35 deg, and p90 height is
    // unchanged at 495 m. Flat where it should be flat, untouched up top.
    const trough = baseHeight.smoothstep(110, 380);
    const floorH = raw.mul(0.20).add(24.0);         // == 30 + 0.20*(raw-30)
    // Gentle rolling ON the floor only: +/-6.5 m at 220/110 m. The floor keeps
    // 20% of the original valleyFloor/broadNoise terms (about +/-2 and +/-5 m
    // at 320 and 520 m), so this fills in the 150-300 m band between them and
    // stops the trough bottom reading as a milled plane. Attenuated by the
    // trough mask so it never perturbs the walls.
    const rolling = noise2(p.worldSeed ^ 0xB3, 1/220, 2).mul(6.5);
    const height = blend(floorH.add(rolling.mul(trough.oneMinus())), raw, trough);

    // Emit biome controls after height so height_at() does not evaluate them.
    //
    // RETIRED 2026-07-30. These were two noise channels the tape used to read
    // back as s.moisture / s.relief before s.noise2World existed. The tape has
    // sampled the same fbm directly since round 1, so they carried no appearance
    // signal; they were kept byte-identical only because editing them re-hashes
    // the field program and re-bakes every sector, and until this pass there was
    // no other reason to pay that. This pass re-bakes the world anyway, so they
    // go now, per their own note.
    // They are replaced by a CONSTANT rather than deleted, because scatter still
    // reads them: `biomeThresholds` above sets both cutoffs to a deliberately
    // unreachable 2.0, and 0 is inside [-1, 1] exactly as the noise was, so every
    // sector keeps classifying as `foothills` precisely as before. `blend` of
    // three literal zeroes is the DSL's cheapest constant node — one dedup'd
    // `const 0` plus one `blend`, two ops for both channels.
    const inert = blend(0.0, 0.0, 0.0);
    return {
      density: heightToDensity(height),
      moisture: inert,
      relief: inert,
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
  // 96 EMITTED ops and this one uses 90 — see the budget block at the end of
  // the method. The parser dedups identical consts, which is why reusing a
  // literal that is already somewhere in the tape is free; oneMinus is a native
  // single op, so no hoisting tricks are needed.)
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

    // Four 3D world-noise fields carry everything below. 3D and world-frame on
    // purpose: 2D (x, z) noise is CONSTANT down a vertical face, which is the
    // one surface in an alpine range that most needs breaking up.
    //   N — 5 octaves at 150/68/31/14/6.4 m (gain 0.6, so the fine octaves keep
    //       real amplitude). One op buys the whole scale ladder the eye reads as
    //       "rock": massif-scale value drift, face-scale patches, metre-scale
    //       grain.
    //   F — RIDGED at 4.5 m, 2 octaves. Ridged fbm peaks along thin connected
    //       crests, which is exactly the topology of a joint set; thresholded
    //       high it is a fracture network and not marbling. This is the HAIRLINE
    //       set — the fine craquelure between the real joints.
    //   J — RIDGED at 8 m, 2 octaves (round 2). The MAJOR joint set: same
    //       topology an octave coarser, thresholded lower so each line is wider
    //       and the network is sparser. Restored from the round-1 cut list —
    //       one uniform crack scale is the tell that a fracture network is
    //       procedural, because real rock fractures hierarchically.
    //   H — 3 octaves at 40/20/10 m: the MINERAL field. Independent of N so a
    //       pale face is not automatically a warm face; this is what makes one
    //       buttress read blue-gray and the next one tan. It does triple duty:
    //       the hue axis, the strata bedding warp, and the wetness lane's seep
    //       pattern at the bottom of the block.
    // The 0.5 m GRANULAR SPECKLE from the round-1 cut list was deliberately NOT
    // restored. It was only ever a stand-in for cm-scale grain, and
    // AlpineRockDetail now bakes and loads, so the rock carries real grain from
    // the tileset; a 0.5 m tape term on top of it would be redundant at close
    // range and pure aliasing at every other range. Its three ops went to the
    // strata block instead.
    const N = s.noise3World(seed ^ 0xC4, 1 / 150, 5, 0.6, 2.2);
    const F = s.ridge3World(seed ^ 0xF2, 1 / 4.5, 2, 0.5, 2.0);
    const J = s.ridge3World(seed ^ 0xF9, 1 / 8, 2, 0.5, 2.0);
    const H = s.noise3World(seed ^ 0xD7, 1 / 40, 3, 0.5, 2.0);

    // --- base value: macro drift ----------------------------------------------
    // +/-32% of value at the extremes, typically +/-11%. Written as 1 - (-0.32*N)
    // because oneMinus is one op where a `const 1` plus an `add` is two.
    const value = N.mul(-0.32).oneMinus();

    // --- strata: MILD bedding, per the round-1 failure analysis ----------------
    // Round 1 shipped a bold version and the user called it too strong; the two
    // failure modes it taught are encoded directly in these numbers.
    //   WARP AMPLITUDE MUST BE WELL UNDER THE BED PERIOD. At warp >= period the
    //   bands stop being horizontal at all and the whole idea collapses into
    //   marbling. Here the warp is +/-3 m against an 11 m period (27%), which
    //   bends the beds with the massif — bedding planes really do dip — without
    //   ever letting a band cross its neighbour. The warp rides H rather than a
    //   fifth noise field: at 40/20/10 m it is exactly the scale a bedding dip
    //   varies over, and it costs zero noise ops.
    //   THE SEAM MUST BE NARROW. A mask covering half the bed reads as corduroy
    //   at overview range, which is what round 1 looked like from the A camera.
    //   `band` is the 0..1 sawtooth of altitude / 11 m; (band - 0.5).abs() folds
    //   it into a 0.5-at-the-seam triangle wave that is CONTINUOUS across the
    //   wrap (0.999 and 0.001 both map to 0.499 — a one-sided threshold on the
    //   raw sawtooth would tear there and alias). The (0.43, 0.49) window means
    //   the seam ramps over 14% of the period (~1.5 m) and saturates only in the
    //   innermost 2% (~0.2 m): a thin dark line every 11 m, not a striped wall.
    const band = alt.add(H.mul(3)).mul(1 / 11).fract();
    const seam = band.sub(0.5).abs().smoothstep(0.43, 0.49);

    // --- the darkening mask: three scales of relief, one blend ------------------
    // hairline: threshold sits high on the ridge so only the crests survive —
    //   thin connected lines, not broad veining. A first pass at (0.50, 0.76)
    //   SATURATED over most of the wall (ridged fbm spends a lot of its range
    //   near the top) and read as crazed glaze rather than as fractures.
    //   (0.62, 0.86) puts coverage near 8%.
    // majorJ: the same construction one octave coarser and thresholded lower, so
    //   the major joints are wider and sparser than the hairlines that hang off
    //   them. Real rock breaks hierarchically and the two scales must not be
    //   equally dark, hence the weights below.
    // The three are MAXed rather than added — overlapping crack sets must not
    // compound into a black smear where they cross — and each carries its own
    // ceiling, which is the entire "mild" control: major joints reach 1.0,
    // hairlines top out at 0.4, bedding seams at 0.45. One `blend` then converts
    // the whole thing to value: a full joint darkens 28%, a hairline 11%, a
    // bedding seam 13%.
    const hairline = F.smoothstep(0.62, 0.86);
    const majorJ = J.smoothstep(0.58, 0.86);
    const relief = hairline.mul(0.4).max(majorJ).max(seam.mul(0.45));
    // Gated by `rock` — the same register the ALPINE_ROCK weight uses — so every
    // one of these terms lives on the walls and stops dead at the talus, the turf
    // and the snowfields instead of scoring the whole world. One gate, one op,
    // three masks. `lines` is then reused four more times below (hue, roughness),
    // which is why it is worth building as a single combined register.
    const lines = relief.mul(rock);
    const V = s.blend(value, value.mul(0.72), lines);

    // --- mineral hue + oxide staining: one chroma axis, R against B ------------
    // The 0.32 scale is SHARED with the value drift above (one `const -0.32`
    // register). R and B are pushed in OPPOSITE directions while G holds, so the
    // lane is a pure warm/cool axis that never changes brightness: h > 0 is
    // oxide tan, h < 0 is cool blue-gray, h = 0 is the neutral base albedo. With
    // AlpineRock's albedo neutral (see the material block up top) the swing is
    // symmetric — the same lane over the old warm-gray base could only ever make
    // tan more tan, which is exactly how it read.
    // Two sources ride the one axis, and the second is the round-1 cut list's
    // RUST STAINING, restored:
    //   H       — the mineral field: which buttress is warm and which is cool.
    //   lines   — subtracted, so anything the darkening mask marks also comes out
    //             WARM. Rust weeps out of joints and along bedding planes on
    //             every real alpine wall, and it weeps hardest from the major
    //             joints (which is what the mask's own weighting already says).
    //             A full joint gets +0.27 of warm oxide on top of its 28%
    //             darkening; a bedding seam +0.12. Free direction, one `sub`.
    // Subtraction rather than a second lane keeps this at three ops and keeps the
    // guarantee that the tint never changes luminance.
    const h = H.sub(lines.mul(0.85)).mul(-0.32);
    s.tint(V.add(h), V, V.sub(h));

    // --- roughness: weathered relief is rougher than the faces between ----------
    // The last of the three round-1 cuts. `lines` is already the register that
    // wants it: a joint, a hairline and a bedding seam are all places where the
    // rock has broken and weathered, and broken rock scatters. +0.18 at a full
    // joint against the material's own 0.95 base clamps out near 1.0, which is
    // the point — the joints go matte while the unbroken faces keep a faint
    // sheen, and that difference is what makes the fracture network read as
    // GEOMETRY under a moving sun rather than as a painted line.
    s.roughnessBias(lines.mul(0.18));

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
    // SEEP (round-1 cut list, restored): a gully is not uniformly damp along its
    // length — it seeps where the rock above it is right and runs dry between,
    // which is why a drainage line reads as a chain of dark patches rather than
    // as a painted stripe. `H` supplies the pattern for free (it is already in a
    // register for the hue axis, and at 40/20/10 m it breaks a gully into
    // patches every few tens of metres, which is the real scale of a seep).
    // Written as a lerp to 0.35 rather than a plain multiply: a dry stretch of
    // gully is still a gully floor and still darker than its banks, so the lane
    // varies between about a third and full strength instead of switching off.
    // The correlation with the hue axis is a bonus rather than a compromise —
    // H > 0 is both the cool blue-gray side of the mineral lane AND the seeping
    // side here, and wet rock really does read cooler.
    // `bare` keeps it off the walls, which shed rather than pool, and out of the
    // snow, which does not gloss.
    const gully = s.fieldCurvature(8).smoothstep(0.25, 2.00);
    const seep = s.blend(0.35, 1.0, H.smoothstep(-0.30, 0.25));
    s.wetness(gully.mul(bare).mul(seep));

    // --- budget ----------------------------------------------------------------
    // 90 of the 96 emitted ops (the cap was raised 64 -> 96 after round 1 proved
    // 64 binding). Round 2 spent the 26-op headroom on the whole round-1 cut
    // list except the granular speckle, which the AlpineRockDetail tileset made
    // redundant (see the noise-field block above), plus mild strata:
    //   strata seam  10 ops    mask combine  5 ops    gully seep    4 ops
    //   rust stain    3 ops    major joints  2 ops    roughness     2 ops
    // The 6 remaining ops are deliberately unspent: a second `fieldCurvature`
    // radius (a second field lane, cap 8) is the obvious next term — a wide
    // radius-24 probe would separate "valley" from "gully" and let the meadow
    // classifier prefer real basins over every incidental swale.
  }

  biomes() {
    return {
      __terrain: { material: "dirt" },
      __vegetation: { profile: "alpine-lush" },
      foothills: { pebbles: 90, rocks: 16 /*, trees: 5 */ },
      meadow:    { pebbles: 90, rocks: 16 /*, trees: 5 */ },
      mountains: { rocks: 4 },
      ocean:     {},
    };
  }
}
