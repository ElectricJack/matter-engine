import { rng } from 'shared-lib/rng';

// Test-only Pebble: 2-3 smoothed spheres, no retopo.
class Pebble extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(2000 + p.seed);
    this.beginVoxels(0.05);
    this.fill(MAT.stone);
    this.smoothing(0.04);
    const blobs = 2 + r.int(2);
    for (let i = 0; i < blobs; ++i) {
      this.sphere([r.range(-0.06, 0.06), r.range(0.06, 0.14), r.range(-0.06, 0.06)],
                  r.range(0.07, 0.13));
    }
    this.endVoxels();
  }
}
