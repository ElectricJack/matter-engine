// ChartVtProof — WP-F proof world for the chart-space VT surfaces() tape.
//
// A compact streamed terrain (a few sectors of rolling hills climbing to one
// ~90 m ridge) with THREE materials declared via defineMaterial and a
// surfaces() classifier that assigns them by slope and altitude:
//   - proofGrass on gentle low ground (detail: the ForestFloor tileset, so
//     the height-blend has a real height channel to work with),
//   - proofRock on steep faces at any altitude (the AlpineRockDetail tileset),
//   - proofSnow on gentle ground above the snow line (AlpineSnowDetail).
// Transitions come from the tape's smoothstep weight fields plus the
// compositor's top-2 height blend: grass fills the rock's height-channel
// crevices across the grass/rock band instead of a linear crossfade.
//
// The geometry buckets keep WorldSector's band-table mechanism (all-dirt via
// __terrain, exactly like StreamMountain) — the tape drives APPEARANCE only,
// per the chart-VT spec's Phase 4 non-goal.

const PROOF_GRASS = defineMaterial("proofGrass", {
  albedo: [0.16, 0.42, 0.13],
  roughness: 0.92,
  detail: "ForestFloor",
});
// Step 0 of issue 676ec01c: proofRock and proofSnow used to carry SCALAR
// albedo only, so two of the three classifier bands rendered as flat plastic.
// "Textures blend very poorly" was in significant part that there was nothing
// to blend TO — the grass carpet met untextured grey at a hard boundary. The
// detail scenes for exactly these surfaces already exist (texel-tape work), so
// this costs one line each and two more .gtex bakes on world load.
const PROOF_ROCK = defineMaterial("proofRock", {
  albedo: [0.37, 0.34, 0.31],
  roughness: 0.83,
  detail: "AlpineRockDetail",
});
const PROOF_SNOW = defineMaterial("proofSnow", {
  albedo: [0.91, 0.93, 0.96],
  roughness: 0.35,
  detail: "AlpineSnowDetail",
});

class ChartVtProof extends World {
  static params = { worldSeed: 20260729 };
  static world = { sectorSize: 64, yMin: -32, yMax: 160 };

  static camera = {
    position: [40.0, 120.0, 150.0],
    target: [0.0, 45.0, 0.0],
  };

  // Small footprint: one high-detail ring plus a coarse surround — enough
  // sectors to see the classifier at several distances without streaming a
  // mountain range. (Rungs must descend consecutively across the rings.)
  static streaming = {
    // OCTREE STREAMING (volumetric-sectors M3). Every streamed scene runs cube
    // tiles now; the column path is deprecated and its world-facing route is
    // commented out (part_base.js.h, dsl_bindings.cpp).
    //
    // These scenes were on the UNIFORM grid -- no nesting, so no level ladder
    // for an octree to descend -- which is why they needed `nestedSectors` as
    // well. With no `terrainBands` authored, resolve_terrain_defaults() fills a
    // table scaled off sectorSize (5S/8S/12S/18S/27S/40S), so the ladder each
    // one gets is proportional to its own tile size rather than inherited from
    // a world it has nothing to do with.
    nestedSectors: true,
    volumetricSectors: true,
    rings: [
      { radius: 96.0, rung: 1 },
      { radius: 288.0, rung: 0 },
    ],
  };

  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  static roots = [
    {
      module: "ForestFloor",
      transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1],
      tileset: true,
    },
  ];

  field(p) {
    // One broad ridge through the origin plus rolling foothills. Only the
    // crests clear the 45–65 m snow band; valley floors sit near 4 m, so the
    // spawn view carries all three classifier bands at once.
    const ridgeBase = ridge2(p.worldSeed ^ 0x11, 1/260, 4, 0.55, 2.0);
    const ridge = ridgeBase.add(1).mul(0.5).smoothstep(0.55, 0.97);
    const rolling = noise2(p.worldSeed ^ 0x12, 1/90, 3).mul(9);
    const height = ridge.mul(78).add(rolling).add(4);

    const relief = noise2(p.worldSeed ^ 1, 1/900, 2);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 2);
    return {
      density: heightToDensity(height),
      moisture,
      relief,
      seaLevel: -20.0,
    };
  }

  // The classifier tape (compiled native, evaluated per chart vertex; world
  // inputs are valid because terrain sectors are world-anchored variants).
  surfaces(s) {
    const steep = s.slope.smoothstep(0.30, 0.55);
    const snowLine = s.altitude.smoothstep(45, 65);
    const snow = snowLine.mul(steep.oneMinus());
    const grass = steep.oneMinus().mul(snow.oneMinus());
    s.weight(PROOF_GRASS, grass);
    s.weight(PROOF_ROCK, steep);
    s.weight(PROOF_SNOW, snow);

    // -----------------------------------------------------------------------
    // MACRO VARIATION (issue 676ec01c step 1). Appearance lanes, so these do
    // NOT classify: they modulate the COMPOSITED texel after the top-2 height
    // blend and cannot move a grass/rock/snow boundary.
    //
    // The complaint this answers is "it looks like carpet". Carpet is what a
    // surface reads as when it has high-frequency detail and NOTHING between
    // that detail and its own silhouette: with a 2 m Wang tile and no
    // low-frequency term, a whole hillside averages to one flat colour with
    // grain on top, at every distance. Real ground varies over tens to
    // hundreds of metres — drier crests, damper hollows, mineral changes —
    // and that is the band this fills.
    //
    // Deliberately done in the TAPE and not in a shader. gbuffer.frag and
    // rt_surface_common.glsl would each need their own copy of a procedural
    // macro field, and worlds that already author these lanes (StreamMountain)
    // would then get modulated twice by two systems that cannot see each
    // other. The tape lane is the shipped path: one authored expression,
    // baked into the VT page, and therefore identical in the raster near band
    // (which rides the page through the mean-preserving ratio), the raster far
    // field (pure page) and every RT bounce (pure page). No twin to keep in
    // step, and no rebuild.
    //
    // TWO value fields, not one, and the split is the whole tuning story. A
    // first pass used a single 5-octave 180 m ladder and moved the measured
    // macro-band contrast of a ground crop by 0.0002 — nothing. At this
    // camera 180 m is roughly 1400 screen pixels: almost all of that field's
    // energy sits at scales LARGER than the frame, so it shows up as a
    // uniform shift and not as variation. The band that reads as "this ground
    // is not all one thing" is tens of metres, so it has to be authored there.
    //   M — 4 octaves at 240/120/60/30 m: which hillside is pale and which is
    //       dark. 3D and world-frame on purpose: a 2D (x, z) field is CONSTANT
    //       down a vertical face, and the steep faces are exactly where the
    //       flatness showed worst.
    //   P — 2 octaves at 16/8 m: patch scale. Stops at 8 m rather than running
    //       down to the 2 m tile — the tape is evaluated per PAGE TEXEL, so a
    //       term finer than a coarse page's texel pitch is point-sampled and
    //       shimmers instead of adding detail.
    //   D — 3 octaves at 70/35/17 m, independent seed, driving a pure
    //       warm/cool axis (R up against B down, G held) so it never changes
    //       luminance and can be tuned separately from the value drift.
    const seed = ChartVtProof.params.worldSeed;
    const M = s.noise3World(seed ^ 0xB1, 1 / 240, 4, 0.55, 2.0);
    const P = s.noise3World(seed ^ 0xB3, 1 / 16, 2, 0.5, 2.0);
    const D = s.noise3World(seed ^ 0xB2, 1 / 70, 3, 0.5, 2.0);
    // Value: 1 - (0.20 M + 0.13 P). +/-33% at the extremes, typically +/-12%.
    // oneMinus is one op where a const-1 plus an add would be two.
    const value = M.mul(0.20).add(P.mul(0.13)).oneMinus();
    const hue = D.mul(0.09);
    s.tint(value.add(hue), value, value.sub(hue));
  }

  biomes() {
    return {
      __terrain: { material: "dirt" },
      foothills: {},
      meadow: {},
      mountains: {},
      ocean: {},
    };
  }
}
