import { rng } from 'shared-lib/rng';

// A grass clump: BLADES tapered mesh blades (one 5-vertex triangle strip each
// = 3 tris/blade) with a slight bend and per-blade tint variation, plus a small
// root skirt below y=0 so slope placement never leaves floating blades. A few
// seeded variants are baked once each and instanced tens of thousands of times
// by Meadow. BLADES is the per-clump density lever.
class Grass extends Part {
  static params = { seed: 0, lodBudget: 1.0 };

  // Opt-in procedural triangle-budget LOD (frame-time package, Stage 3): the
  // baker re-runs build() at each fraction; level N+1's blades are a strict
  // prefix of level N's, so LOD switches never reshuffle the field.
  static lodBudgets = [1.0, 0.3, 0.08];
  static lodAnchorSize = 0.5;   // blade height (m): full res holds until ~2 px

  build(p) {
    const r = rng(3000 + p.seed);
    const BLADES = 25 + r.int(11);      // 25-35 blades
    const SKIRT = 0.08;                 // root depth below y=0

    // Draw ALL blade parameters first so the RNG stream is identical at every
    // budget; emission below is a prefix subset.
    const blades = [];
    for (let b = 0; b < BLADES; ++b) {
      blades.push({
        ang:  r.range(0, Math.PI * 2),
        d:    r.range(0, 0.35),          // clump radius
        hgt:  r.range(0.35, 0.7),
        w:    r.range(0.02, 0.035),      // half-width at base
        lean: r.range(0.05, 0.25),       // tip lateral offset (bend)
        g:    r.range(0.75, 1.1),        // per-blade tint variation
        yaw:  r.range(0, Math.PI * 2),
      });
    }
    const count = Math.max(1, Math.ceil(p.lodBudget * BLADES));
    const widen = Math.sqrt(BLADES / count);   // coverage ~1/sqrt(k); 1.0 at full budget

    this.fill(MAT.grass);
    for (let b = 0; b < count; ++b) {
      const bl = blades[b];
      const w = bl.w * widen;
      this.tint(0.32 * bl.g, 0.55 * bl.g, 0.18 * bl.g, 0.6);
      this.pushMatrix();
      this.translate(Math.cos(bl.ang) * bl.d, 0, Math.sin(bl.ang) * bl.d);
      this.rotateY(bl.yaw);
      // 5-vertex strip: root pair (below y=0), tapered mid pair, tip = 3 tris.
      this.beginShape(SHAPE.strip);
        this.vertex(-w, -SKIRT, 0);
        this.vertex( w, -SKIRT, 0);
        this.vertex(-w * 0.6, bl.hgt * 0.55, bl.lean * 0.5);
        this.vertex( w * 0.6, bl.hgt * 0.55, bl.lean * 0.5);
        this.vertex( 0, bl.hgt, bl.lean);
      this.endShape();
      this.popMatrix();
    }
  }
}
