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
// APPEARANCE comes from the continuous world-space material in
// shared-lib/mountain_surface.js:
// green-brown valley floors, meadow clumps on the gentle moist ground, gray
// walls on the steep faces, scree fans on the mid-steep aprons under those
// walls, and a ragged snow line across the 450-650 m crests. The geometry band
// table in WorldSector.js is untouched — terrain still meshes as one all-dirt
// bucket; only texturing is classified.

// Terrain is one continuous GPU field material written into chart VT pages.
// The five palette handles keep existing registry IDs stable for forest assets;
// no terrain material requests a repeated geometry-baked detail atlas.
const ALPINE_GROUND = defineMaterial("AlpineGround", {
  albedo: [0.125, 0.116, 0.087], roughness: 0.94,
});
const ALPINE_ROCK = defineMaterial("AlpineRock", {
  albedo: [0.235, 0.230, 0.221], roughness: 0.83,
});
const SCREE = defineMaterial("Scree", {
  albedo: [0.207, 0.200, 0.186], roughness: 0.94,
});
const ALPINE_SNOW = defineMaterial("AlpineSnow", {
  albedo: [0.78, 0.80, 0.82], roughness: 0.80,
});
const ALPINE_MEADOW = defineMaterial("AlpineMeadow", {
  albedo: [0.105, 0.125, 0.074], roughness: 0.94,
});

import { mountainSurface } from 'shared-lib/mountain_surface';
import { alpineHabitat } from 'shared-lib/alpine_ecology';
import { MOUNTAIN_FOREST_PROFILE } from 'shared-lib/mountain_forest';

// Append new material handles so existing forest bake identities stay stable.
const GEOMETRY_ROCK = defineMaterial('Mountain.GeometryRock', {
  albedo:[0.28,0.265,0.24],roughness:0.88,specularStrength:0.22,
});

// Declare at module scope so the definition loader registers every handle.
const FOREST_MATERIALS = {
  barkMaterial: defineMaterial('mountain.forest.bark', { albedo:[0.24,0.15,0.085], roughness:0.94,
    specularStrength:0.25, detail:'ConiferBarkDetail', detailMode:'surface' }),
  branchMaterial: defineMaterial('mountain.forest.branch', { albedo:[0.24,0.145,0.075], roughness:0.91,
    specularStrength:0.2, detail:'ConiferBranchDetail', detailMode:'surface' }),
  needleMaterial: defineMaterial('mountain.forest.needles', { albedo:[0.065,0.18,0.085], roughness:0.72,
    specularStrength:0.22, subsurface:0.12, scatteringColor:[0.13,0.30,0.065],
    scatteringDistance:0.001, thinWalled:true, doubleSided:true }),
  coneMaterial: defineMaterial('mountain.forest.cone', { albedo:[0.30,0.18,0.085], roughness:0.92 }),
  redwoodMaterial: defineMaterial('mountain.forest.redwood', { albedo:[0.30,0.13,0.055], roughness:0.97,
    specularStrength:0.15, detail:'RedwoodBarkDetail', detailMode:'surface' }),
};


class StreamMountain extends World {
  // geometryRockStress swaps the three-rock detailed inspection site for the
  // 1,283-rock stress benchmark (shared-lib/mountain_geometry_site.js). It is
  // a benchmark profile only; the shipped scene keeps it off.
  static params = { worldSeed: 20260722, terrainOnly: false, geometryRockStress: false };
  static world = { sectorSize: 64, yMin: -96, yMax: 704 };

  static camera = {
    position: [380.0, 90.0, 1600.0],
    target:   [420.0, 55.0, 1420.0],
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
    // Direct VT carries the close material too. The 16K address space lets
    // 64 m sectors retain 128 t/m, including their chart gutters. Physical
    // pages remain demand-driven; nested tiles scale density by their size.
    terrainTexelsPerMeter: 128,
    // NESTED SECTOR LOD (docs/terrain-nested-sector-lod-2026-08-08.md).
    //
    // Level L tiles are 64<<L metres across at a 2<<L voxel, so cells-per-tile
    // is a constant 32x32 and each annulus below holds a near-constant tile
    // count. On the uniform grid this world's authored reach resides 78,161
    // sectors, 78% of them in the two coarsest bands drawing one to four quads
    // apiece -- the whole per-part machine (a JS bake, a publish, a BLAS, a
    // TLAS instance, chart registration, residency bookkeeping) run 32,000
    // times to draw one quad. Nested it is 1,207 tiles for the same triangles
    // and the same reach, and update() costs 0.367 ms instead of 5.67.
    //
    // THE BAND TABLE BELOW IS UNCHANGED. Nesting only reinterprets it: band
    // LOD l is the annulus where level 5-l tiles live. It happens to already
    // satisfy what a nested ladder wants -- every annulus wider than one tile
    // of the next coarser level (318/868/1419/2097/3051/2342 against
    // 128/256/512/1024/2048) -- so the restriction pass has nothing to fix.
    // The narrowest margin is the outermost: 2342 m of annulus against a
    // 2048 m tile. Pulling 7753 outward is what to do if that band ever starts
    // forcing splits.
    nestedSectors: true,

    // VOLUMETRIC SECTORS (volumetric-sectors M3). Tiles are cubes on an octree
    // rather than columns on a quadtree, and yMin/yMax above stop bounding a
    // mesh slab and start bounding the tree: -96..704 is 800 m, so a column at
    // level 0 becomes a stack of up to 13 cubes.
    //
    // MEASURED COST, so it is a choice and not an accident. On the 3D churn
    // soak this world holds ~1900-2850 resident tiles against ~390-520 on
    // columns, and the selection tick goes 0.19 ms -> 1.57 ms. Per-tile cost is
    // only ~1.5x; the rest is tile COUNT, because a heightfield's vertical
    // stack is mostly empty air and solid rock that a column never had to
    // represent as separate tiles. StreamCaverns wins from cubes because its
    // yMin is -1024 and the column path pays ~590 density rows to reach it;
    // this world's 800 m of extent is a much weaker case for them.
    //
    // MATTER_VOLUMETRIC_SECTORS=0 runs the column path without editing this
    // file, which is the A/B and the rollback.
    volumetricSectors: true,

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
    //
    // UNDER NESTING that trap is gone: the outermost BAND bounds residency and
    // these rings only grade scatter, so the 10095 below is now redundant
    // rather than load-bearing. Kept at that value so the world reads the same
    // with the flag off, which is the rollback position.
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

  // Terrain sources are evaluated directly in surfaces(); bark atlases remain
  // independently requested by the forest materials above.
  static roots = [];

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
    // Metre-scale relief is geometry, independent of VT/POM. Keep it below
    // the large landforms and resolve it with the denser source voxel grid.
    const groundRelief = noise2(p.worldSeed ^ 0xC91, 1/9, 2).mul(.65)
      .add(noise2(p.worldSeed ^ 0xD27, 1/4, 1).mul(.22))
      .add(noise2(p.worldSeed ^ 0xD28, 1/1.5, 1).mul(.10))
      .add(noise2(p.worldSeed ^ 0xD29, 1/.75, 1).mul(.035));
    const height = blend(floorH.add(rolling.mul(trough.oneMinus())), raw, trough)
      .add(groundRelief);

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

  // Coherent color/roughness/height in world metres. Appearance edits keep
  // field geometry and habitat placement unchanged; no terrain source settle.
  surfaces(s) {
    s.source(ALPINE_GROUND, mountainSurface(s, StreamMountain.params.worldSeed));
  }

  habitat(h) {
    alpineHabitat(h, StreamMountain.params.worldSeed);
  }

  biomes() {
    return {
      __terrain: { material: "dirt" },
      __terrainOnly: StreamMountain.params.terrainOnly,
      __geometryRocks: {material:GEOMETRY_ROCK},
      __geometryRockStress: StreamMountain.params.geometryRockStress,
      __vegetation: StreamMountain.params.terrainOnly ? undefined :
        { profile: MOUNTAIN_FOREST_PROFILE, materials: FOREST_MATERIALS },
      foothills: { rocks: 16 },
      meadow:    { rocks: 16 },
      mountains: { rocks: 4 },
      ocean:     {},
    };
  }
}
