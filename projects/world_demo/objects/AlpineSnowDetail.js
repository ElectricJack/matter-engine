// AlpineSnowDetail — wind-packed snow field, bound via
// defineMaterial({ detail: 'AlpineSnowDetail' }).
//
// The read: sastrugi. The base heightfield's spectrum is strongly
// ANISOTROPIC: nearly all its energy sits on wavevectors perpendicular to
// the wind axis (wind blows along (2,1); wavevectors cluster around (1,-2)
// multiples), so the field forms elongated ridges aligned WITH the wind,
// 30-60 cm apart and only ~2-3 cm tall. Small along-wind components break
// the ridges into finite tongues. Height range is deliberately tight
// (sigma ~2.3 cm) so POM reads as wind texture, not lumps.
//
// On top: soft SnowDrift domes (per-tile clumping the shared 64-sample base
// cannot provide -- these are what make the 16 Wang tiles differ), and rare
// deeply-embedded ScreeStone tips breaking through the crust, each of which
// pulls a natural wind-shadow read from the baked horizon shadows.
//
// Albedo: MAT.snow everywhere (0.90,0.90,0.95); stone tips are the only
// dark accents. The bake datum lands on the tallest stone tip (~9 cm), so
// the open snow surface sits ~6-10 cm recessed -- inside the POM cap, and
// physically right: stones stand proud of snow.
class AlpineSnowDetail extends Tileset {
  static requires = [
    ...[0, 1, 2].map(s => ({ module: 'SnowDrift',  params: { seed: s } })),
    ...[0, 1, 2, 3, 4, 5].map(s => ({ module: 'ScreeStone', params: { seed: s } })),
  ];

  build() {
    const SIZE = 2.0;
    this.tile({ size: SIZE, texelsPerMeter: 512, seed: 20260731 });

    const TAU = Math.PI * 2;
    let lcg = 0x1c9de6a3 >>> 0;
    const rnd = () => {
      lcg = (Math.imul(lcg, 1664525) + 1013904223) >>> 0;
      return lcg / 4294967296;
    };
    // Integer wavenumbers only (exactly tile-periodic). Ridge set: multiples
    // and near-neighbors of (1,-2) -- across-wind variation, along-wind
    // constancy. Breakup set: low-amplitude along-wind terms. Fine set: a
    // whisper of isotropic crust roughness.
    const harmonics = [
      // across-wind ridges (the sastrugi)
      { kx: 1, kz: -2, a: 0.016, ph: rnd() * TAU },
      { kx: 2, kz: -4, a: 0.011, ph: rnd() * TAU },
      { kx: 1, kz: -3, a: 0.009, ph: rnd() * TAU },
      { kx: 2, kz: -3, a: 0.007, ph: rnd() * TAU },
      { kx: 3, kz: -6, a: 0.006, ph: rnd() * TAU },
      { kx: 3, kz: -5, a: 0.004, ph: rnd() * TAU },
      // along-wind breakup (ridges become tongues, not rails)
      { kx: 2, kz: 1,  a: 0.004, ph: rnd() * TAU },
      { kx: 4, kz: 2,  a: 0.003, ph: rnd() * TAU },
      // faint isotropic crust
      { kx: 7, kz: 0,  a: 0.002, ph: rnd() * TAU },
      { kx: 0, kz: 7,  a: 0.002, ph: rnd() * TAU },
      { kx: 5, kz: 5,  a: 0.002, ph: rnd() * TAU },
    ];
    this.base((x, z) => {
      let h = 0;
      for (const t of harmonics) h += t.a * Math.cos(TAU * (t.kx * x + t.kz * z) / SIZE + t.ph);
      return h;
    }, MAT.snow);

    // Soft drift domes: the per-tile variation carrier. Half-embedded so only
    // 2-5 cm stands proud.
    this.layer('SnowDrift', {
      density: 1.3, scale: [0.7, 1.5], placement: 'cluster',
      physics: false, embed: 0.5,
      params: r => ({ seed: r.int(3) }),
    });
    // Rare stone tips breaking the crust: deeply embedded, ~6-9 cm proud.
    this.layer('ScreeStone', {
      density: 0.22, scale: [0.9, 1.6], placement: 'poisson',
      physics: false, embed: 0.75,
      params: r => ({ seed: r.int(6) }),
    });
  }
}
