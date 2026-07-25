import { rng } from 'shared-lib/rng';

// A fist-size SDF pebble: 2-4 overlapping smoothed spheres jittered by seed.
// Same recipe as Rock at ~1/4 the size and a finer voxel grid. One baked
// variant per seed, scattered in thousands by Meadow.
//
// `size` scales the authored geometry while the voxel spacing shrinks
// RELATIVE to the pebble (spacing = 0.05 / size), so larger pebbles bake
// with proportionally more voxels and stay smooth instead of magnifying
// the size-1 facets. size = 1 reproduces the original grid exactly.
class Pebble extends Part {
  static params = { seed: 0, size: 1.0 };

  build(p) {
    const S = Math.max(0.25, p.size);
    const r = rng(2000 + p.seed);
    this.beginVoxels(Math.max(0.012, 0.05 / S));
    this.fill(r.random() < 0.5 ? MAT.stone : MAT.stoneDark);
    this.smoothing(0.04 * S);
    const blobs = 2 + r.int(3);
    for (let i = 0; i < blobs; ++i) {
      this.sphere([r.range(-0.06, 0.06) * S, r.range(0.06, 0.14) * S, r.range(-0.06, 0.06) * S],
                  r.range(0.07, 0.13) * S);
    }
    this.endVoxels();
  }
}
