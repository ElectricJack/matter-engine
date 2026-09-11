// Physical geometry for the native-RT local-light acceptance scene.  The
// cosmetic glow sources live in LocalLightRtGlowProxy and are intentionally a
// separate non-ray-traced Part; this Part remains visible to all RT ray kinds.
const LOCAL_LIGHT_RT_CORNER_FIXTURE = Object.freeze({
  // The source and receiver centers are separated by the x=0.8 return.  The
  // card is far enough beyond its z=3.25 end that card-to-receiver rays clear
  // the masonry instead of grazing it (the old z=4.75 card clipped the end).
  returnX: 0.8,
  returnMaxZ: 3.25,
  source: Object.freeze([-2.5, 2.1, 0.0]),
  card: Object.freeze([-0.50, 1.65, 5.75]),
  wallReceiver: Object.freeze([2.35, 1.40, 1.85]),
  sphereReceiver: Object.freeze([2.20, 0.72, 1.05]),
});

class LocalLightRtGalleryFixture extends Part {
  static params = {
    plaster: 0, limestone: 0, foundation: 0, terracotta: 0,
    iron: 0, gold: 0, clearGlass: 0, pomGround: 0,
  };

  build(p) {
    // Neutral datum under all three stations.
    this.fill(p.foundation);
    this.box([0, -0.16, 0], [21.0, 0.16, 7.0]);

    // ------------------------------------------------------------------
    // A. Two roofed rooms, joined only by a finite doorway.  The west local
    // source cannot illuminate the east-room floor through the solid returns.
    const wall = (center, half) => {
      this.fill(p.limestone);
      this.box(center, half);
    };
    this.fill(p.plaster);
    this.box([-11.0, 0.04, 0], [8.0, 0.04, 5.0]);
    wall([-19.0, 2.6, 0], [0.18, 2.6, 5.0]);
    wall([-3.0, 2.6, 0], [0.18, 2.6, 5.0]);
    wall([-11.0, 2.6, -5.0], [8.0, 2.6, 0.18]);
    wall([-11.0, 2.6, 5.0], [8.0, 2.6, 0.18]);
    wall([-11.0, 5.15, 0], [8.0, 0.12, 5.0]);
    // Partition at x=-11, with a 2.2 m doorway centred at z=0.
    wall([-11.0, 2.6, -3.05], [0.18, 2.6, 1.95]);
    wall([-11.0, 2.6, 3.05], [0.18, 2.6, 1.95]);
    wall([-11.0, 4.25, 0], [0.18, 0.95, 1.10]);
    // Alternating receivers make both the lit doorway projection and the solid
    // partition's shadow readable from the east-room camera.
    this.fill(p.plaster);
    this.box([-7.4, 0.35, -2.4], [1.25, 0.35, 1.05]);
    this.box([-7.4, 0.35, 2.4], [1.25, 0.35, 1.05]);
    this.fill(p.iron);
    this.box([-13.2, 1.1, 2.55], [0.42, 1.1, 0.42]);

    // ------------------------------------------------------------------
    // B. Saturated card and pale receiver on opposite sides of an L-return.
    // The return blocks the source-to-receiver segment; only a secondary ray
    // hitting the locally lit card can carry its red-orange response around.
    this.fill(p.plaster);
    this.box([0.8, 0.04, 0], [4.0, 0.04, 5.0]);
    wall([0.8, 2.15, 0.0], [0.16, 2.15, 3.25]);
    wall([-0.9, 2.15, -3.1], [1.70, 2.15, 0.16]);
    this.fill(p.terracotta);
    this.box(LOCAL_LIGHT_RT_CORNER_FIXTURE.card, [1.15, 1.55, 0.10]);
    this.fill(p.plaster);
    this.box(LOCAL_LIGHT_RT_CORNER_FIXTURE.wallReceiver,
             [1.30, 1.40, 0.10]);
    this.sphere(LOCAL_LIGHT_RT_CORNER_FIXTURE.sphereReceiver, 0.68);
    // A dark outline makes the pale wall receiver unambiguous in a tight A/B
    // capture without adding an emissive or alternative colored surface.
    this.fill(p.iron);
    this.box([2.35, 2.83, 1.97], [1.42, 0.045, 0.055]);
    this.box([1.00, 1.40, 1.97], [0.045, 1.43, 0.055]);
    this.box([3.70, 1.40, 1.97], [0.045, 1.43, 0.055]);

    // ------------------------------------------------------------------
    // C. Spotlight footprint plus reflective/transmissive secondary hits.
    this.fill(p.plaster);
    this.box([12.6, 0.04, 0], [6.4, 0.04, 5.0]);
    // Closed-world POM witness: a thin raised slab under the unobstructed warm
    // point at [10.2, 3.35, 3.1].  A grazing fixed camera resolves the relief;
    // native RT must retain the same local illumination as raster instead of
    // self-shadowing from the recessed POM shading point below this geometry.
    this.fill(p.pomGround);
    this.box([6.2, 0.16, 3.4], [2.15, 0.12, 1.25]);
    // Thin fins cut visibly into the blue spot cone and provide hard/soft
    // source-radius shadow boundaries.
    this.fill(p.iron);
    this.box([7.7, 1.0, -0.7], [0.18, 1.0, 1.0]);
    this.box([8.7, 0.65, -1.7], [0.16, 0.65, 0.75]);

    this.fill(p.gold);
    this.sphere([11.4, 1.25, 1.25], 1.20);
    this.fill(p.clearGlass);
    this.sphere([16.5, 1.75, -0.7], 1.10);
    // Pale target lies directly beyond the closed glass volume from the cool
    // point source at z=-3.2, making transmission visibility easy to sample.
    this.fill(p.plaster);
    this.box([16.5, 1.75, 1.65], [1.45, 1.55, 0.12]);
    // Off-camera green card for the gold/glass reflection camera.  Its light is
    // indexed at the secondary world-space hit, not by screen-space clusters.
    this.fill(p.plaster);
    this.box([19.2, 2.3, 3.8], [0.10, 2.0, 1.25]);

    // Narrow physical lamp bodies surround, but do not replace, the analytic
    // source positions.  They stay in this ordinary RT-visible Part even when
    // LocalLightRtGlowProxy is hidden or omitted.
    this.fill(p.iron);
    for (const source of [
      [-14.5, 2.35, -0.8], [-2.5, 2.1, 0.0], [10.2, 3.35, 3.1],
      [16.5, 2.0, -3.2], [18.4, 3.0, 3.4],
    ]) {
      this.cylinder([source[0], 0.08, source[2]],
                    [source[0], source[1] - 0.16, source[2]], 0.035);
      this.sphere([source[0], source[1] - 0.16, source[2]], 0.16);
    }
    // Spot housing hangs above the cone apex; the lens proxy itself is not in
    // the TLAS, so it cannot duplicate the analytic spot's energy.
    this.cylinder([8.1, 7.42, -1.0], [8.1, 7.85, -1.0], 0.11);
    this.sphere([8.1, 7.46, -1.0], 0.19);
  }
}
