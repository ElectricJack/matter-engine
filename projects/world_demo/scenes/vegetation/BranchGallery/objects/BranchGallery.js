// BranchGallery: TreeBranch instances planted upright at the origin for
// close-up iteration on the branch/foliage look (trees hidden meanwhile).
// TreeBranch is a single bake, so instances differ only by yaw.
const COUNT = 4;
const SPACING = 10;

class BranchGallery extends Part {
  static requires = [{ module: 'TreeBranch' }];

  build(p) {
    for (let i = 0; i < COUNT; ++i) {
      this.pushMatrix();
      this.translate((i - (COUNT - 1) / 2) * SPACING, 0, 0);
      this.rotateY(i * (Math.PI / 2) * 0.5);
      this.placeChild('TreeBranch');
      this.popMatrix();
    }
  }
}
