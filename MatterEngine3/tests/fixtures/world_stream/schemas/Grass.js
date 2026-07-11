import { rng } from 'shared-lib/rng';

// Test-only Grass: small blade cluster, mesh strips (safe, no voxels/retopo).
class Grass extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(3000 + p.seed);
    this.fill(MAT.grass);
    for (let b = 0; b < 5; ++b) {
      const ang = r.range(0, Math.PI * 2);
      const d   = r.range(0, 0.2);
      const h   = r.range(0.25, 0.5);
      const w   = 0.025;
      this.pushMatrix();
      this.translate(Math.cos(ang) * d, 0, Math.sin(ang) * d);
      this.rotateY(r.range(0, Math.PI * 2));
      this.beginShape(SHAPE.strip);
        this.vertex(-w, -0.05, 0);
        this.vertex( w, -0.05, 0);
        this.vertex(-w * 0.6, h * 0.5, 0.05);
        this.vertex( w * 0.6, h * 0.5, 0.05);
        this.vertex(0, h, 0.1);
      this.endShape();
      this.popMatrix();
    }
  }
}
