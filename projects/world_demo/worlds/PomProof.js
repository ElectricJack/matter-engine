// PomProof — a purpose-built instrument for looking at ground POM.
//
// WHY THIS WORLD EXISTS
//
// StreamMountain is the wrong instrument for judging parallax occlusion
// mapping. It blends five detail tilesets through a slope/altitude classifier,
// so every change of surface angle arrives together with a change of MATERIAL.
// When a steep face looks wrong there is no way to tell whether the fault is
// the angle, the tileset, the blend between two tilesets, or the distance —
// all four covary. Worse, the flat ground it is compared against is a
// different tileset again.
//
// This world removes every variable except one:
//
//   * ONE detail tileset over the entire world (ForestFloor — see below).
//     No classifier. No blend. No second material. The surfaces() tape
//     declares a single material at constant weight 1, which is the identity
//     classifier; nothing about the surface varies anywhere except its
//     ORIENTATION.
//   * Three hemispherical domes of radius 15, 40 and 80 m. A hemisphere
//     presents every surface angle from 0 deg at the crown to 90 deg at the
//     rim, CONTINUOUSLY, on one object, with no seam and no material change.
//     An angle-dependent defect therefore appears as a band at a computable
//     radius rather than hiding in the confound between two materials.
//   * Perfectly flat ground everywhere else (y = 1 m exactly), as the control.
//     POM is known-good on flat ground, so the flats are what the flanks are
//     judged against — and they are in the same frame, at the same distance
//     band, wearing the same texture.
//   * Three radii so scale dependence is visible: the same 0.3-0.4 m of relief
//     against a 15 m, a 40 m and an 80 m curvature.
//
// THE GEOMETRY IS EXACT, WHICH IS THE POINT
//
// Each dome is the closed form
//
//     y(d) = GROUND + R * sqrt(max(0, 1 - (d/R)^2)),   d = |(x,z) - (cx,cz)|
//
// so for any camera the incidence angle at any surface point can be computed
// in closed form instead of guessed. For an eye at E and a dome centred at
// C = (cx, GROUND, cz) with radius R, the SILHOUETTE (incidence exactly 90 deg,
// where the view ray is tangent) is the circle at distance R^2/L from C along
// the C->E axis, radius R*sqrt(1 - R^2/L^2), with L = |E - C|. Everything
// between the near rim and that circle sweeps monotonically from face-on to
// tangent. That is what makes the smear onset MEASURABLE here and not in
// StreamMountain: you can read the angle off the geometry.
//
// The flat ground gives the same reading for free: from an eye at height h,
// flat ground at horizontal distance d is at incidence atan(d/h). At the spawn
// camera's h = 6 m that is 45 deg at 6 m, 65 deg at 12.9 m, 77 deg at 26 m,
// 84 deg at 57 m, 88 deg at 172 m. So the ground apron IS a calibrated
// incidence ruler lying in the bottom of every frame.
//
// WHAT TO LOOK AT — see README.md beside this file.

// ---------------------------------------------------------------------------
// THE ONE MATERIAL.
//
// ForestFloor, not AlpineRockDetail, and the reason is the height channel.
// AlpineRockDetail's own header states its accounting: base sigma ~0.018 m
// (range ~ +-0.06) with slabs up to ~+0.09 proud, i.e. a total relief around
// 0.15 m. The Ground POM relief cap slider runs to roughly half a metre, and
// the shader takes relief = min(atlas h_range, relief_cap_m) — so on
// AlpineRockDetail every cap value above ~0.15 m is CLAMPED BY THE ATLAS and
// moving the slider does nothing at all. That would make the central
// experiment (change the cap, watch the displacement move) unfalsifiable.
//
// ForestFloor's base spans [-0.142, +0.132] m before its scattered Pebble and
// Rock children stand proud of it, which puts its baked h_range near 0.38 m —
// enough that the whole useful cap range moves the picture. It is also the
// tileset under the grazing-smear diagnosis this world is meant to reproduce,
// and it is the one that literally carries pebbles and rocks, which is the
// thing an observer watches slide inward as the cap comes up.
// ---------------------------------------------------------------------------
const POM_GROUND = defineMaterial("pomGround", {
  // Mid-neutral so neither shadowed relief nor lit relief clips, and so the
  // eye reads GEOMETRY rather than colour. Roughness high: a specular sheen
  // moving across the surface would be a second parallax cue and would
  // contaminate the judgement of the first.
  albedo: [0.34, 0.32, 0.28],
  roughness: 0.95,
  detail: "ForestFloor",
});

const GROUND = 1.0;   // flat-ground height, metres

class PomProof extends World {
  static params = { worldSeed: 20260802 };

  // yMax clears the tallest crown (GROUND + 80 = 81 m) with room for the
  // native-voxel slab; yMin is shallow because nothing is below the ground
  // plane and every metre of slab is meshing cost at rung 0.
  static world = { sectorSize: 64, yMin: -8, yMax: 96 };

  // ------------------------------------------------------------------------
  // CAMERAS — deliberately METRES from a flank, not tens of metres. The
  // incidence table below is for the MEASUREMENT camera (see the second block);
  // spawn is a wider framing that keeps the control ground in shot.
  //
  // Eye 8 m short of the 40 m dome's rim at 6 m up, looking up and along the
  // flank. The visible band of that dome runs from its rim at (45, 1) to the
  // silhouette at (54.34, 26.69) — everything past that is self-occluded — and
  // over those 9.3 m of surface the incidence sweeps continuously. Measured
  // off the meshed gradient normals (which is what gbuffer.frag hands the
  // march as plane_n, not the face normals), by 1 m of x:
  //
  //     45-46  cos 0.837  33 deg      50-51  cos 0.216  77 deg
  //     46-47  cos 0.710  45 deg      51-52  cos 0.135  82 deg
  //     47-48  cos 0.521  59 deg      52-53  cos 0.081  85 deg
  //     48-49  cos 0.405  66 deg      53-54  cos 0.034  88 deg
  //     49-50  cos 0.289  73 deg      54-55  cos 0.009  89 deg
  //
  // at distances of only 9-27 m, so 0.35 m of relief is tens of pixels of
  // parallax rather than a sub-pixel rumour. That range — face-on to tangent,
  // one material, one object, one frame, at one distance decade — is the whole
  // instrument. The target is aimed at the middle of the band: the flank spans
  // elevations 8.7 deg (its foot) to 50.0 deg (the tangent point) from this
  // eye, so 29.4 deg centres it.
  //
  // A low eye near a close dome necessarily fills the frame with dome; the
  // flat control ground is then at the left and right margins and beyond the
  // dome, not underfoot. That is geometry, not an oversight — see the
  // flat-ground control camera below when the flats need to dominate.
  //
  // The world schema carries ONE camera and no fov, so the rest live here as a
  // comment; drive them over MATTER_CMD_FIFO with `cam ex ey ez tx ty tz`.
  //
  //   THE MEASUREMENT CAMERA — every number in PomProof.README.md was taken
  //   here, because it is the framing whose image rows map monotonically onto
  //   the incidence table above, which is what makes a row -> angle calibration
  //   possible at all:
  //     position [ 37.0,  6.0,  0.0]  target [ 53.0, 15.0,  0.0]
  //   the 80 m dome's flank at the same 8 m standoff (same angles, 2x scale):
  //     position [157.0,  6.0,  0.0]  target [176.0, 26.0,  0.0]
  //   flat-ground control looking DOWN at 45 deg, no dome in frame:
  //     position [ 30.0, 18.0,  0.0]  target [ 55.0, 10.0,  0.0]
  //   all three domes broadside, for scale dependence in one frame:
  //     position [ 12.0, 14.0, 55.0]  target [ 70.0, 12.0,  5.0]
  //   pressed right against the 15 m dome, ~8 m, maximum parallax:
  //     position [-21.0,  2.2,-10.0]  target [ 40.0, 10.0,  6.0]
  // ------------------------------------------------------------------------
  //
  // SPAWN is a different camera from the measurement one, deliberately. It sits
  // ~12 m off the 15 m dome's flank with the dome filling the right of frame
  // and FLAT GROUND running from under the camera out to the horizon on the
  // left — so one frame holds the flank's face-on-to-tangent sweep AND the
  // control surface it is judged against, at a matched set of angles, in the
  // same texture. The 40 m dome stands behind as a silhouette for scale.
  //
  // The measurement camera is 8 m off the 40 m flank and is better for reading
  // the effect, but a low eye that close fills the entire frame with dome and
  // leaves no control in shot. That is geometry, not an oversight: a plane seen
  // from 2 m up is only ever visible in the sliver below the horizon. Rather
  // than compromise both jobs into one mediocre camera, spawn shows the effect
  // WITH its control and the measurement camera isolates the flank.
  static camera = {
    position: [-24.0, 2.5, -12.0],
    target: [40.0, 10.0, 6.0],
  };

  // Raking side light. The camera looks along +X, so a sun travelling mostly
  // along -Z rakes ACROSS the view direction: relief casts across the flank
  // instead of toward or away from the eye. Light that arrives along the view
  // axis flattens exactly the shading cue this world exists to inspect.
  static lights = {
    sun: { dir: [-0.35, -0.42, -0.84], color: [2.4, 2.2, 1.95] },
    sky: { color: [0.42, 0.52, 0.72] },
  };

  // No `static fog` and volumetrics off: aerial perspective would put a
  // distance-dependent veil over exactly the far ground the near flank is
  // being compared against, and A/B captures have to differ ONLY in the knob
  // under test. (Volumetrics also need RT, and the deterministic capture
  // procedure runs with MATTER_DISABLE_VK_RT=1.)
  static volumetrics = { enabled: false };

  // Scatter tiers. Nothing is scattered in this world (see biomes() below), so
  // these only bound sector residency; they are small on purpose.
  static streaming = {
    rings: [
      { radius: 128.0, rung: 1 },
      { radius: 340.0, rung: 0 },
    ],
    // lod 5 is the native voxel mesh (2 m lattice) and is the only rung that
    // resolves a near-vertical dome flank at all; the ladder below it is a
    // heightfield grid at 1<<lod cells per 64 m sector, i.e. 4 m at lod 4 and
    // 32 m at lod 1. 320 m of lod 5 reaches past the far edge of the 80 m dome
    // from the spawn point (325 - 37 = 288 m) and stops there, because native
    // voxel sectors are the expensive ones and nothing past the domes matters.
    terrainBands: [
      { radius: 320.0, lod: 5 },
      { radius: 460.0, lod: 4 },
      { radius: 600.0, lod: 3 },
      { radius: 740.0, lod: 2 },
      { radius: 880.0, lod: 1 },
      { radius: 1020.0, lod: 0 },
    ],
  };

  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  // `tileset: true` is the deprecated pre-defineMaterial alias and it is here
  // on purpose. It binds ForestFloor to material 16 (DIRT), which is the
  // bucket the geometry band table below emits, so the legacy non-chart
  // sampling path sees the SAME atlas the surfaces() tape does. Because it
  // agrees with POM_GROUND's `detail:` on (module, params "{}", density 0),
  // plan_detail_bakes MERGES the two into a single request: one settle, one
  // .gtex, one slot, two bound materials. "Exactly one detail tileset in the
  // world" is therefore true of every path, not just the chart-VT one.
  static roots = [
    {
      module: "ForestFloor",
      transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
      tileset: true,
    },
  ];

  field(p) {
    // Three hemispheres over a dead-flat plane.
    //
    // dome() (world_base.js) returns EXACTLY 0 outside its footprint, so max()
    // unions them and the final .add(GROUND) lifts the whole thing onto the
    // plane: flat ground reads exactly GROUND, and each crown reads exactly
    // GROUND + R. No clamp is needed and none is used — the closed form in the
    // header comment is therefore literally the surface, which is what makes
    // every angle in this world computable rather than estimated.
    //
    // Placement leaves flat runs between the footprints ([15, 45] and
    // [125, 165] on the +X axis) so a frame can hold flank and control ground
    // at once, and puts them on one axis so the "all three at once" camera
    // above works. 50 field ops of the 96-op budget: 2 shared coordinate reads
    // + 14 per dome + 2 unions + 2 for the lift + 2 noise.
    const small = dome(0.0, 0.0, 15.0);
    const medium = dome(85.0, 0.0, 40.0);
    const large = dome(245.0, 0.0, 80.0);
    const height = small.max(medium).max(large).add(GROUND);

    // Required by the field contract, and both are deliberately inert here:
    // biomeThresholds above are set past what these can reach, and the
    // surfaces() tape reads neither. They exist so the program parses.
    const relief = noise2(p.worldSeed ^ 1, 1 / 900, 2);
    const moisture = noise2(p.worldSeed ^ 2, 1 / 700, 2);

    return {
      density: heightToDensity(height),
      moisture,
      relief,
      // ABOVE EVERY SURFACE IN THE WORLD, and that is load-bearing rather than
      // a mistake. FieldRuntime::biome_at returns Ocean wherever height is
      // below sea level, and WorldSector.js skips every scatter kind on ocean
      // ground — including the landmark boulders, which are the one kind that
      // is NOT gated by the biome table's counts and would otherwise drop a
      // 25-40 m rock every 180 m across this world. A test world for a
      // per-texel shading effect must not have props in front of the surface
      // under test. Nothing else consumes sea level: it is read once into
      // WorldSession::sea_level for an editor readout, and no water is drawn.
      seaLevel: 1.0e5,
    };
  }

  // The identity classifier: one material, constant weight. This is what makes
  // "nothing varies but orientation" literally true — there is no second
  // weight field for a boundary to appear in, and no appearance lane, so every
  // texel in the world composites from the same atlas with the same tint.
  surfaces(s) {
    s.weight(POM_GROUND, s.value(1.0));
  }

  biomes() {
    // __terrain forces every geometry bucket to DIRT, so the terrain meshes as
    // ONE material — the tape drives appearance only, exactly as ChartVtProof
    // and MetalProof do. The biome entries are present and empty: with all of
    // them at zero counts, and every point classified Ocean by the sea level
    // above, WorldSector places nothing at all.
    return {
      __terrain: { material: "dirt" },
      foothills: {},
      meadow: {},
      mountains: {},
      ocean: {},
    };
  }
}
