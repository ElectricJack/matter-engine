import { rng } from 'shared-lib/rng';

// A soft wind-packed snow mound: 3-4 heavily smoothed, Y-squashed SDF spheres
// blended into one low dome ~0.35-0.5 m across and ~7-10 cm tall. Deliberately
// round-ish (no long axis): tileset layer() placements draw a random yaw, so
// an elongated drift would tumble against the wind direction baked into the
// sastrugi base. Domes stay orientation-agnostic and just add the soft height
// clumping the 64-sample base grid cannot carry. Sits on y=0; layers embed it
// ~half so only 2-5 cm stands proud.
class SnowDrift extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(8300 + p.seed);
    this.beginVoxels(0.02);
    this.fill(MAT.snow);
    this.smoothing(0.06);                     // very soft blends -- packed snow
    const blobs = 3 + r.int(2);
    for (let i = 0; i < blobs; ++i) {
      const ang = r.range(0, Math.PI * 2);
      const dist = (i === 0) ? 0 : r.range(0.03, 0.12);
      const rad = r.range(0.10, 0.16);
      this.pushMatrix();
      this.translate(Math.cos(ang) * dist, rad * r.range(0.28, 0.40), Math.sin(ang) * dist);
      this.scale(r.range(1.0, 1.3), r.range(0.30, 0.42), r.range(1.0, 1.3));
      this.sphere([0, 0, 0], rad);
      this.popMatrix();
    }
    this.endVoxels();
  }
}
