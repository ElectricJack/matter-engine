// Direct-triangle (mesh / None session) gallery: the SAME primitive verbs as
// VoxelPrims, but here they emit real swept triangle solids instead of SDF
// brushes. This is the session-polymorphism payoff -- one verb, two
// representations -- and proves none of the round primitives fall back to the
// old "shape made of spheres" beading. No beginVoxels: in the None session the
// solids triangulate directly under the current transform/material/tint.
class MeshPrims extends Part {
  build(p) {
    const G = 2.4;
    const Y = 1.0;
    this.fill(MAT.metal);

    // 0: UV sphere
    this.sphere([0 * G, Y, 0], 0.7);

    // 1: 12-triangle box
    this.box([1 * G, Y, 0], [0.6, 0.6, 0.6]);

    // 2: swept tube via line() (hollow, smooth-tapered r0 -> r1, NOT beaded)
    this.line([2 * G - 0.5, Y - 0.6, 0], [2 * G + 0.5, Y + 0.6, 0], 0.42, 0.18);

    // 3: capsule (cylindrical wall + hemisphere caps)
    this.capsule([3 * G, Y - 0.6, 0], [3 * G, Y + 0.6, 0], 0.4);

    // 4: cylinder (flat caps)
    this.cylinder([4 * G, Y - 0.7, 0], [4 * G, Y + 0.7, 0], 0.45);

    // 5: cone (tapered to a true apex at r1 = 0)
    this.cone([5 * G, Y - 0.7, 0], [5 * G, Y + 0.9, 0], 0.55, 0.0);

    // 6: tinted box (per-triangle tint flows through TriEx in mesh mode, G4)
    this.tint(0.2, 0.6, 1.0);
    this.box([6 * G, Y, 0], [0.55, 0.55, 0.55]);
    this.tint(1, 1, 1, 0);

    // 7: a hand-authored TRIANGLE_FAN face (the loose-vertex mesh path), baked
    //    under a scale to keep it in the row.
    this.fill(MAT.leaf);
    this.pushMatrix();
    this.translate(7 * G, Y, 0);
    this.scale(0.9, 0.9, 0.9);
    this.beginShape(SHAPE.fan);
      this.vertex(0, 0, 0);
      this.vertex(-0.6, 0.0, 0.0);
      this.vertex(-0.3, 0.7, 0.0);
      this.vertex(0.3, 0.8, 0.0);
      this.vertex(0.6, 0.1, 0.0);
    this.endShape();
    this.popMatrix();
  }
}
