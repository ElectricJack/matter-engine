// ForestFloor — a Wang-tile ground atlas for Meadow. Baked once on world load
// via run_tileset_phase(opts), producing WorldData/Meadow/ForestFloor.gtex which
// the viewer samples through MaterialDef.groundTilesetSlot = 0 for material DIRT.
//
// Shared content (base + inline geometry) is toroidally wrapped at tile bounds.
// Per-tile-color variation comes from layer() scatters, which produce the 16
// unique tiles required by the de Bruijn atlas.

class ForestFloor extends Tileset {
  static requires = [
    ...[0, 1, 2, 3].map(s => ({ module: 'Pebble', params: { seed: s, size: 2.0 } })),
    ...[0, 1].map(s => ({ module: 'Rock',   params: { seed: s, size: 1.2, detail: 2.5 } })),
    ...[0, 1, 2].map(s => ({ module: 'Twig', params: { seed: s, size: 4.0 } })),
    { module: 'Leaf' },
  ];

  build() {
    // 2 m tile, 512 texels/m -> 1024 px per tile edge, 4096x4096 atlas.
    const SIZE = 2.0;
    this.tile({ size: SIZE, texelsPerMeter: 512, seed: 1234 });

    // Dirt underlayer: uneven forest soil built from a spectral sum of
    // rotated harmonic pairs. Every term is cos(2*pi*(kx*x + kz*z)/SIZE + ph)
    // with INTEGER wavenumbers (kx, kz), so the field is exactly SIZE-periodic
    // in both axes by construction — no seam lines, no lattice. The previous
    // value-noise fBm read as corduroy: its grid-aligned lattice produced
    // visible diagonal rows of hollows. Here the wavevectors are chosen on
    // near-circular rings at many distinct angles (isotropic power spectrum),
    // with pseudo-random phases from a tiny LCG, so no direction dominates
    // and nothing repeats visibly inside one tile.
    //
    // Amplitude accounting: within each ring every term gets
    // amp / sqrt(n/2), so the ring's variance sums to amp^2 (sigma == amp).
    // Total sigma = sqrt(0.038^2 + 0.022^2 + 0.012^2 + 0.006^2) ~= 0.046 m;
    // measured over a 512^2 grid the field spans [-0.142, +0.132] m — the
    // deepest hollows land in the -0.10..-0.15 m band, inside the POM
    // relief cap's useful range.
    const TAU = Math.PI * 2;
    let lcg = 0x2f6e2b1 >>> 0;
    const rnd = () => {
      lcg = (Math.imul(lcg, 1664525) + 1013904223) >>> 0;
      return lcg / 4294967296;
    };
    // Rings: [wavevectors...] at |k| ~ 2 (1 m features), ~4 (0.5 m),
    // ~7 (0.3 m), ~12 (0.17 m). (kx,kz) and (-kx,-kz) alias the same cosine,
    // so each list only spans a half-plane (kx > 0, or kx == 0 && kz > 0).
    const rings = [
      { amp: 0.038, ks: [[2, 0], [0, 2], [2, 1], [1, 2], [2, -1], [1, -2]] },
      { amp: 0.022, ks: [[4, 0], [0, 4], [3, 2], [2, 3], [3, -2], [2, -3], [4, 1], [1, -4]] },
      { amp: 0.012, ks: [[7, 0], [0, 7], [6, 3], [3, 6], [6, -3], [3, -6], [5, 5], [5, -5]] },
      { amp: 0.006, ks: [[12, 0], [0, 12], [11, 4], [4, 11], [11, -4], [4, -11], [9, 8], [8, -9]] },
    ];
    const harmonics = [];
    for (const ring of rings) {
      const a = ring.amp / Math.sqrt(ring.ks.length / 2);
      for (const [kx, kz] of ring.ks) {
        harmonics.push({ kx, kz, a, ph: rnd() * TAU });
      }
    }
    this.base((x, z) => {
      let h = 0;
      for (const t of harmonics) {
        h += t.a * Math.cos(TAU * (t.kx * x + t.kz * z) / SIZE + t.ph);
      }
      return h;
    }, MAT.dirt);

    // Litter layers. Ordered light->heavy so later layers can push earlier
    // ones without moving them across a seam.
    //
    // Physics is opt-in per layer; only Rock (chunky, must sit on uneven
    // terrain) uses it for the final bake. Everything else snaps to
    // base_height + fit_half_height - embed * 2 * fit_half_height.
    //
    // Scale notes (final size = authored native size * layer scale):
    //   Pebble : baked at size 2.0 (~0.5 m blob, fine grid) -> 0.4-1.0x -> 20-50 cm
    //   Rock   : baked at size 1.2, detail 2.5              -> 0.5-0.9x -> 0.6-1.1 m
    //   Twig   : baked at size 4.0 (~0.7-1.4 m, voxelized)  -> 0.6-1.1x
    //   Leaf   : native ~1 m blade                          -> 0.15-0.28x -> 15-28 cm
    //
    // Distribution: cluster placement (gaussian clumps around uniform centers)
    // for pebbles/twigs/leaves reads as litter drifts; rocks go sparse poisson.
    this.layer('Pebble', {
      density: 1.8, scale: [0.4, 1.0], placement: 'cluster',
      physics: false, embed: 0.2,
      params: r => ({ seed: r.int(4), size: 2.0 }),
    });
    this.layer('Rock', {
      density: 0.35, scale: [0.5, 0.9], placement: 'poisson',
      physics: false, embed: 0.15,
      params: r => ({ seed: r.int(2), size: 1.2, detail: 2.5 }),
    });
    this.layer('Twig', {
      density: 1.0, scale: [0.7, 1.2], placement: 'cluster',
      physics: false, embed: 0.05,
      params: r => ({ seed: r.int(3), size: 4.0 }),
    });
    this.layer('Leaf', {
      density: 4.0, scale: [0.16, 0.3], placement: 'cluster',
      physics: false, embed: 0.0,
    });
  }
}
