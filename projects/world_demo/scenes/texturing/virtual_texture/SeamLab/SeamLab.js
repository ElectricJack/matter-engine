// SeamLab — the instrument world for terrain SEAMS, and nothing else.
//
// Filed against issue da52492c ("sector borders are not correctly connecting",
// StreamCaverns, streaming frozen). StreamCaverns cannot answer that question:
// it is a cave world, so it has AUTHORED holes in its surface everywhere, and a
// dark patch in a screenshot is exactly as likely to be a tunnel mouth as a
// crack. Every seam measurement taken on it has to argue about which. This
// world removes the argument by removing the ambiguity, and it is deliberately
// the smallest thing that still exercises the whole cross-level machinery.
//
// THE ORACLE, and why the world is shaped the way it is.
//
//   * The density is a HEIGHTFIELD -- `h(x, z) - y`, no caves, no overhangs, no
//     shafts. Every solid point below the surface is solid all the way to yMin.
//     A camera above the terrain therefore has NOTHING legitimate to see except
//     the surface, so any ray that reaches the background has gone through a
//     hole. `background pixels == 0` is an exact, tolerance-free gate, and it is
//     the check StreamCaverns can never have.
//   * The surface still has to be MESHED volumetrically (`volumetricSectors`),
//     so tiles are cubes, seams exist on all six faces, and the -y band and the
//     Y-tiled mesher path are under test -- the same regime the issue was filed
//     against. Heightfield density + cube tiles is the cheap corner of that
//     regime, not a different one.
//   * A 32 m world-space CHECKER in the tape paints level-0 tile parity onto the
//     terrain. Coarser tiles are 64/128/256 m, all multiples of 32, so every
//     tile border in this world coincides with a checker edge whatever level it
//     is drawn at. A screenshot therefore says where the tile borders are
//     without a debug view, and a crack can be read against them by eye.
//
// SIZE. Reach is 496 m and a level-0 tile is 32 m at a 2 m voxel, so every tile
// at every level is 16^3 cells -- an eighth of the 32^3 the 64 m worlds bake.
// The whole world fills in seconds, which is what makes it usable as a test
// fixture rather than a soak.
//
// NO SCATTER, NO TILESETS, NO VOLUMETRICS. Vegetation would occlude the thing
// being looked at, a .gtex bake would put tileset settle time in front of every
// run, and fog would tint the background the oracle is thresholding on. The
// materials below carry no `detail` key for the same reason: untextured flat
// albedo is the highest-contrast background a crack can appear against.
//
// See docs/seam-suite-2026-08-13.md for the checks that run against this world.

// ---------------------------------------------------------------------------
// Materials. Two, both untextured, chosen for contrast rather than looks: the
// checker in surfaces() blends between them, and a crack shows as neither.
// Material 0 is the fallback when every tape weight is zero.
// ---------------------------------------------------------------------------
const SEAM_PALE = defineMaterial("seamPale", {
  albedo: [0.74, 0.72, 0.68],
  roughness: 1.0,
});
const SEAM_SLATE = defineMaterial("seamSlate", {
  albedo: [0.34, 0.36, 0.40],
  roughness: 1.0,
});

class SeamLab extends World {
  static params = { worldSeed: 20260813 };

  // sectorSize 32 with the global 2 m voxel base gives 16 cells per tile per
  // axis at EVERY level (a level-L tile is 32<<L metres at a 2<<L voxel), so a
  // cube tile is 4096 cells against the 32768 a 64 m world bakes.
  //
  // The Y extent is 256 m: eight level-0 tiles stacked, one level-3 tile. That
  // is enough for the surface to cross several horizontal tile planes -- which
  // is what puts the -y face and its band under test -- without paying for
  // depth nothing will ever look at.
  static world = { sectorSize: 32, yMin: -64, yMax: 192 };

  // Looking down the ladder from above: at 150 m up and 40 degrees down the
  // frame is all terrain out to the fog-free horizon, which is the pose the
  // background-pixel oracle wants. The suite drives its own poses; this is
  // where the editor opens.
  static camera = {
    position: [180.0, 150.0, 180.0],
    target:   [0.0, 60.0, 0.0],
  };

  // Both off. Volumetrics and fog are per-frame GPU costs that tint the
  // background, and the background is what the oracle thresholds on.
  static volumetrics = { enabled: false };
  static fog = { density: 0.0 };

  // Thresholds above the noise range, as in StreamCaverns: every sector
  // classifies as `foothills`, whose entry below is empty, so nothing scatters.
  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  static streaming = {
    nestedSectors: true,
    volumetricSectors: true,

    // FOUR LEVELS, NOT SIX. Three cross-level rings is enough to have one of
    // every transition the welder handles, and each level added past that
    // doubles the tile size a coarse-side residue is measured in without adding
    // a new case. Reach 496 m keeps the whole world inside one screenshot.
    //
    // Each annulus is at least as wide as one tile of the next coarser level
    // (48/64/128/256 against tiles of 64/128/256), so the 2:1 restriction pass
    // has nothing to repair and every seam in frame is a 1-level step -- the
    // case the welder is specified for. A world that violated that would be
    // testing the repair pass instead.
    terrainBands: [
      { radius:  48.0, lod: 5 },   // level 0:  32 m tiles,  2 m voxels
      { radius: 112.0, lod: 4 },   // level 1:  64 m tiles,  4 m
      { radius: 240.0, lod: 3 },   // level 2: 128 m tiles,  8 m
      { radius: 496.0, lod: 2 },   // level 3: 256 m tiles, 16 m
    ],
    // Inert under nesting (the outer band bounds residency), kept so the world
    // still reads the same with nestedSectors off.
    rings: [ { radius: 496.0, rung: 0 } ],
  };

  field(p) {
    // RELIEF AT EVERY VOXEL SCALE, which is the whole design of this height.
    // A seam opens because the coarse tile and the fine tile disagree about
    // where the surface is, and they disagree in proportion to how much of the
    // height lives BELOW the coarse voxel. So there is one octave set per rung
    // in the ladder: 420 m (visible at every level), 110 m (lost by the 16 m
    // voxel), 26 m (lost by the 8 m voxel) and 7 m (lost by the 4 m voxel).
    // A smooth world would hide the defect this world exists to show.
    const swell  = noise2(p.worldSeed ^ 0x01, 1 / 420, 3).mul(42);
    const ridges = ridge2(p.worldSeed ^ 0x02, 1 / 110, 2).mul(22);
    const grain  = noise2(p.worldSeed ^ 0x03, 1 /  26, 2).mul(6);
    const fine   = noise2(p.worldSeed ^ 0x04, 1 /   7, 2).mul(1.5);

    // THE SCARP. A near-vertical step ~24 m tall along a wandering line, put in
    // because a seam on a near-vertical wall is the worst case for a mesher
    // whose ownership rule is direction-asymmetric, and because a graph over
    // the plane with a 3 m transition is as close to a wall as a heightfield
    // gets. `wander` keeps the line off the tile grid so the step is never
    // accidentally parallel to the seam it is crossing.
    const wander = noise2(p.worldSeed ^ 0x05, 1 / 260, 2).mul(70);
    const scarp  = worldX().add(wander).smoothstep(-3.0, 3.0).mul(24);

    // 72 m base keeps the whole range inside [yMin, yMax] with margin: the sum
    // above spans roughly -70..+70 about it.
    const height = swell.add(ridges).add(grain).add(fine).add(scarp).add(72.0);

    // Inert biome channels (see biomeThresholds), and a sea level below yMin so
    // nothing in this world is ocean.
    const inert = blend(0.0, 0.0, 0.0);
    return {
      density: heightToDensity(height),
      moisture: inert,
      relief: inert,
      seaLevel: -2000.0,
    };
  }

  // -------------------------------------------------------------------------
  // The tape: a 32 m world-space CHECKER, and that is all.
  //
  // This is a measuring aid, not a look. 32 m is the level-0 tile size and
  // every coarser tile is a multiple of it, so a checker cell edge lies on a
  // tile border at every level in the ladder -- meaning a screenshot of this
  // world shows you the tile grid, and a crack can be located against it
  // without a debug view or a wireframe pass.
  //
  // Built from worldX/worldZ rather than noise so it is exactly periodic: any
  // wobble in a checker edge across a seam is the seam, not the pattern.
  // `fract` is the only op here that is not arithmetic.
  // -------------------------------------------------------------------------
  surfaces(s) {
    // Sawtooth over 64 m thresholded at the half -> 32 m stripes, so a checker
    // CELL is 32 m and every cell edge lies on a multiple of 32. The period is
    // twice the cell, which is the part that is easy to get wrong: a 32 m
    // period gives 16 m cells whose edges land on odd multiples of 16 and miss
    // half the tile borders they are supposed to mark.
    const stripe = (axis) =>
      axis.mul(1 / 64.0).fract().smoothstep(0.49, 0.51);
    const sx = stripe(s.worldX);
    const sz = stripe(s.worldZ);
    // XOR of the two stripes: a + b - 2ab.
    const checker = sx.add(sz).sub(sx.mul(sz).mul(2.0));

    s.weight(SEAM_PALE, checker.oneMinus());
    s.weight(SEAM_SLATE, checker);

    // Flat value: no depth grading, no grain. Shading variation would compete
    // with the thing being looked for.
    s.tint(1.0, 1.0, 1.0);
  }

  biomes() {
    // No counts and no __vegetation key anywhere: WorldSector reads __terrain
    // only, and every entry below is deliberately the empty set.
    return {
      __terrain: { material: "dirt" },
      foothills: {},
      meadow:    {},
      mountains: {},
      ocean:     {},
    };
  }
}
