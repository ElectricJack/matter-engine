// ForestFloor — a Wang-tile ground atlas for Meadow. Baked once on world load
// via run_tileset_phase(opts), producing WorldData/Meadow/ForestFloor.gtex which
// the viewer samples through MaterialDef.groundTilesetSlot = 0 for material DIRT.
//
// Shared content (base + inline geometry) is toroidally wrapped at tile bounds.
// Per-tile-color variation comes from layer() scatters, which produce the 16
// unique tiles required by the de Bruijn atlas.

export default class ForestFloor extends Tileset {
  static requires = [
    ...[0, 1, 2, 3].map(s => ({ module: 'Pebble', params: { seed: s } })),
    ...[0, 1].map(s => ({ module: 'Rock',   params: { seed: s } })),
    { module: 'Twig' },
    { module: 'Leaf' },
  ];

  build() {
    // 2 m tile, 512 texels/m -> 1024 px per tile edge, 4096x4096 atlas.
    this.tile({ size: 2.0, texelsPerMeter: 512, seed: 1234 });

    // Dirt underlayer: a low-amplitude bumpy heightfield so AO has structure to
    // catch. All shared content; wraps toroidally at the tile bounds.
    this.base((x, z) => 0.03 * Math.sin(x * 2.1) * Math.cos(z * 2.1), MAT.dirt);

    // Scattered content that produces the 16 tile variants. Ordered light→heavy so
    // later layers can push earlier ones without moving them across a seam.
    this.layer('Pebble', {
      density: 120, scale: [0.4, 1.0], placement: 'poisson',
      physics: false, embed: 0.3,
      params: r => ({ seed: r.int(4) }),
    });
    this.layer('Rock', {
      density: 4, scale: [0.8, 1.6], physics: true,
      params: r => ({ seed: r.int(2) }),
    });
    this.layer('Twig', {
      density: 25, scale: [0.7, 1.3], physics: true, dropHeight: [0.1, 0.3],
    });
    this.layer('Leaf', {
      density: 200, scale: [0.8, 1.2], physics: true, dropHeight: [0.05, 0.25],
    });
  }
}
