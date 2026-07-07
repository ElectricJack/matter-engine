// ForestFloor — a Wang-tile ground atlas for Meadow. Baked once on world load
// via run_tileset_phase(opts), producing WorldData/Meadow/ForestFloor.gtex which
// the viewer samples through MaterialDef.groundTilesetSlot = 0 for material DIRT.
//
// Shared content (base + inline geometry) is toroidally wrapped at tile bounds.
// Per-tile-color variation comes from layer() scatters, which produce the 16
// unique tiles required by the de Bruijn atlas.

class ForestFloor extends Tileset {
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
    //
    // Physics is opt-in per layer. It only pays off for chunky objects whose
    // rest pose actually depends on collision (Rock — solid, needs to sit on
    // uneven terrain without clipping and can push its neighbours). Pebble,
    // Twig, Leaf are small/thin enough that algorithmic surface-snap gives a
    // visually equivalent result at a fraction of the settle cost:
    //   settle cost scales roughly O(N) per box3d step * ~1000 steps, so at
    //   349 bodies/m^2 * 64 m^2 = ~22k dynamic bodies the current stack does
    //   not converge in a useful wall-clock. Rock alone at 4/m^2 * 64 = 256
    //   bodies settles in seconds. Everything else uses `physics: false` and
    //   snaps to base_height + fit_half_height - embed * 2 * fit_half_height.
    // Note on scales: these part scripts (Pebble/Rock/Twig/Leaf) were originally
    // authored for larger-scale placements. Ground-litter sizes are much smaller
    // than their native unit dimensions, so the layer scale is expressed as a
    // multiplier that brings the object into the intended physical size:
    //   Pebble : native ~10cm ball  -> 0.4-1.0x -> 4-10cm pebbles
    //   Rock   : native ~30cm blob  -> 0.4-0.8x -> 12-24cm rocks
    //   Twig   : native ~20cm stick -> 0.4-1.0x -> 8-20cm twigs
    //   Leaf   : native ~1m blade   -> 0.03-0.06x -> 3-6cm leaves
    this.layer('Pebble', {
      density: 60, scale: [0.4, 1.0], placement: 'poisson',
      physics: false, embed: 0.3,
      params: r => ({ seed: r.int(4) }),
    });
    this.layer('Rock', {
      density: 2, scale: [0.4, 0.8], physics: true,
      params: r => ({ seed: r.int(2) }),
    });
    this.layer('Twig', {
      density: 15, scale: [0.4, 1.0], physics: false, embed: 0.02,
    });
    this.layer('Leaf', {
      density: 80, scale: [0.03, 0.06], physics: false, embed: 0.0,
    });
  }
}
