// AlpineRockDetail — exposed alpine rock face / bedding strata, a Wang-tile
// ground atlas bound via defineMaterial({ detail: 'AlpineRockDetail' }).
//
// The read: sedimentary beds cropping out on a weathered face. The base
// heightfield is a smooth tile-periodic field DOMINATED by one directional
// harmonic (wavevector (2,1) sets a consistent strike/dip across every tile),
// then terraced into ~3 cm steps — each terrace riser is a bedding plane
// edge, which is exactly the few-cm ledge scale POM + baked horizon shadows
// sell best. On top: tabular AlpineSlab fragments (broken bed pieces lying on
// the outcrop, some tan/dark so different beds show), fine ScreeStone rubble
// clustered where frost-shatter debris collects, and flat LichenPatch decals
// for the yellow-green crustose accents alpine rock always carries.
//
// Per-tile variation (the 16 Wang tiles) comes from layer() scatters, whose
// interior domains are independently seeded per tile; the terraced base is
// shared content and wraps toroidally.
//
// Height accounting: terraced base sigma ~0.045 m (range ~ +-0.10), slabs up
// to ~+0.09 proud of local base. The runtime POM relief cap (~0.12 m) clamps
// only the deepest hollow-to-crest extremes; the 3 cm strata steps live well
// inside the useful range.
class AlpineRockDetail extends Tileset {
  static requires = [
    ...[0, 1, 2, 3].map(s => ({ module: 'AlpineSlab',  params: { seed: s } })),
    ...[0, 1, 2, 3, 4, 5].map(s => ({ module: 'ScreeStone', params: { seed: s } })),
    ...[0, 1, 2, 3].map(s => ({ module: 'LichenPatch', params: { seed: s } })),
  ];

  build() {
    const SIZE = 2.0;
    this.tile({ size: SIZE, texelsPerMeter: 512, seed: 20260729 });

    const TAU = Math.PI * 2;
    let lcg = 0x51ab33c7 >>> 0;
    const rnd = () => {
      lcg = (Math.imul(lcg, 1664525) + 1013904223) >>> 0;
      return lcg / 4294967296;
    };

    // Smooth dip field g: INTEGER wavenumbers only, so it is exactly
    // SIZE-periodic (no seams). The (2,1) term dominates -> contours are
    // near-parallel wavy lines, i.e. a consistent strike direction; the
    // smaller cross terms bow and pinch the beds so they read geological,
    // not printed.
    const dip = [
      { kx: 2, kz: 1,  a: 0.040, ph: rnd() * TAU },   // dominant bedding
      { kx: 1, kz: -1, a: 0.014, ph: rnd() * TAU },
      { kx: 3, kz: 2,  a: 0.009, ph: rnd() * TAU },
      { kx: 0, kz: 2,  a: 0.007, ph: rnd() * TAU },
    ];
    // Fine post-terrace roughness so treads aren't machine-flat.
    const fine = [
      { kx: 7, kz: 0,  a: 0.0045, ph: rnd() * TAU },
      { kx: 0, kz: 7,  a: 0.0045, ph: rnd() * TAU },
      { kx: 5, kz: 5,  a: 0.0040, ph: rnd() * TAU },
      { kx: 5, kz: -5, a: 0.0040, ph: rnd() * TAU },
      { kx: 11, kz: 4, a: 0.0022, ph: rnd() * TAU },
      { kx: 4, kz: -11, a: 0.0022, ph: rnd() * TAU },
    ];

    const STEP = 0.030;   // bedding step height (m)
    this.base((x, z) => {
      let g = 0;
      for (const t of dip) g += t.a * Math.cos(TAU * (t.kx * x + t.kz * z) / SIZE + t.ph);
      // Terrace: mostly-flat tread with a gentle back-slope (0.15) and a
      // sharp riser packed into the top of each band (ft^6). Continuous at
      // band edges: floor+0.15+0.85 == floor+1.
      const t = g / STEP;
      const ft = t - Math.floor(t);
      let h = STEP * (Math.floor(t) + 0.15 * ft + 0.85 * Math.pow(ft, 6));
      for (const f of fine) h += f.a * Math.cos(TAU * (f.kx * x + f.kz * z) / SIZE + f.ph);
      return h;
    }, MAT.stone);

    // Broken bed fragments lying on the outcrop. Snapped (no settle): flat
    // slabs on terraced rock sit naturally with a fraction embedded.
    // Scale ranges are HALF the original: AlpineSlab bakes at a doubled
    // native size so its box core clears the part mesher's ~67 mm voxel
    // (see AlpineSlab.js header); world-space size is unchanged.
    this.layer('AlpineSlab', {
      density: 0.5, scale: [0.35, 0.65], placement: 'poisson',
      physics: false, embed: 0.42,
      params: r => ({ seed: r.int(4) }),
    });
    // Frost-shatter rubble collecting in cracks and against risers.
    // ScreeStone's native height grew 0.2 -> ~0.45 m (same mesher-resolution
    // reason); scales are multiplied by ~0.44 to keep 5-22 cm world rubble.
    this.layer('ScreeStone', {
      density: 2.6, scale: [0.10, 0.22], placement: 'cluster',
      physics: false, embed: 0.4,
      params: r => ({ seed: r.int(6) }),
    });
    // Crustose lichen colonies -- pure albedo accents, near-zero relief.
    this.layer('LichenPatch', {
      density: 1.0, scale: [0.6, 1.3], placement: 'cluster',
      physics: false, embed: 0.0,
      params: r => ({ seed: r.int(4) }),
    });
  }
}
