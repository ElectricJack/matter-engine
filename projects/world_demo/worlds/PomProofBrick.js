// PomProofBrick — PomProof's dome instrument wearing a REGULAR LATTICE.
//
// ---------------------------------------------------------------------------
// RELATIONSHIP TO PomProof.js — READ THIS BEFORE EDITING EITHER FILE
//
// This world is a deliberate sibling of PomProof.js. The geometry, cameras,
// lighting, streaming bands, biome table and surfaces() tape are IDENTICAL by
// design; the ONLY intended difference is the detail tileset:
//
//     PomProof       -> ForestFloor  (stochastic: spectral base + scattered
//                                     pebbles, rocks and leaves)
//     PomProofBrick  -> BrickProof   (regular:   running-bond masonry, flat
//                                     faces, deep sharp-walled grooves)
//
// It is a separate file rather than a subclass because the provider loads
// exactly one world script per process (local_provider.h builds
// cfg.world_path = worlds/<MATTER_WORLD>.js), so `extends PomProof` has
// nothing to resolve against — PomProof.js is never in scope. It is a separate
// file rather than a swap because PomProof.README.md's entire measurement
// table (the cap-travel figures, the incidence-vs-cap-decided histogram, the
// max_march_m -> onset ladder) was taken against ForestFloor, and silently
// re-pointing that world at a different atlas would invalidate every published
// number in it.
//
// KEEPING THE TWO IN STEP: the field, camera, lights, streaming and biomes
// blocks below are meant to stay byte-identical to PomProof.js. If you change
// one, change both, and diff them to confirm:
//
//     diff <(sed -n '/field(p)/,$p' worlds/PomProof.js) \
//          <(sed -n '/field(p)/,$p' worlds/PomProofBrick.js)
//
// should report no differences. That is the whole point: the two worlds are a
// single-variable A/B on the DETAIL TEXTURE, and they are only a valid A/B for
// as long as nothing else drifts between them.
//
// ---------------------------------------------------------------------------
// WHY A REGULAR LATTICE IS WORTH A SECOND WORLD
//
// PomProof isolated the variable (surface orientation) but could not always
// read the answer, because ForestFloor is noise and NOISE HIDES THE FAILURES:
//
//   * A smeared pebble field still looks like a pebble field. There is no
//     correct position for any pebble, so a march that lands metres away
//     produces something that is wrong but not WRONG-LOOKING. A brick course
//     is a straight line with a known direction; bend it, shear it or tear it
//     and there is nothing to argue about.
//
//   * Occlusion has no reference. "Is that pebble hiding the hollow behind
//     it?" has no answer when the hollow's shape is unknown. A flat brick
//     face with a 12 cm groove behind it has exactly one correct behaviour at
//     a grazing angle: the near lip occludes the groove. When it fails you
//     watch the groove smear THROUGH the brick.
//
//   * The warp is invisible. This is the important one. On a dome flank the VT
//     warp field re-parameterises the surface, and a regular grid draws that
//     re-parameterisation directly — curved courses, varying brick size,
//     shear ARE the warp made visible. Noise has no lattice to distort, so
//     PomProof could show that the warp changed the picture but never what it
//     was geometrically DOING. Run this world with and without
//     MATTER_VT_WARP=0 (it is read once per process, so that needs two
//     launches) and the difference is legible rather than merely present.
//
//   * The depth is authored. BrickProof's grooves are exactly 0.12 m deep, so
//     apparent displacement can be checked against a number instead of being
//     judged "present". See BrickProof.js's HEIGHT BUDGET section: this atlas's
//     h_range is 0.22 m in closed form, and the relief cap saturates VISUALLY
//     at 0.17 m = groove depth + the bake's 0.05 m padding. ForestFloor could
//     not offer either figure — its README records that it could not pin down
//     its own h_range at all, because the slider tops out at 0.5 and
//     ForestFloor's range sits just above 0.352.
//
// CONSEQUENCE FOR THE SMEAR CONTOUR. The onset of grazing smear is
// acos(relief / max_march_m) with relief = min(h_range, relief_cap_m). Because
// this atlas's h_range is 0.22 rather than ForestFloor's ~0.36, the onset at
// stock settings is acos(0.22 / 1.59) = 82.0 degrees, NOT the 77.2 degrees
// quoted in PomProof.README.md. To probe the formula at 77.2 degrees here, set
//
//     render.pom.max_march_m = 0.99        (0.22 / cos 77.2 = 0.994)
//
// over MATTER_CMD_FIFO. Testing the same formula at two different (relief,
// max_march_m) pairs that predict two different angles is a stronger check
// than reproducing one number.
// ---------------------------------------------------------------------------

// The one material. Albedo and roughness are byte-identical to PomProof's
// POM_GROUND, and BrickProof's base() uses MAT.dirt exactly as ForestFloor's
// does, so the two worlds differ in the HEIGHT CHANNEL and nothing else.
//
// Mid-neutral so neither shadowed nor lit relief clips, and so the eye reads
// GEOMETRY rather than colour. Roughness high: a specular sheen travelling
// across the surface would be a second parallax cue and would contaminate the
// judgement of the first.
const POM_GROUND = defineMaterial("pomGround", {
  albedo: [0.34, 0.32, 0.28],
  roughness: 0.95,
  detail: "BrickProof",
});

const GROUND = 1.0;   // flat-ground height, metres

class PomProofBrick extends World {
  static params = { worldSeed: 20260802 };

  static world = { sectorSize: 64, yMin: -8, yMax: 96 };

  // ------------------------------------------------------------------------
  // CAMERAS — identical to PomProof's so captures from the two worlds overlay
  // exactly. The incidence table is for the MEASUREMENT camera; spawn is a
  // wider framing that keeps the control ground in shot.
  //
  // Eye 8 m short of the 40 m dome's rim at 6 m up. The visible band of that
  // dome runs from its rim at (45, 1) to the silhouette at (54.34, 26.69), and
  // over those 9.3 m of surface the incidence sweeps continuously. Measured
  // off the meshed gradient normals (what gbuffer.frag hands the march as
  // plane_n), by 1 m of x:
  //
  //     45-46  cos 0.837  33 deg      50-51  cos 0.216  77 deg
  //     46-47  cos 0.710  45 deg      51-52  cos 0.135  82 deg
  //     47-48  cos 0.521  59 deg      52-53  cos 0.081  85 deg
  //     48-49  cos 0.405  66 deg      53-54  cos 0.034  88 deg
  //     49-50  cos 0.289  73 deg      54-55  cos 0.009  89 deg
  //
  // With a 0.5 m brick pitch and a 0.25 m course pitch, that band is ~19
  // bricks wide and ~37 courses tall, so the lattice is resolved everywhere
  // along it — which is what makes a course that bends visible as a bend
  // rather than as a couple of stray pixels.
  //
  // The world schema carries ONE camera and no fov, so the rest live here as a
  // comment; drive them over MATTER_CMD_FIFO with `cam ex ey ez tx ty tz`.
  //
  //   THE MEASUREMENT CAMERA — the framing whose image rows map monotonically
  //   onto the incidence table above:
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
  static camera = {
    position: [-24.0, 2.5, -12.0],
    target: [40.0, 10.0, 6.0],
  };

  // Raking side light. The camera looks along +X, so a sun travelling mostly
  // along -Z rakes ACROSS the view direction: relief casts across the flank
  // instead of toward or away from the eye. Light arriving along the view axis
  // flattens exactly the shading cue this world exists to inspect. It matters
  // more here than in PomProof: a raking light is what puts a hard shadow line
  // on the far wall of every bed joint, and that shadow line is the most
  // sensitive occlusion indicator in the frame.
  static lights = {
    sun: { dir: [-0.35, -0.42, -0.84], color: [2.4, 2.2, 1.95] },
    sky: { color: [0.42, 0.52, 0.72] },
  };

  static volumetrics = { enabled: false };

  static streaming = {
    rings: [
      { radius: 128.0, rung: 1 },
      { radius: 340.0, rung: 0 },
    ],
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
  // on purpose, exactly as in PomProof. It binds BrickProof to material 16
  // (DIRT), which is the bucket the geometry band table emits, so the legacy
  // non-chart sampling path sees the SAME atlas the surfaces() tape does.
  // Because it agrees with POM_GROUND's `detail:` on (module, params "{}",
  // density 0), plan_detail_bakes MERGES the two into a single request: one
  // settle, one .gtex, one slot, two bound materials. "Exactly one detail
  // tileset in the world" is therefore true of every path, not just the
  // chart-VT one.
  static roots = [
    {
      module: "BrickProof",
      transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
      tileset: true,
    },
  ];

  field(p) {
    // Three hemispheres over a dead-flat plane. Identical to PomProof.
    //
    // dome() (world_base.js) returns EXACTLY 0 outside its footprint, so max()
    // unions them and the final .add(GROUND) lifts the whole thing onto the
    // plane: flat ground reads exactly GROUND, and each crown reads exactly
    // GROUND + R. No clamp is needed and none is used — the closed form is
    // therefore literally the surface, which is what makes every angle in this
    // world computable rather than estimated.
    const small = dome(0.0, 0.0, 15.0);
    const medium = dome(85.0, 0.0, 40.0);
    const large = dome(245.0, 0.0, 80.0);
    const height = small.max(medium).max(large).add(GROUND);

    // Required by the field contract, and both are deliberately inert here.
    const relief = noise2(p.worldSeed ^ 1, 1 / 900, 2);
    const moisture = noise2(p.worldSeed ^ 2, 1 / 700, 2);

    return {
      density: heightToDensity(height),
      moisture,
      relief,
      // ABOVE EVERY SURFACE IN THE WORLD, and load-bearing rather than a
      // mistake: FieldRuntime::biome_at returns Ocean wherever height is below
      // sea level, and WorldSector.js skips every scatter kind on ocean ground
      // — including the landmark boulders, which are the one kind NOT gated by
      // the biome table's counts and would otherwise drop a 25-40 m rock every
      // 180 m across this world. A test world for a per-texel shading effect
      // must not have props in front of the surface under test.
      seaLevel: 1.0e5,
    };
  }

  // The identity classifier: one material, constant weight. Nothing varies but
  // orientation — there is no second weight field for a boundary to appear in,
  // and no appearance lane, so every texel composites from the same atlas with
  // the same tint.
  surfaces(s) {
    s.weight(POM_GROUND, s.value(1.0));
  }

  biomes() {
    // __terrain forces every geometry bucket to DIRT so the terrain meshes as
    // ONE material; the tape drives appearance only. The biome entries are
    // present and empty: with all counts at zero, and every point classified
    // Ocean by the sea level above, WorldSector places nothing at all.
    return {
      __terrain: { material: "dirt" },
      foothills: {},
      meadow: {},
      mountains: {},
      ocean: {},
    };
  }
}
