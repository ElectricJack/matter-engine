// FloorDemo — minimal world root that draws a flat 16x16m dirt quad (material
// 16 = DIRT) so the ForestFloor tileset atlas gets Wang-sampled onto it by
// the raster/raytrace shader when MaterialDef.groundTilesetSlot >= 0.
// No scatter, no assembly children — just enough geometry to see the atlas.
class FloorDemo extends Part {
  build(p) {
    this.fill(MAT.dirt);
    const S = 8.0;   // half-extent -> 16 m x 16 m quad = 8 x 8 Wang tiles (tile = 2 m)
    // Two-triangle floor at y=0.
    this.beginShape(SHAPE.triangles);
      this.vertex(-S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0, -S);
      this.vertex( S, 0, -S); this.vertex(-S, 0,  S); this.vertex( S, 0,  S);
    this.endShape();
  }
}
