// StreamCaverns — a VOLUMETRIC streaming world, and the voxel terrain stress test.
//
// Every other streamed world in this project is a heightfield: its density is
// `h(x, z) - y`, so the mesher knows there is nothing to mesh outside a few
// voxels either side of the surface and never looks. This world's density reads
// world y directly (noise3/ridge3 + worldY, see field()), so the mesher has to
// mesh the whole authored Y range -- 1280 m of it, at every rung -- and that is
// the point. It exists to measure what the voxel path actually costs when the
// surface is not a graph over the plane.
//
// The shape: a gentle plateau perforated by a tunnel network that starts at the
// surface and runs to 1000 m down. Swiss cheese, not a mountain. Tunnels break
// through the surface wherever they happen to reach it -- the openings are not
// authored, they are what a 3D field does when it meets a 2D one.
//
// SCALE. The network is sized to be flown, not squeezed through, and that is a
// measured property rather than an eyeballed one: MatterEngine3/tests/cavern_probe
// reads this world's density directly and reports the clearance distribution and
// whether a ball of a given radius can travel from open sky to the floor.
// Current (2048 m box, 8 m lattice): 26.5% air, mean clearance 23.6 m, 54% of
// the network's volume clears 12 m, and a 16 m ball reaches y = -980 m. Before
// the widening those were 12.6 m / 31.6% / -980 m -- note the last number did
// NOT move, which is the whole reason the probe reports both: the world was
// always connected to the bottom, it was connected through cracks.
//
//   cd MatterEngine3/tests && make build/cavern_probe && ./build/cavern_probe
//
// NO VEGETATION. objects/WorldSector.js here meshes terrain and returns; the
// biome table below carries no counts and no __vegetation profile, so the
// alpine planner never runs. That is deliberate: scatter is instance cost, and
// mixing it in would confound the measurement this world exists to take. If you
// want scatter over caves, that is a different world.

// ---------------------------------------------------------------------------
// Materials. Two, both reusing detail tilesets that already bake for
// StreamMountain -- a new tileset would put its settle cost in front of the
// first fill, which is exactly the number we are trying to read.
//
// Declaration order is load-bearing the same way it is in StreamMountain:
// material 0 is the fallback when every tape weight is 0
// (SurfaceRuntime::classify_vertices), and "unclassified rock" is the right
// answer here because most of this world's surface area is tunnel wall.
// ---------------------------------------------------------------------------
const CAVE_ROCK = defineMaterial("CaveRock", {
  albedo: [0.40, 0.39, 0.38],
  roughness: 0.95,
  detail: "AlpineRockDetail",
});
// Rubble: the floors of the tunnels, and the surface plateau up top.
const CAVE_RUBBLE = defineMaterial("CaveRubble", {
  albedo: [0.46, 0.44, 0.41],
  roughness: 1.0,
  detail: "ScreeDetail",
});

class StreamCaverns extends World {
  static params = { worldSeed: 20260810 };

  // yMin IS THE COST DIAL. A volumetric world meshes [yMin, surface] at every
  // rung, so the slab is (h_max - yMin) / voxel samples deep: at the 2 m rung
  // that is ~590 samples per column against the ~10 a heightfield sector needs.
  // -1024 is the authored "tunnels go down a thousand metres"; pulling it up is
  // the first thing to try if a fill is too slow to work with.
  static world = { sectorSize: 64, yMin: -1024, yMax: 256 };

  // Above the plateau looking down, so the surface openings are the first thing
  // in frame. Fly INTO one to see the network; the interesting camera work here
  // is vertical, which no other world in this project rewards.
  static camera = {
    position: [40.0, 260.0, 180.0],
    target:   [0.0, 90.0, 0.0],
  };

  // Volumetrics OFF and fog thin, unlike StreamMountain. Both are per-frame GPU
  // costs unrelated to voxel meshing, and leaving them on would put them in
  // every frame time this world is here to measure. Turn them on from the
  // editor if you want to see what caves look like full of god rays.
  static volumetrics = { enabled: false };
  static fog = {
    density:    0.0016,
    minHeight:  -1024.0,
    maxHeight:  200.0,
    noiseScale: 0.00030,
    color:     [0.62, 0.63, 0.66],
    wind:      [0.05, 0.0, 0.02],
  };

  // Thresholds above the [-1, 1] noise range, as in StreamMeadow: every sector
  // classifies as `foothills` and material_at returns dirt/rock by slope alone.
  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  static streaming = {
    nestedSectors: true,

    // VOLUMETRIC SECTORS (volumetric-sectors M3). This is the world the mode
    // exists for: yMin is -1024, so under the column path a level-0 tile
    // samples ~590 rows of density down to the floor whether or not there is
    // anything in them, and yMin is this world's dominant cost dial for exactly
    // that reason. With cubes, a tile away from the surface or a cavern shell
    // costs one all-air/all-solid pass and yields nothing.
    //
    // It also turns the bands from cylinders into spheres, which is what makes
    // flying DOWN a tunnel refine the rock around you rather than the surface
    // 800 m overhead.
    //
    // StreamCaverns flips first and alone; StreamMountain stays on columns
    // until this has been soaked. MATTER_VOLUMETRIC_SECTORS=0 forces it off
    // without editing the world, which is the A/B baseline.
    volumetricSectors: true,

    // BANDS PULLED IN HARD vs StreamMountain's 318/1186/2605/4702/7753/10095.
    // A heightfield sector's cost is its surface area; a volumetric sector's is
    // its VOLUME, and the near band pays that at the 2 m voxel. These radii keep
    // each annulus wider than one tile of the next coarser level
    // (160/224/448/768/1024/1472 against tiles of 128/256/512/1024/2048), so the
    // nested restriction pass has nothing to fix, while holding the whole world
    // to about 4 km of reach instead of 10.
    //
    // This is the dial to turn when measuring. Widening the LOD 5 band is the
    // most expensive single change you can make here.
    terrainBands: [
      { radius:  160.0, lod: 5 },   // level 0:   64 m tiles,  2 m voxels
      { radius:  384.0, lod: 4 },   // level 1:  128 m tiles,  4 m
      { radius:  832.0, lod: 3 },   // level 2:  256 m tiles,  8 m
      { radius: 1600.0, lod: 2 },   // level 3:  512 m tiles, 16 m
      { radius: 2624.0, lod: 1 },   // level 4: 1024 m tiles, 32 m
      { radius: 4096.0, lod: 0 },   // level 5: 2048 m tiles, 64 m
    ],
    // Under nesting the outermost BAND bounds residency and rings only grade
    // scatter, of which this world has none -- so this table is inert. It is
    // here at the outer band's radius so the world still reads the same with
    // nestedSectors off, which is the rollback position.
    rings: [
      { radius: 4096.0, rung: 0 },
    ],
  };

  field(p) {
    // ---- the surface: a gentle plateau ------------------------------------
    // Deliberately cheap and deliberately boring. Three octaves of 2D noise,
    // ~40-150 m, no ridges and no glacial remap. Every op here is evaluated
    // ONCE PER COLUMN by the mesher's ColumnCache (it is y-independent), so its
    // cost is amortised over ~590 depth samples -- but a reader comparing this
    // world's fill time against StreamMountain's should know the surface is not
    // where the difference comes from.
    const swell = noise2(p.worldSeed ^ 0x11, 1 / 900, 3).mul(38);
    const roll  = noise2(p.worldSeed ^ 0x12, 1 / 260, 3).mul(14);
    const grain = noise2(p.worldSeed ^ 0x13, 1 / 40,  2).mul(3);
    const height = swell.add(roll).add(grain).add(96.0);

    // ---- the tunnel network -------------------------------------------------
    // Air where TWO independent ridged 3D fields are both near their crests.
    //
    // The intersection is what makes tunnels rather than sheets, and it is worth
    // being precise about why. A single ridged field's high-value set is a
    // connected SURFACE -- ridged fbm peaks along the zero set of value noise,
    // which in 3D is a sheet, so thresholding one field carves caverns shaped
    // like folded paper. Intersecting two independent sheets gives their
    // intersection CURVE: a one-dimensional network, which is what a tunnel is.
    // Thresholding both at 0.58 puts a few percent of the volume in air and
    // leaves the network connected over hundreds of metres.
    //
    // Written as `t.sub(TH).mul(-1)` because FieldNode has no reverse subtract:
    // the value wanted is (TH - t), positive in rock and negative in air, so the
    // pair MAXes -- solid wherever either field falls short of its threshold.
    // SCALE IS THE WIDTH DIAL, not the threshold. Both control how much of the
    // volume is air, but they do it differently and only one of them makes a
    // tunnel you can fly down: fbm is scale-invariant, so doubling the
    // wavelength doubles every passage's cross-section at an unchanged air
    // fraction, while lowering the threshold keeps the passages the width they
    // were and simply adds more of them. Measured on the 1/130 original
    // (MatterEngine3/tests/cavern_probe.cpp): 40% of the network's volume sat
    // under 4 m of clearance and only 31.6% cleared 12 m -- a connected network
    // of cracks, which is exactly what "too small to fly through" describes.
    //
    // 1/240 is ~1.85x, and the second octave's gain drops 0.5 -> 0.4 because
    // that octave lands at half the wavelength and is what shreds a wall into
    // pinches: it was adding 8 m of relief to a 20 m passage.
    const TH = 0.58;
    const t1 = ridge3(p.worldSeed ^ 0x21, 1 / 240, 2, 0.4, 2.0);
    const t2 = ridge3(p.worldSeed ^ 0x22, 1 / 240, 2, 0.4, 2.0);
    const tunnels = t1.sub(TH).mul(-1).max(t2.sub(TH).mul(-1));

    // ---- the shafts: what actually makes the network DEEP -------------------
    // The tunnel network above is isotropic, and isotropic is not what "tunnels
    // go down a thousand metres" asks for. Measured (a 512 m flood fill from the
    // surface, 4 m cells): tunnels + caverns alone connect the sky to -300 m and
    // no further. Not because the network stops -- 3D noise keeps producing
    // passages all the way to the floor -- but because a passage that wanders
    // isotropically takes a random walk to descend, and a random walk does not
    // get 1000 m down inside any box you can see across.
    //
    // So: a set of fields that are functions of (x, z) ONLY. A 2D field carved
    // out of a 3D density is a vertical prism -- the same hole at every altitude
    // -- which is exactly a shaft. Two ridged 2D fields intersected the same way
    // the tunnels are gives isolated pipes rather than walls, and thresholding
    // them high (0.70 against the tunnels' 0.58) keeps them rare enough to be
    // landmarks instead of a colander.
    //
    // `wobble` is what stops them reading as drilled: a low-amplitude 3D noise
    // added to both fields before the threshold, so the shaft wall moves with
    // depth and the pipe winds. Both fields share the one wobble register
    // deliberately -- a shared displacement moves the whole shaft coherently,
    // where independent ones would just fray both walls.
    //
    // Cost note: the ridge2 pair is y-INDEPENDENT, so the mesher's ColumnCache
    // evaluates it once per column and never again down the ~590 samples of
    // depth. The part of this world that reaches deepest is also the cheapest
    // part of it.
    // Scaled with the tunnels (1/210 -> 1/330) so a shaft stays wider than the
    // passages feeding it -- a shaft narrower than its tunnels reads as a dead
    // end you cannot enter rather than as a way down. The wobble's wavelength
    // goes with it (1/70 -> 1/150): at 70 m it was displacing the wall faster
    // than the shaft was wide, which frays a pipe instead of bending it.
    const SH = 0.70;
    const wobble = noise3(p.worldSeed ^ 0x41, 1 / 150, 2, 0.5, 2.0).mul(0.22);
    const s1 = ridge2(p.worldSeed ^ 0x51, 1 / 330, 2, 0.5, 2.0).add(wobble);
    const s2 = ridge2(p.worldSeed ^ 0x52, 1 / 330, 2, 0.5, 2.0).add(wobble);
    const shafts = s1.sub(SH).mul(-1).max(s2.sub(SH).mul(-1));

    // ---- the caverns --------------------------------------------------------
    // Rooms, not corridors: one low-frequency 3D field thresholded high, so the
    // top few percent of its range opens out into chambers tens of metres
    // across. These are what the tunnel network runs between.
    // 1/260 -> 1/420, for the same reason the tunnels moved: these are supposed
    // to be the rooms the network runs between, and a room has to be
    // conspicuously bigger than the corridor or it is just a wide spot.
    const cav = noise3(p.worldSeed ^ 0x31, 1 / 420, 3, 0.5, 2.0);
    const caverns = cav.sub(0.42).mul(-1);

    // ---- the floor ----------------------------------------------------------
    // Solid rock below -1000 m, so the world has a bottom to stand on and the
    // deepest tunnels terminate instead of opening into the void under yMin.
    // (yMin itself is 24 m lower: the mesher needs the density to be unambiguous
    // at the very bottom sample of the slab, not exactly zero there.)
    const floorTop = worldY().sub(-1000.0).mul(-1);

    // ---- composition --------------------------------------------------------
    // heightToDensity(height) is `height - y`: positive below the surface. It is
    // MIN'd with the two carve fields (air wherever any of them says air) and
    // then MAX'd with the floor, which puts rock back below -1000 m regardless
    // of what the noise wanted there.
    //
    // Note what this makes the surface openings: nothing authored them. A tunnel
    // that happens to reach y = h(x, z) is simply air on both sides of the
    // surface, and the mesher emits the hole. The 1/240 m tunnel spacing against
    // a ~110 m mean surface height is what sets how many there are -- widening
    // the network made each opening bigger and, at an unchanged air fraction,
    // made them rarer.
    const density = heightToDensity(height)
      .min(tunnels)
      .min(shafts)
      .min(caverns)
      .max(floorTop);

    // Inert biome channels, the same construction StreamMountain retired its
    // real ones to: `biomeThresholds` above is unreachable, so a constant inside
    // [-1, 1] classifies every sector as `foothills` exactly as a noise channel
    // would have. Two ops for both.
    const inert = blend(0.0, 0.0, 0.0);
    return {
      density,
      moisture: inert,
      relief: inert,
      seaLevel: -2000.0,   // below yMin: nothing in this world is ocean
    };
  }

  // -------------------------------------------------------------------------
  // The classifier tape. Short on purpose -- this world is a measurement, not a
  // look -- but not absent: a tunnel wall rendered in one flat grey gives no
  // depth cue at all, and "are these actually connected tunnels?" is a question
  // you answer with your eyes.
  //
  // The one interesting thing here: `s.altitude` and `s.normalY` are the only
  // handles that mean anything underground. There is no snow line, no tree
  // line, and no sun -- the classifier's whole job is to separate the surfaces
  // you can stand on from the ones you cannot.
  // -------------------------------------------------------------------------
  surfaces(s) {
    const seed = StreamCaverns.params.worldSeed;
    const up = s.normalY;

    // Rubble on anything walkable -- the plateau up top and the tunnel floors,
    // which are the same surface as far as the classifier is concerned. Full
    // below ~25 deg, gone by ~40 deg. A cave ROOF has normalY near -1, so it
    // falls out of this and stays rock, which is the distinction that makes a
    // tunnel read as a tunnel.
    const flat = up.smoothstep(0.76, 0.91);
    // Broken up so the floor is not a uniform stripe: 6 m 3D world noise through
    // a tight window, the same idiom StreamMountain's meadow clumps use, and for
    // the same reason -- these edges resolve per texel.
    const patchy = s.noise3World(seed ^ 0x2A, 1 / 6, 2, 0.5, 2.0)
      .smoothstep(-0.22, 0.10);
    const rubble = flat.mul(patchy);

    s.weight(CAVE_ROCK, rubble.oneMinus());
    s.weight(CAVE_RUBBLE, rubble);

    // --- appearance: depth-graded value ---------------------------------------
    // ONE lane, and it is the one that carries information. Rock darkens with
    // depth -- 0 to -1000 m maps to full to 62% value -- so a screenshot of a
    // tunnel tells you roughly how deep it is, and a shaft that drops a long way
    // reads as dropping a long way. Physically it stands in for the light that
    // does not reach; practically it is the only cue this world has for a
    // dimension the camera cannot show you.
    const depth = s.altitude.smoothstep(-1000.0, 100.0);
    const value = s.blend(0.62, 1.0, depth);
    // A little 3D grain on top so the walls are not flat under a moving light.
    const grain = s.noise3World(seed ^ 0xC4, 1 / 22, 3, 0.55, 2.0).mul(-0.16);
    const V = value.add(value.mul(grain));
    s.tint(V, V, V);
  }

  biomes() {
    // No counts anywhere and no __vegetation key: WorldSector reads this table
    // only for __terrain, so every entry below is the empty set on purpose.
    // `foothills` is the biome every sector classifies as (see
    // biomeThresholds); the rest are here because biome_at can name them.
    return {
      __terrain: { material: "dirt" },
      foothills: {},
      meadow:    {},
      mountains: {},
      ocean:     {},
    };
  }
}
