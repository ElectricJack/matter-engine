// AlpineMeadowDetail — clumpy alpine meadow grass, bound via
// defineMaterial({ detail: "AlpineMeadowDetail" }).
//
// The read: dense-but-CLUMPY turf, not a lawn. Distinct MeadowTuft clumps
// (each a blade-strip tuft with its own dominant green — bright yellow-green,
// deep green, olive, dark, part-dried tan — see MeadowTuft.js) sit on bare
// soil with real gaps between them, plus sparse Pebble stones. The per-tuft
// material palettes are what deliver the "flecks of different green colors":
// the .gtex albedo comes from per-triangle materials, so neighboring clumps
// genuinely differ in hue.
//
// HEIGHT is the load-bearing channel here: tufts stand 7-20 cm proud of the
// soil while the gaps sit near the base field, so the runtime height blend
// against Scree/AlpineGround makes grass visibly grow up BETWEEN stones at
// material boundaries. The soil base keeps a modest sigma (~2.8 cm) so the
// tuft-vs-gap contrast dominates the height histogram.
//
// Per-tile variation (the 16 Wang tiles) comes from layer() scatters with
// independently seeded interiors; the soil base is shared and wraps
// toroidally.
class AlpineMeadowDetail extends Tileset {
  static requires = [
    ...[0, 1, 2, 3, 4, 5].map(s => ({ module: 'MeadowTuft', params: { seed: s } })),
    ...[0, 1, 2].map(s => ({ module: 'Pebble', params: { seed: s, size: 2.0 } })),
  ];

  build() {
    const SIZE = 2.0;
    this.tile({ size: SIZE, texelsPerMeter: 512, seed: 20260801 });

    // Soil underlayer: isotropic spectral sum with INTEGER wavenumbers only
    // (exactly SIZE-periodic, no seams) — same recipe as ForestFloor's dirt,
    // gentler: hummocks sway the tufts, they don't carry the relief.
    const TAU = Math.PI * 2;
    let lcg = 0x3d81f4a9 >>> 0;
    const rnd = () => {
      lcg = (Math.imul(lcg, 1664525) + 1013904223) >>> 0;
      return lcg / 4294967296;
    };
    const rings = [
      { amp: 0.022, ks: [[2, 0], [0, 2], [2, 1], [1, 2], [2, -1], [1, -2]] },
      { amp: 0.013, ks: [[4, 0], [0, 4], [3, 2], [2, 3], [3, -2], [2, -3]] },
      { amp: 0.006, ks: [[7, 0], [0, 7], [5, 5], [5, -5]] },
    ];
    const harmonics = [];
    for (const ring of rings) {
      const a = ring.amp / Math.sqrt(ring.ks.length / 2);
      for (const [kx, kz] of ring.ks) harmonics.push({ kx, kz, a, ph: rnd() * TAU });
    }
    this.base((x, z) => {
      let h = 0;
      for (const t of harmonics) h += t.a * Math.cos(TAU * (t.kx * x + t.kz * z) / SIZE + t.ph);
      return h;
    }, MAT.dirt);

    // Main turf: clustered tufts -> connected grassy patches with bare-soil
    // lanes between cluster centers. Snapped; the tuft's root skirt hides
    // slope contact. Native tuft is already placement-scale (blades 10-20 cm),
    // so scale ~1 keeps 7-20 cm world-space blades.
    this.layer('MeadowTuft', {
      density: 4.2, scale: [0.75, 1.15], placement: 'cluster',
      physics: false, embed: 0.0,
      params: r => ({ seed: r.int(6) }),
    });
    // Filler tufts: smaller, sparser, uniform — soften the cluster edges and
    // seed lone clumps in the soil lanes.
    this.layer('MeadowTuft', {
      density: 1.8, scale: [0.45, 0.7], placement: 'uniform',
      physics: false, embed: 0.0,
      params: r => ({ seed: r.int(6) }),
    });
    // Occasional small stones half-sunk in the turf.
    this.layer('Pebble', {
      density: 0.7, scale: [0.12, 0.28], placement: 'poisson',
      physics: false, embed: 0.35,
      params: r => ({ seed: r.int(3), size: 2.0 }),
    });
  }
}
