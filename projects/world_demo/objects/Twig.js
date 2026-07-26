import { rng } from 'shared-lib/rng';

// A small fallen twig scattered by ForestFloor. size = 1 keeps the original
// cheap path: a short tapered swept-tube line() (6-sided — fine at ~20 cm).
// size > 1 switches to a voxelized build: line() inside beginVoxels is a
// smooth SDF capsule, so big twigs get a round cross-section plus a slight
// kink and a stub side-branch instead of a magnified hex tube. Radius grows
// sub-linearly (size^0.7) so long twigs stay twig-thin, not log-fat.
class Twig extends Part {
  static params = { seed: 0, size: 1.0 };

  build(p) {
    const S = Math.max(0.25, p.size);
    const r = rng(5000 + p.seed);
    const len = r.range(0.18, 0.34) * S;
    const rad = r.range(0.008, 0.015) * Math.pow(S, 0.7);

    if (S <= 1.001) {
      this.fill(MAT.bark);
      this.line([-len * 0.5, 0, 0], [len * 0.5, 0, 0], rad, rad * 0.6);
      return;
    }

    const kink = len * r.range(0.03, 0.08);        // sideways bend at the joint
    const joint = len * r.range(-0.15, 0.15);      // where the kink sits
    this.beginVoxels(Math.max(0.006, rad * 0.55));
    this.fill(MAT.bark);
    this.smoothing(rad * 0.7);
    // Two shaft capsules meeting at a kinked joint; the far half tapers.
    this.line([-len * 0.5, 0, 0], [joint, 0, kink], rad, rad);
    this.line([joint, 0, kink], [len * 0.5, 0, -kink * 0.5], rad * 0.8, rad * 0.8);
    // Stub side-branch near the thick end.
    const bx = -len * r.range(0.15, 0.35);
    const blen = len * r.range(0.12, 0.22);
    const bz = (r.random() < 0.5 ? 1 : -1) * blen * 0.8;
    this.line([bx, 0, 0], [bx + blen * 0.6, rad * 1.5, bz], rad * 0.6, rad * 0.6);
    this.endVoxels();
  }
}
