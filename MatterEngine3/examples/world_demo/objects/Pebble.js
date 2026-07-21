import { rng } from 'shared-lib/rng';

// A fist-size SDF pebble: 2-4 overlapping smoothed spheres jittered by seed.
// Same recipe as Rock at ~1/4 the size and a finer voxel grid. One baked
// variant per seed, scattered in thousands by Meadow.
class Pebble extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(2000 + p.seed);
    this.beginVoxels(0.05);
    this.fill(r.random() < 0.5 ? MAT.stone : MAT.stoneDark);
    this.smoothing(0.04);
    const blobs = 2 + r.int(3);
    for (let i = 0; i < blobs; ++i) {
      this.sphere([r.range(-0.06, 0.06), r.range(0.06, 0.14), r.range(-0.06, 0.06)],
                  r.range(0.07, 0.13));
    }
    this.endVoxels();
  }
}
