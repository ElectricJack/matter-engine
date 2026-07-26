// Voxel-session gallery: every iso-primitive brush plus the postfix-CSG and
// transform behaviors, laid out left-to-right in ONE marching-cubes field. The
// brushes are spaced apart so they mesh as disjoint surface components (a far
// subtract brush has no effect on a distant solid), letting one bake show them
// all side by side.
//
// Postfix CSG model (important): sphere()/box()/etc. always emit a Union brush;
// difference()/intersection() retroactively retag the LAST-emitted brush. So a
// carve reads as "emit the carving brush, then call difference()".
class VoxelPrims extends Part {
  build(p) {
    const G = 2.4;            // column pitch (world units)
    const Y = 1.0;            // lift everything off the ground plane
    this.fill(MAT.stone);
    this.beginVoxels(0.08);

    // --- Native iso-primitives, one per column ------------------------------
    // 0: sphere (the classic hot path)
    this.sphere([0 * G, Y, 0], 0.7);

    // 1: oriented box (a real sdBox brush now, not a cubic sphere stamp)
    this.box([1 * G, Y, 0], [0.6, 0.6, 0.6]);

    // 2: capsule (sdSegment - r)
    this.capsule([2 * G - 0.5, Y - 0.5, 0], [2 * G + 0.5, Y + 0.5, 0], 0.4);

    // 3: cylinder (capped cone with equal radii)
    this.cylinder([3 * G, Y - 0.7, 0], [3 * G, Y + 0.7, 0], 0.45);

    // 4: cone (tapered capped cone; r1 = 0 -> a true point)
    this.cone([4 * G, Y - 0.7, 0], [4 * G, Y + 0.8, 0], 0.55, 0.0);

    // 5: line() in a voxel session lowers to a capsule (G1), proving the verb is
    //    session-polymorphic rather than mesh-only.
    this.line([5 * G - 0.5, Y - 0.5, 0], [5 * G + 0.5, Y + 0.5, 0], 0.35);

    // --- Transform-stack correctness (G2): a non-uniform scale must thicken the
    // box extents, proving the brush is mapped into scaled brush-local space
    // rather than the radius/extent being applied in unscaled world space. ----
    this.pushMatrix();
    this.translate(6 * G, Y, 0);
    this.scale(1.6, 0.5, 1.0);
    this.box([0, 0, 0], [0.5, 0.5, 0.5]);   // renders as a flat, wide slab
    this.popMatrix();

    // --- Tint: a fat-primitive brush carries a per-brush tint (G4). ----------
    this.tint(0.9, 0.3, 0.2);
    this.box([7 * G, Y, 0], [0.55, 0.55, 0.55]);
    this.tint(1, 1, 1, 0);                  // reset to neutral (alpha 0)

    // --- Ordered CSG: add -> subtract -> add. The base cube gets a bite carved
    // out, THEN a knob is added back. If stage ordering were lost (old flat
    // bag-of-adds / bag-of-subtracts), the knob would also be carved away. Its
    // survival is the visual proof that operation order is preserved. ---------
    const cx = 8 * G;
    this.box([cx, Y, 0], [0.7, 0.7, 0.7]);          // add: base cube
    this.sphere([cx + 0.5, Y + 0.5, 0.5], 0.7);     // (emitted as Union...)
    this.difference();                              // ...retagged to subtract -> bite
    this.sphere([cx + 0.5, Y + 0.5, 0.5], 0.28);    // add back: knob inside the bite

    // --- Intersection: box AND sphere = a rounded cube. ---------------------
    const ix = 9 * G;
    this.box([ix, Y, 0], [0.6, 0.6, 0.6]);
    this.sphere([ix, Y, 0], 0.78);
    this.intersection();                            // retag the sphere -> intersect

    this.endVoxels();
  }
}
