// Extrude gallery (mesh / None session): the general profile-sweep machine that
// cylinder/capsule/cone are circular specializations of. Exercises the four
// hard requirements of the extruder:
//   - a convex profile swept straight,
//   - a CONCAVE profile (an L) -- needs the constrained ear-clipping triangulator,
//   - a profile WITH A HOLE (begin/endContour) -> a hollow tube,
//   - the three join styles (MITER/BEVEL/ROUND) at a polyline bend.
// Profiles are authored with beginShape(SHAPE.polygon)+vertex (2D u,v; z ignored),
// retained lazily at endShape(), then consumed by extrude(path). Each item sets
// its own transform first so the path coordinates are local to that column.
class Extrusions extends Part {
  build(p) {
    const G = 3.0;            // wider pitch; extrusions are taller than the prims
    this.fill(MAT.rock);

    // --- 0: convex square swept straight up ---------------------------------
    this.pushMatrix();
    this.translate(0 * G, 0, 0);
    this.beginShape(SHAPE.polygon);          // outer CCW
      this.vertex(-0.5, -0.5);
      this.vertex( 0.5, -0.5);
      this.vertex( 0.5,  0.5);
      this.vertex(-0.5,  0.5);
    this.endShape();
    this.extrude([[0, 0, 0], [0, 2.2, 0]]);
    this.popMatrix();

    // --- 1: concave L profile (the reason we need a real triangulator) ------
    this.pushMatrix();
    this.translate(1 * G, 0, 0);
    this.beginShape(SHAPE.polygon);          // L outline, CCW (interior on left)
      this.vertex(0.0, 0.0);
      this.vertex(1.0, 0.0);
      this.vertex(1.0, 0.4);
      this.vertex(0.4, 0.4);
      this.vertex(0.4, 1.0);
      this.vertex(0.0, 1.0);
    this.endShape();
    this.extrude([[0, 0, 0], [0, 2.0, 0]]);
    this.popMatrix();

    // --- 2: square-with-hole -> a hollow tube (custom tube extrusion) -------
    this.pushMatrix();
    this.translate(2 * G, 0, 0);
    this.beginShape(SHAPE.polygon);
      this.vertex(-0.6, -0.6);               // outer CCW
      this.vertex( 0.6, -0.6);
      this.vertex( 0.6,  0.6);
      this.vertex(-0.6,  0.6);
      this.beginContour();                   // hole CW (opposite winding)
        this.vertex(-0.3, -0.3);
        this.vertex(-0.3,  0.3);
        this.vertex( 0.3,  0.3);
        this.vertex( 0.3, -0.3);
      this.endContour();
    this.endShape();
    this.extrude([[0, 0, 0], [0, 2.4, 0]]);
    this.popMatrix();

    // --- 3,4,5: join styles at a 90-degree polyline bend --------------------
    // Same small square profile swept "up then sideways"; only joinType differs,
    // so the elbow shows a sharp miter, a chamfered bevel, and a rounded fillet.
    const elbow = [[0, 0, 0], [0, 1.4, 0], [1.0, 1.4, 0]];
    const joins = [JOIN.miter, JOIN.bevel, JOIN.round];
    for (let i = 0; i < 3; ++i) {
      this.pushMatrix();
      this.translate((3 + i) * G, 0, 0);
      this.beginShape(SHAPE.polygon);
        this.vertex(-0.3, -0.3);
        this.vertex( 0.3, -0.3);
        this.vertex( 0.3,  0.3);
        this.vertex(-0.3,  0.3);
      this.endShape();
      this.joinType(joins[i]);
      this.extrude(elbow);
      this.popMatrix();
    }
  }
}
