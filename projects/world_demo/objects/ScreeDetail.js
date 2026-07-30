// ScreeDetail — talus / scree: densely packed angular stones settled into
// natural contact piles, bound via defineMaterial({ detail: 'ScreeDetail' }).
//
// The signature of scree is stone-on-stone height variation with almost no
// exposed matrix, so this tileset leans on the box3d settle: three physics
// layers of sharp ScreeStone fragments drop in coarse->fine order (large
// frames first, medium packs around them, small chips rattle into the gaps
// last, exactly how talus actually sorts on a slope's surface). A gently
// undulating dirt base (sigma ~3.5 cm) gives the piles macro-structure and
// shows only as dark slivers between stones.
//
// Per-tile variation: every layer's interior domains are independently
// seeded per Wang tile, and the settle runs on the full 4x4 torus, so all 16
// tiles get genuinely different pile arrangements while edge strips stay
// seam-consistent.
//
// Height accounting: settled piles span roughly 0.25-0.35 m crest-to-gap;
// the POM relief cap (~0.12 m) clamps the deepest gaps flat, which reads as
// shadowed voids between stones -- the right call per the relief-cap design
// note ("parallax needs ~10 cm to sell relief; deeper detail is real
// geometry's job").
class ScreeDetail extends Tileset {
  static requires = [
    ...[0, 1, 2, 3, 4, 5].map(s => ({ module: 'ScreeStone', params: { seed: s } })),
  ];

  build() {
    const SIZE = 2.0;
    this.tile({ size: SIZE, texelsPerMeter: 512, seed: 20260730 });

    // Dirt underlayer: isotropic spectral sum (integer wavenumbers -> exactly
    // tile-periodic), sigma ~0.035 m. Same recipe as ForestFloor's soil, less
    // energy: the stones carry the relief here, the base only sways the piles.
    const TAU = Math.PI * 2;
    let lcg = 0x7be91d05 >>> 0;
    const rnd = () => {
      lcg = (Math.imul(lcg, 1664525) + 1013904223) >>> 0;
      return lcg / 4294967296;
    };
    const rings = [
      { amp: 0.028, ks: [[2, 0], [0, 2], [2, 1], [1, 2], [2, -1], [1, -2]] },
      { amp: 0.016, ks: [[4, 0], [0, 4], [3, 2], [2, 3], [3, -2], [2, -3]] },
      { amp: 0.007, ks: [[7, 0], [0, 7], [5, 5], [5, -5]] },
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

    // Stones, coarse -> fine so later drops pack into earlier gaps.
    // Native ScreeStone height ~0.2 m at scale 1.
    this.layer('ScreeStone', {              // frame stones, 20-35 cm
      density: 4.5, scale: [1.0, 1.6], placement: 'poisson',
      physics: true,
      params: r => ({ seed: r.int(6) }),
    });
    this.layer('ScreeStone', {              // packing stones, 12-21 cm
      density: 13.0, scale: [0.6, 1.05], placement: 'uniform',
      physics: true,
      params: r => ({ seed: r.int(6) }),
    });
    this.layer('ScreeStone', {              // chips and shards, 7-12 cm
      density: 24.0, scale: [0.35, 0.62], placement: 'uniform',
      physics: true,
      params: r => ({ seed: r.int(6) }),
    });
  }
}
