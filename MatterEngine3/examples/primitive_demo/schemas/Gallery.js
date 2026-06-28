// Root of the primitive test world. Composes the three sub-galleries via
// placeChild (instancing + the transform stack), each on its own row, and adds
// a root-level mesh demo for the two transform verbs that the sub-galleries
// don't cover: lookAt (orient +Z at a target) and tint. Baked as one artifact
// whose child table points at the three sub-gallery artifacts.
class Gallery extends Part {
  static requires = [
    { module: 'VoxelPrims' },
    { module: 'MeshPrims' },
    { module: 'Extrusions' },
  ];

  build(p) {
    // Each sub-gallery lays its items along +X; stack the galleries along -Z so
    // the three rows don't overlap. placeChild bakes the child at the current
    // matrix-stack top, exactly like Tree -> TreeBranch.
    const row = (mod, z) => {
      this.pushMatrix();
      this.translate(0, 0, z);
      this.placeChild(mod);
      this.popMatrix();
    };
    row('VoxelPrims', 0);
    row('MeshPrims', -3.0);
    row('Extrusions', -6.5);

    // --- Root mesh demo: lookAt + tint --------------------------------------
    // A pointer cone whose +Z axis is aimed by lookAt at a target marker, so the
    // cone visibly points at the little sphere. Tinted so it reads against the
    // metal/rock of the rows.
    this.fill(MAT.light);
    const target = [-3.0, 2.5, -3.0];

    // Target marker (a small untinted sphere sitting at the aim point).
    this.pushMatrix();
    this.translate(target[0], target[1], target[2]);
    this.sphere([0, 0, 0], 0.18);
    this.popMatrix();

    // The aiming cone: stand it at a fixed spot, orient +Z toward the target,
    // then sweep the cone along +Z (a -> b along local +Z).
    this.pushMatrix();
    this.translate(-3.0, 1.0, 1.0);
    this.lookAt(target);
    this.tint(1.0, 0.85, 0.2);
    this.cone([0, 0, 0], [0, 0, 1.4], 0.35, 0.0);
    this.tint(1, 1, 1, 0);
    this.popMatrix();
  }
}
