// Flat 48x48 m dirt ground for the lightweight Meadow verification world.
// Own part (not inline root geometry): expanded world roots emit child
// instances only, so a floor must be placed like any other child. MAT.dirt is
// the material the ForestFloor tileset slot binds to, so this quad is the
// surface that shows the Wang-tile atlas.
class MeadowGround extends Part {
  build(p) {
    this.fill(MAT.dirt);
    const S = 24.0;
    this.beginShape(SHAPE.triangles);
      this.vertex(-S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0, -S);
      this.vertex( S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0,  S);
    this.endShape();
  }
}
