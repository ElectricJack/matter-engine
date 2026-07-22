import { rng } from 'shared-lib/rng';

// Lightweight Meadow — a fixed 48x48 m flat dirt ground with scattered rocks.
// Purpose: verification stage for the Vulkan ground-tileset work (spec
// 2026-07-21): the ForestFloor .gtex binds to MAT.dirt, so the ground quad
// shows the Wang-tile atlas; rocks give parallax/shadow reference points.
// No trees, no grass, no terrain tiles, no streaming. The original 816x816
// world is preserved at examples/world_demo/backup/Meadow.objects.js.bak.

const S = 24.0;                 // half-extent: ground spans [-24, 24]^2
const ROCK_VARIANTS = 4;
const ROCKS = 30;               // ordinary rocks
const BOULDER_SIZES = [2.5, 4.0], BOULDER_SEEDS = 2, BOULDERS = 5;
const PEBBLE_VARIANTS = 4;
const PEBBLES = 40;             // a few physical pebbles vs. the baked-in ones
const GRASS_VARIANTS = 5;
// ~50% grass coverage: 48x48 m = 2304 m^2, target ~1150 m^2 of grass.
// ~90 clumps x ~14 tufts x ~0.8 m^2 visual footprint per tuft (scale ~2)
// lands in that band while keeping clumped, patchy structure.
const GRASS_CLUMPS = 90;          // clump centers
const GRASS_PER_CLUMP = [10, 18]; // tufts per clump (range)
const GRASS_CLUMP_RADIUS = 1.2;   // tuft spread around the center

function makeRequires() {
  const req = [];
  req.push({ module: 'MeadowGround' });
  for (let s = 0; s < ROCK_VARIANTS; ++s) req.push({ module: 'Rock', params: { seed: s } });
  for (const sz of BOULDER_SIZES)
    for (let s = 0; s < BOULDER_SEEDS; ++s)
      req.push({ module: 'Rock', params: { seed: s, size: sz } });
  for (let s = 0; s < PEBBLE_VARIANTS; ++s) req.push({ module: 'Pebble', params: { seed: s } });
  for (let s = 0; s < GRASS_VARIANTS; ++s) req.push({ module: 'Grass', params: { seed: s } });
  return req;
}

class Meadow extends Part {
  static params = { worldSeed: 20260721 };
  static requires = function(p) { return makeRequires(); };

  build(p) {
    const r = rng(p.worldSeed);

    // Ground is its own part: expanded roots emit child instances only, so
    // inline root triangles would be dropped (that bit us — the "ground" in
    // early screenshots was the sky gradient).
    this.placeChild('MeadowGround');

    // Ground-follow placement on the flat plane (sink slightly so bases
    // don't float on the texture).
    const put = (module, params, x, z, s, sinkY) => {
      this.pushMatrix();
      this.translate(x, -sinkY, z);
      this.rotateY(r.range(0, Math.PI * 2));
      this.scale(s, s, s);
      this.placeChild(module, params);
      this.popMatrix();
    };

    const M = S - 2.0;          // keep scatter off the very edge
    for (let i = 0; i < ROCKS; ++i) {
      const s = r.range(0.6, 1.8);
      put('Rock', { seed: r.int(ROCK_VARIANTS) },
          r.range(-M, M), r.range(-M, M), s, 0.15 * s);
    }
    for (let i = 0; i < BOULDERS; ++i) {
      const sz = BOULDER_SIZES[r.int(BOULDER_SIZES.length)];
      const s = r.range(0.8, 1.2);
      put('Rock', { seed: r.int(BOULDER_SEEDS), size: sz },
          r.range(-M, M), r.range(-M, M), s, 0.15 * sz * s);
    }
    for (let i = 0; i < PEBBLES; ++i) {
      put('Pebble', { seed: r.int(PEBBLE_VARIANTS) },
          r.range(-M, M), r.range(-M, M), r.range(0.5, 1.5), 0.02);
    }

    // Large grass clumps: cluster tufts around scattered centers so they
    // read as patches rather than confetti.
    for (let c = 0; c < GRASS_CLUMPS; ++c) {
      const cx = r.range(-M, M), cz = r.range(-M, M);
      const n = GRASS_PER_CLUMP[0] +
                r.int(GRASS_PER_CLUMP[1] - GRASS_PER_CLUMP[0] + 1);
      for (let i = 0; i < n; ++i) {
        const a = r.range(0, Math.PI * 2);
        const d = GRASS_CLUMP_RADIUS * Math.sqrt(r.random());
        const x = Math.max(-M, Math.min(M, cx + Math.cos(a) * d));
        const z = Math.max(-M, Math.min(M, cz + Math.sin(a) * d));
        put('Grass', { seed: r.int(GRASS_VARIANTS) },
            x, z, r.range(1.6, 2.6), 0.02);
      }
    }
  }
}
