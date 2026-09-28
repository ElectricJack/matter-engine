// Shared geometry for the local-light stress and isolation worlds.
// mode 0 = 17x17 stress gallery, 1 = point isolation, 2 = spot isolation.
class LocalLightGalleryFixture extends Part {
  build(p) {
    const mode = p.mode || 0;
    const halfExtent = mode === 0 ? 37 : 10;

    this.fill(MAT.charcoal);
    this.box([0, -0.18, 0], [halfExtent, 0.18, halfExtent]);

    // Pale receivers make finite radius and cone edges easy to read with all
    // sun/sky terms suppressed in the World definitions.
    this.fill(MAT.plaster);
    this.box([0, 0.05, 0], [halfExtent - 0.5, 0.05, halfExtent - 0.5]);
    this.box([0, 4.0, -halfExtent + 0.5],
             [halfExtent - 0.5, 4.0, 0.12]);

    // A curved metallic receiver is the regression target for local GGX/F0.
    this.fill(MAT.goldRough);
    this.sphere([0, 1.65, 0], mode === 0 ? 1.6 : 2.2);

    this.fill(MAT.chrome);
    this.sphere([mode === 0 ? 5.0 : 4.5, 1.25, 0], 1.2);

    // Simple occluder-like fins establish depth and show that baseline raster
    // local lights are intentionally unshadowed (documented in the scene
    // README); the RT follow-up owns local visibility.
    this.fill(MAT.stoneDark);
    const finCount = mode === 0 ? 9 : 3;
    for (let i = 0; i < finCount; ++i) {
      const x = (i - (finCount - 1) * 0.5) * 5.0;
      this.box([x, 1.0, 6.0], [0.18, 1.0, 1.5]);
    }
  }
}
