import { rng } from 'shared-lib/rng';

// A crustose lichen colony: a loose cluster of small flat discs hugging the
// rock surface (y ~ 2-8 mm), emitted as triangle fans like Leaf -- pure
// albedo decal, near-zero relief. Map lichen (Rhizocarpon) yellow-green
// dominates via MAT.grass with darker MAT.leaf thalli mixed in; disc rims are
// jittered per-vertex so edges read organic, not stamped. Native cluster
// ~0.2 m across; tilesets scale 0.5-1.3x.
class LichenPatch extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(8200 + p.seed);
    const discs = 6 + r.int(5);
    const SEGS = 9;

    for (let i = 0; i < discs; ++i) {
      // Cluster around origin: first disc central, rest scattered.
      const ang = r.range(0, Math.PI * 2);
      const dist = (i === 0) ? 0 : r.range(0.02, 0.10);
      const cx = Math.cos(ang) * dist;
      const cz = Math.sin(ang) * dist;
      const cy = 0.002 + i * 0.0008;          // stagger to avoid coplanar faces
      const rad = r.range(0.018, 0.055);

      this.fill(r.random() < 0.7 ? MAT.grass : MAT.leaf);
      this.beginShape(SHAPE.fan);
      this.vertex(cx, cy, cz);
      // First rim vertex must repeat at the end to close the fan.
      const t0 = r.range(0, Math.PI * 2);
      const rim = [];
      for (let s = 0; s < SEGS; ++s) {
        // t DECREASES so the fan winds with a +Y (up) face normal; the discs
        // are single-sided (MAT.grass is not double-sided like MAT.leaf).
        const t = t0 - (s / SEGS) * Math.PI * 2;
        const rr = rad * r.range(0.7, 1.25);   // ragged rim
        rim.push([cx + Math.cos(t) * rr, cy, cz + Math.sin(t) * rr]);
      }
      for (let s = 0; s < SEGS; ++s) this.vertex(rim[s][0], rim[s][1], rim[s][2]);
      this.vertex(rim[0][0], rim[0][1], rim[0][2]);
      this.endShape();
    }
  }
}
