class GlassCrate extends Part {
  build(p) {
    this.fill(MAT.glass);
    this.box([0, 0, 0], [1.5, 1.5, 1.5]);
  }
}
