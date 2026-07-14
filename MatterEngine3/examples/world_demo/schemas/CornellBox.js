// CornellBox — classic RT validation scene: 5-wall room (open front) with
// red left wall, green right wall, white back/ceiling, semi-reflective
// floor, two white boxes inside, and an emissive ceiling light panel.
class CornellBox extends Part {
  build(p) {
    const S = 10.0;
    const H = 20.0;

    const quad = (a, b, c, d) => {
      this.vertex(a[0], a[1], a[2]);
      this.vertex(b[0], b[1], b[2]);
      this.vertex(c[0], c[1], c[2]);
      this.vertex(c[0], c[1], c[2]);
      this.vertex(d[0], d[1], d[2]);
      this.vertex(a[0], a[1], a[2]);
    };

    const solidBox = (cx, cz, hx, h, hz) => {
      const x0 = cx - hx, x1 = cx + hx;
      const z0 = cz - hz, z1 = cz + hz;
      quad([x0,0,z1],[x1,0,z1],[x1,h,z1],[x0,h,z1]);
      quad([x1,0,z0],[x0,0,z0],[x0,h,z0],[x1,h,z0]);
      quad([x1,0,z1],[x1,0,z0],[x1,h,z0],[x1,h,z1]);
      quad([x0,0,z0],[x0,0,z1],[x0,h,z1],[x0,h,z0]);
      quad([x0,h,z0],[x0,h,z1],[x1,h,z1],[x1,h,z0]);
    };

    // Floor: stone with light gray tint (matte, not mirror)
    this.fill(MAT.stone);
    this.tint(0.9, 0.9, 0.9, 1.0);
    this.beginShape(SHAPE.triangles);
    quad([-S,0,-S],[-S,0,S],[S,0,S],[S,0,-S]);
    this.endShape();

    // (no ceiling — open top lets sunlight in)

    // Back wall (-Z): white stone
    this.fill(MAT.snow);
    this.tint(1.0, 1.0, 1.0, 0.0);
    this.beginShape(SHAPE.triangles);
    quad([-S,0,-S],[S,0,-S],[S,H,-S],[-S,H,-S]);
    this.endShape();

    // Left wall (-X): red
    this.fill(MAT.stone);
    this.tint(0.85, 0.08, 0.08, 1.0);
    this.beginShape(SHAPE.triangles);
    quad([-S,0,S],[-S,0,-S],[-S,H,-S],[-S,H,S]);
    this.endShape();

    // Right wall (+X): green
    this.tint(0.08, 0.85, 0.08, 1.0);
    this.beginShape(SHAPE.triangles);
    quad([S,0,-S],[S,0,S],[S,H,S],[S,H,-S]);
    this.endShape();

    // Ceiling light panel (emissive)
    this.fill(MAT.light);
    this.tint(1.0, 1.0, 0.95, 1.0);
    const L = 3.0;
    this.beginShape(SHAPE.triangles);
    quad([-L,H-0.01,-L],[L,H-0.01,-L],[L,H-0.01,L],[-L,H-0.01,L]);
    this.endShape();

    // Tall box (left-rear): white stone
    this.fill(MAT.stone);
    this.tint(0.9, 0.9, 0.9, 1.0);
    this.beginShape(SHAPE.triangles);
    solidBox(-3.5, -2.0, 2.5, 14.0, 2.5);
    this.endShape();

    // Short box (right-front): white stone
    this.beginShape(SHAPE.triangles);
    solidBox(4.0, 2.5, 2.5, 6.0, 2.5);
    this.endShape();
  }
}
