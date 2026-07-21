import { rng } from 'shared-lib/rng';

// Test-only Rock: simple sphere SDF, no retopo (avoids autoremesher crash).
class Rock extends Part {
  static params = { seed: 0, size: 1.0 };

  build(p) {
    const S = Math.max(0.02, p.size);
    const r = rng(1000 + p.seed);
    this.beginVoxels(Math.min(0.8, Math.max(0.04, 0.15 * S)));
    this.fill(MAT.rock);
    this.smoothing(0.02);
    this.sphere([0, 0.4 * S, 0], S * r.range(0.35, 0.5));
    this.sphere([r.range(-0.1, 0.1) * S, 0.3 * S, r.range(-0.1, 0.1) * S],
                S * r.range(0.3, 0.45));
    this.endVoxels();
  }
}
