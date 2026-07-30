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

  static streaming = {
    // Keep expensive scatter close. The outer ring stops where the height fog
    // fully swallows the silhouettes (~2.5 km in editor captures): the earlier
    // 4800 m ring streamed ~17,600 full-detail sectors, none of them visible
    // past ~3 km, and ran the editor past 45 GB resident (std::bad_alloc).
    // True 5 km vistas need the heightfield terrain-LOD ladder from the
    // 2026-07-28 alpine design doc, not more full-detail sectors.
    rings: [
      { radius: 128.0, rung: 2 },
      { radius: 320.0, rung: 1 },
      { radius: 2560.0, rung: 0 },
    ],
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
    // These two are also the tape's only WORLD-SPACE noise channels, and they
    // are tuned for that job: the surfaces() tape's own noise2/ridge2 sample
    // PART-LOCAL x/z, which on a 64 m sector variant repeats identically in
    // every sector, so anything that must vary across a massif has to arrive
    // through the field. Both stay inside [-1, 1], far below the (deliberately
    // unreachable) 2.0 biome thresholds above, so every sector still
    // classifies as `foothills` for scatter exactly as before.
    //   relief   — 4 octaves at 420/210/105/52 m: breaks the snow line into
    //              tongues and fingers and roughens the rock/scree boundary.
    //   moisture — 3 octaves at 260/130/65 m: separates scree fans from the
    //              grassier aprons between them.
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
  // chart vertex; world inputs (altitude, moisture, relief) are legal because
  // terrain sectors are world-anchored variants — one instance per sector.
  //
  // Weights are NORMALIZED by the runtime and the compositor keeps the top 2,
  // so what matters is each material's weight RELATIVE to its neighbours; the
  // (1 - w) products below are what make one class actually displace another
  // instead of averaging with it. Budget note: the tape is capped at 64 ops
  // (terrain_field kMaxOps) and every literal costs a register, hence the
  // hoisted ONE/ZERO pair and the `inv` helper (one `blend` op per complement
  // instead of the four `oneMinus()` spends).
  // -------------------------------------------------------------------------
  surfaces(s) {
    const ONE = s.value(1);
    const ZERO = s.value(0);
    const inv = (a) => s.blend(ONE, ZERO, a);

    const slope = s.slope;          // 1 - normal.y: 0.13 @30deg, 0.29 @45deg,
    const up = s.normalY;           //               0.50 @60deg, 0.71 @75deg
    const alt = s.altitude;         // world y (sectors are world-anchored)
    const macro = s.relief;         // world fbm, 420 m down to 52 m
    const patch = s.moisture;       // world fbm, 260 m down to 65 m
    const fine = s.noise2(0x5EEDA1, 1 / 17, 2, 0.5, 2.0);   // sector-local

    // --- rock: steep faces, with a ragged rather than iso-slope boundary ----
    // +/-0.055 of slope is about +/-4 deg of wobble on the wall/apron edge, so
    // the wall/talus edge follows the massif instead of tracing an iso-slope.
    const steepN = slope.add(macro.mul(0.055));
    const rock = steepN.smoothstep(0.30, 0.55);             // ~46deg..~63deg
    const invRock = inv(rock);

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
    const invSnow = inv(snow);

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
    // grass and the snow line read as alpine rubble, and it is the tape's only
    // rung-INDEPENDENT handle: `slope` comes from MESH normals, and a 2 m-voxel
    // rung-0 sector reads visibly gentler than the 0.5 m-voxel rung-2 build of
    // the same face, so a purely slope-driven classifier drifts toward turf with
    // distance. Altitude does not flatten with the LOD ladder, so the far field
    // stays stone because it is HIGH even when its normals say it is gentle.
    const altHigh = alt.smoothstep(300, 470);
    const stony = invRock.mul(invSnow).mul(altHigh);
    const scree = fans.max(stony);

    // --- alpine ground: turf, and only where turf grows ----------------------
    // Positively defined rather than "whatever is left": gentle (full below
    // ~20 deg, gone by ~35 deg), below the snow, below the stony belt. Anything
    // steeper or higher belongs to rock/scree, and an all-zero sample falls back
    // to this material anyway (it is tape material 0).
    const gentle = up.smoothstep(0.82, 0.94);
    const ground = gentle.mul(invSnow).mul(inv(altHigh));

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
