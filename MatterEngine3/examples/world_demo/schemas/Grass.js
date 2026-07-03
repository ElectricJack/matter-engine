import { rng } from 'shared-lib/rng';

// A grass clump: BLADES tapered mesh blades (one 5-vertex triangle strip each
// = 3 tris/blade) with a slight bend and per-blade tint variation, plus a small
// root skirt below y=0 so slope placement never leaves floating blades. A few
// seeded variants are baked once each and instanced tens of thousands of times
// by Meadow. BLADES is the per-clump density lever.
class Grass extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(3000 + p.seed);
    const BLADES = 25 + r.int(11);      // 25-35 blades
    const SKIRT = 0.08;                 // root depth below y=0

    this.fill(MAT.grass);
    for (let b = 0; b < BLADES; ++b) {
      const ang = r.range(0, Math.PI * 2);
      const d = r.range(0, 0.35);       // clump radius
      const hgt = r.range(0.35, 0.7);
      const w = r.range(0.02, 0.035);   // half-width at base
      const lean = r.range(0.05, 0.25); // tip lateral offset (bend)
      const g = r.range(0.75, 1.1);     // per-blade tint variation

      this.tint(0.32 * g, 0.55 * g, 0.18 * g, 0.6);
      this.pushMatrix();
      this.translate(Math.cos(ang) * d, 0, Math.sin(ang) * d);
      this.rotateY(r.range(0, Math.PI * 2));
      // 5-vertex strip: root pair (below y=0), tapered mid pair, tip = 3 tris.
      this.beginShape(SHAPE.strip);
        this.vertex(-w, -SKIRT, 0);
        this.vertex( w, -SKIRT, 0);
        this.vertex(-w * 0.6, hgt * 0.55, lean * 0.5);
        this.vertex( w * 0.6, hgt * 0.55, lean * 0.5);
        this.vertex( 0, hgt, lean);
      this.endShape();
      this.popMatrix();
    }
  }
}
