// Fixture-only rounded PBR samples. Spheres make environment reflection and
// closed-volume transmission much easier to judge than a single broad plane.
class CastleMaterialSamples extends Part {
  static params = {
    gold: 0, agedGold: 0, clearGlass: 0, coloredGlass: 0,
    plaster: 0, limestone: 0, targetMaterial: 0,
  };

  build(p) {
    // Bright neutral cards are intentionally close enough to appear in the
    // metal reflections while remaining outside the main sample silhouette.
    this.fill(p.plaster);
    this.box([0, 2.58, 0.62], [1.48, 0.035, 0.72]);
    this.pushMatrix();
    this.translate(-1.48, 1.28, 0.82);
    this.rotateY(-0.34);
    this.box([0, 0, 0], [0.035, 1.12, 0.76]);
    this.popMatrix();

    this.fill(p.gold);
    this.sphere([-0.58, 1.52, 0.58], 0.43);
    this.fill(p.agedGold);
    this.sphere([-0.58, 0.55, 0.58], 0.34);

    // Opaque targets sit behind the closed glass volumes along the fixture's
    // default +Z sight line, proving that the sample is transmitting rather
    // than merely tinted and opaque.
    this.fill(p.targetMaterial);
    this.box([0.60, 1.52, 0.12], [0.24, 0.24, 0.08]);
    this.fill(p.limestone);
    this.box([0.60, 0.55, 0.12], [0.20, 0.20, 0.08]);
    this.fill(p.clearGlass);
    this.sphere([0.60, 1.52, 0.58], 0.43);
    this.fill(p.coloredGlass);
    this.sphere([0.60, 0.55, 0.58], 0.34);
  }
}
