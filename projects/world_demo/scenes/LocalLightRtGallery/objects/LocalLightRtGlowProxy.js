// Cosmetic presentation only.  Analytic World.lights own the source energy;
// excluding this Part from RT prevents the glow meshes from becoming duplicate
// GI emitters while the physical fixture/room geometry remains ray visible.
class LocalLightRtGlowProxy extends Part {
  build() {
    this.rayTraced(false);
    this.fill(MAT.lightWarmLow);
    for (const source of [
      [-14.5, 2.35, -0.8], [-2.5, 2.1, 0.0], [10.2, 3.35, 3.1],
      [16.5, 2.0, -3.2], [18.4, 3.0, 3.4], [8.1, 7.3, -1.0],
    ]) this.sphere(source, 0.10);
  }
}
