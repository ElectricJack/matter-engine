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
const ALPINE_ROCK = defineMaterial("AlpineRock", {
  albedo: [0.45, 0.42, 0.38],
  roughness: 0.95,
  detail: "AlpineRockDetail",
});
// Talus aprons under the walls, and stony ground above the green belt.
const SCREE = defineMaterial("Scree", {
  albedo: [0.50, 0.47, 0.43],
  roughness: 1.0,
  detail: "ScreeDetail",
});
// Snowfields on the crests and high benches.
const ALPINE_SNOW = defineMaterial("AlpineSnow", {
  albedo: [0.90, 0.90, 0.95],
  roughness: 0.75,
  detail: "AlpineSnowDetail",
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
    const dryFan = dry.mul(0.6).add(0.4);
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
    const stony = invRock.mul(invSnow).mul(altHigh);
    const scree = fans.max(stony);

    // --- alpine ground: turf, and only where turf grows ----------------------
    // Positively defined rather than "whatever is left": gentle (full below
    // ~20 deg, gone by ~35 deg), below the snow, below the stony belt. Anything
    // steeper or higher belongs to rock/scree, and an all-zero sample falls back
    // to this material anyway (it is tape material 0).
    const gentle = up.smoothstep(0.82, 0.94);
    const ground = gentle.mul(invSnow).mul(altHigh.oneMinus());

    s.weight(ALPINE_GROUND, ground);
    s.weight(ALPINE_ROCK, rock);
    s.weight(SCREE, scree);
    s.weight(ALPINE_SNOW, snow);
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
