// PhysicsPlayground root — a simple 1x1x1m box used as the visual geometry
// for the procedurally generated dynamic crate entities.
class Crate extends Part {
  build(p) {
    this.fill(MAT.dirt);
    this.box(0, 0.5, 0, 1, 1, 1);
  }
}
