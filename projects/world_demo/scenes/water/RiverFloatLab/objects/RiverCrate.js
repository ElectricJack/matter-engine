// Scene-local 1.5 m crate. Other worlds keep using the shared 3 m Crate.
class RiverCrate extends Part {
  build() {
    this.fill(MAT.plaster);
    this.box([0, 0, 0], [0.75, 0.75, 0.75]);
  }
}
