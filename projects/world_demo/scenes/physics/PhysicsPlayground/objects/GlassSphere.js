class GlassSphere extends Part {
  build(p) {
    this.fill(MAT.glass);
    this.sphere([0, 0, 0], 1.2);
  }
}
