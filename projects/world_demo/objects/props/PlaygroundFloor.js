// PhysicsPlayground root — a flat 40x40m stone floor used as the visual
// counterpart to the "floor-body" static RigidBody/BoxCollider entity so the
// physics playground has something to render crates falling onto.
class PlaygroundFloor extends Part {
  build(p) {
    this.fill(MAT.stone);
    const S = 20.0;
    this.beginShape(SHAPE.triangles);
      this.vertex(-S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0, -S);
      this.vertex( S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0,  S);
    this.endShape();
  }
}
