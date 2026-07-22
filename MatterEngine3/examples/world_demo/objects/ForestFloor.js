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

    // Dirt underlayer: uneven forest soil built from tile-periodic value
    // noise. The lattice hash wraps mod `freq`, so the field satisfies
    // f(0,z) == f(SIZE,z) and f(x,0) == f(x,SIZE) exactly (x = SIZE maps to
    // lattice cell freq == cell 0) — no seam lines. Three fBm octaves at
    // 0.67 m / 0.29 m / 0.15 m features, total amplitude ~0.12 m, so normals
    // and baked AO get soil-like relief with no directional banding.
    const vnoise = (x, z, freq, seed) => {
      const fx = x / SIZE * freq, fz = z / SIZE * freq;
      const ix = Math.floor(fx), iz = Math.floor(fz);
      const tx = fx - ix, tz = fz - iz;
      const sx = tx * tx * (3 - 2 * tx), sz = tz * tz * (3 - 2 * tz);
      const h = (i, j) => {
        i = ((i % freq) + freq) % freq;
        j = ((j % freq) + freq) % freq;
        const n = Math.sin(i * 127.1 + j * 311.7 + seed * 74.7) * 43758.5453;
        return n - Math.floor(n);
      };
      const a = h(ix, iz), b = h(ix + 1, iz), c = h(ix, iz + 1), d = h(ix + 1, iz + 1);
      return (a + (b - a) * sx + (c - a) * sz + (a - b - c + d) * sx * sz) * 2 - 1;
    };
    // Domain warp: offset the sample point by low-frequency periodic noise
    // before evaluating the fBm. Because the warp field itself tiles, the
    // warped field still tiles exactly, but the value-noise lattice loses its
    // axis alignment so no cross-hatch reads through at mid distance.
    this.base((x, z) => {
      const wx = x + 0.22 * vnoise(x, z, 2, 7);
      const wz = z + 0.22 * vnoise(x, z, 2, 8);
      return 0.065 * vnoise(wx, wz, 3, 1)
           + 0.035 * vnoise(wx, wz, 7, 2)
           + 0.016 * vnoise(x, z, 13, 3);
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
