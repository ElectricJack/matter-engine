class Crate extends Part {
  build(p) {
    this.fill(MAT.plaster);
    this.box([0, 0, 0], [1.5, 1.5, 1.5]);
  }
}
