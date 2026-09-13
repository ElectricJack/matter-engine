// One individually meshed voxel stone. Reused with affine placements, never a texture.
// A 20-cell edge preserves a slightly rounded, chipped hand-dressed silhouette.
class KreuzensteinBrick extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { seed: 0, material: 8 };
  build(p) {
    this.fill(p.material);
    const c = [
      [0.61, 0.56, 0.46],
      [0.56, 0.52, 0.44],
      [0.65, 0.59, 0.48],
      [0.59, 0.55, 0.48],
    ][p.seed];
    this.tint(...c, 1);
    this.beginModifier();
    this.beginVoxels(0.05);
    this.box([0, 0, 0], [0.485, 0.485, 0.485]);
    // Small corner losses; all six faces and the chipped edges are genuine voxel CSG.
    this.sphere([0.49, 0.48, 0.47], 0.065 + p.seed * 0.012);
    this.difference();
    this.sphere([-0.48, -0.47, 0.49], 0.07);
    this.difference();
    this.sphere([0.18, 0.49, -0.48], 0.045);
    this.difference();
    this.endVoxels();
    this.endModifier([{ simplify: 0.16 }]);
  }
}
