import { rng } from 'shared-lib/rng';

// A thin tabular rock-slab fragment: a piece of a sedimentary bed that broke
// off along its bedding plane and now lies flat on the strata below. Built as
// a squat box core whose perimeter is shaved by 6-8 near-vertical plane cuts
// (irregular polygon outline, like flagstone) plus a shallow top bevel or two.
// Some seeds stack a smaller offset slab on top so the fragment itself shows
// a bedding step. Native footprint ~0.5 x 0.35 m, ~6-9 cm thick, sitting on
// y=0 for snapped (embed) placement.
//
// Seed palette: 0,2 grey stone; 1 tan sand (a weathered lighter bed); 3 dark
// stone -- so a scatter of slabs reads as fragments of *different* beds.
class AlpineSlab extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(8100 + p.seed);
    const L = 0.5 * r.range(0.9, 1.15);    // half-ish length scale base
    const W = 0.36 * r.range(0.85, 1.15);
    const TH = r.range(0.055, 0.09);       // full thickness
    const mats = [MAT.stone, MAT.sand, MAT.stone, MAT.stoneDark];

    this.beginModifier();
    const spacing = 0.016;
    this.beginVoxels(spacing);
    this.fill(mats[p.seed & 3]);
    this.smoothing(0.3 * spacing);

    const yaw0 = r.range(0, Math.PI * 2);

    // Core slab: box half-extents, resting on y=0.
    this.pushMatrix();
    this.rotateY(yaw0);
    this.box([0, TH * 0.5, 0], [L * 0.5, TH * 0.5, W * 0.5]);
    this.popMatrix();

    // Optional smaller slab stacked on top with a lateral offset: an in-part
    // bedding step (~45% of seeds).
    const stacked = r.random() < 0.45;
    let topY = TH;
    if (stacked) {
      const th2 = TH * r.range(0.6, 0.9);
      this.pushMatrix();
      this.rotateY(yaw0 + r.range(-0.25, 0.25));
      this.box([r.range(-0.12, 0.12) * L, TH + th2 * 0.5, r.range(-0.12, 0.12) * W],
               [L * 0.5 * r.range(0.55, 0.75), th2 * 0.5, W * 0.5 * r.range(0.55, 0.8)]);
      this.popMatrix();
      topY = TH + th2;
    }

    // Perimeter cuts: big boxes whose inner face passes at dist from center,
    // facing inward at a near-vertical angle -> irregular polygonal outline.
    const B = 1.6;
    const cuts = 6 + r.int(3);
    for (let i = 0; i < cuts; ++i) {
      const az = (i + r.range(-0.35, 0.35)) * (Math.PI * 2 / cuts);
      const dx = Math.cos(az), dz = Math.sin(az);
      // Distance of the cut plane from center, in the footprint's metric.
      const dist = r.range(0.30, 0.48) * (Math.abs(dx) * L + Math.abs(dz) * W);
      this.pushMatrix();
      this.translate(dx * (dist + B), topY * 0.5, dz * (dist + B));
      this.rotateY(-az + Math.PI * 0.5);       // box +X faces the center
      this.rotateZ(r.range(-0.12, 0.12));      // slight off-vertical fracture
      this.box([0, 0, 0], [B, B, B]);
      this.difference();
      this.popMatrix();
    }

    // One or two shallow top bevels: tilted boxes shaving the upper arris.
    const bevels = 1 + r.int(2);
    for (let i = 0; i < bevels; ++i) {
      const az = r.range(0, Math.PI * 2);
      const dx = Math.cos(az), dz = Math.sin(az);
      const dist = r.range(0.25, 0.42) * (Math.abs(dx) * L + Math.abs(dz) * W);
      this.pushMatrix();
      this.translate(dx * dist, topY + B * 0.92, dz * dist);
      this.rotateY(-az);
      this.rotateX(r.range(0.10, 0.22));       // tilt so the shave is a wedge
      this.box([0, 0, 0], [B, B, B]);
      this.difference();
      this.popMatrix();
    }

    this.endVoxels();
    this.endModifier([{ simplify: 0.10 }]);
  }
}
