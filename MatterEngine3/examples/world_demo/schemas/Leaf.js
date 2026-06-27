// A single small flattened leaf blob in leaf-green. Pure part (no children).
class Leaf extends Part {
  build(p) {
    this.beginVoxels(0.06);
    this.fill(MAT.leaf);
    this.pushMatrix();
    this.scale(1.0, 0.35, 1.0);   // flatten on Y so it reads as a leaf
    this.sphere([0, 0, 0], 0.12);
    this.popMatrix();
    this.endVoxels();
  }
}
