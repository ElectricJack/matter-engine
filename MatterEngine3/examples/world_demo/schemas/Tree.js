import { LSystem, LSystemFollower } from 'shared-lib/lsystem';
import { sub, add, scale, length, normalize, cross } from 'shared-lib/vecmath';

// Port of MatterEngine2's Tree.cs, then reworked into a flowing two-layer trunk.
// Pass 1: an L-system follower walks a branching skeleton and records trunk
// segments (with tapering stroke widths); at bracket depth > 2 it stops the
// voxel trunk and places a TreeBranch twig instead. Pass 2 sweeps a "flowing"
// core (sphere stack with a smooth height-based sway so the trunk curves instead
// of standing rigid). Pass 3 releases wandering "bark" strands that climb the
// trunk surface, winding around it (half one way, half the other) so they
// criss-cross into a woven, irregular bark texture.
class Tree extends Part {
  static requires = [{ module: 'TreeBranch' }];

  build(p) {
    const DEG = Math.PI / 180;
    // Scaled up from the original 0.25 so the bark resolves with more detail.
    const S = 0.5;
    const a1 = -35 * DEG;
    const a2 =  45 * DEG;

    // Heavy mesh simplification on the voxel trunk (keep 30% => ~70% reduction).
    // QEM preserves high-curvature bark ribs while collapsing the flatter trunk
    // body. Branches opt in too (TreeBranch.js); leaves never call simplify().
    this.simplify(0.3);

    // --- prototype tunables (world units, sized for S = 0.5) ----------------
    const BARK_RATIO  = 1 / 8;    // bark dab radius = local trunk radius * 1/8
    const VOX         = 0.07;     // voxel grid spacing (must resolve bark bumps)
    const BARK_MIN_R  = 0.32;     // only bark trunk thicker than this (world units)
    const CORE_INSET  = 0.9;      // shrink core so bark strands protrude
    // Wandering bark strands.
    const SPIRAL      = 1.4;      // base winding steepness (1.0 ~= 45deg helix)
    const WANDER      = 2.2;      // extra random angular walk per step (criss-cross)
    const RAD_WANDER  = 0.30;     // radial wobble as a fraction of bark radius
    // Flowing-core sway: lateral wander as a function of height.
    const SWAY_AMP    = 0.40;     // max lateral offset (world units)
    const SWAY_FREQ   = 0.42;     // vertical wavelength of the sway
    // Base flare: trunk widens toward the ground.
    const FLARE       = 0.7;      // extra radius fraction at the very base
    const FLARE_H     = 3.0;      // height over which the flare fades out
    // ------------------------------------------------------------------------

    const lsys = new LSystem();
    lsys.init('X');
    lsys.rule('X => Fwd [Rotx1 X] [Rotx2 X] [Rotz1 X] X [Rotz2 X]');
    lsys.rule('Fwd => Fwd Fwd');
    lsys.rewrite(4);

    const follower = new LSystemFollower(this, lsys, 4.0 * S, 1.0 * S, 50.0 * S);
    follower.addAction('Rotx1', () => this.rotateX(a1));
    follower.addAction('Roty1', () => this.rotateY(a1));
    follower.addAction('Rotz1', () => this.rotateZ(a1));
    follower.addAction('Rotx2', () => this.rotateX(a2));
    follower.addAction('Roty2', () => this.rotateY(a2));
    follower.addAction('Rotz2', () => this.rotateZ(a2));

    // Safety guard (v2 had none): cap placed twigs so a deep rewrite can't OOM.
    const MAX_BRANCHES = 110;
    let branches = 0;

    follower.onGetForwardDistance = () => (2.0 + Math.random() * 3.6) * S;

    follower.onBeginForward = (t, dist, depth) => {
      if (depth > 2) {
        // Place a TreeBranch at EVERY crown node (not just the first after a
        // trunk draw), so the canopy is a dense thicket of branches rather than
        // a sparse handful of tips. Capped at MAX_BRANCHES to bound the bake.
        if (branches < MAX_BRANCHES) {
          this.pushMatrix();
          // Splay each branch outward: a random azimuth plus an outward tilt off
          // the (near-vertical) crown direction, so branches radiate to fill the
          // canopy volume instead of all shooting straight up in a clump.
          this.rotateY(Math.random() * 360 * DEG);
          this.rotateX((30 + Math.random() * 45) * DEG);
          this.placeChild('TreeBranch');
          this.popMatrix();
          ++branches;
        }
        return false;          // stop the voxel trunk here; the twig takes over
      }
      return true;
    };

    follower.onEndForward = () => {
      this.rotateX((Math.random() * 6 - 3) * DEG);
      this.rotateZ((Math.random() * 4 - 2) * DEG);
    };

    // Pass 1: record the trunk skeleton (absolute local-frame segments).
    const segs = [];
    follower.onSegment = (from, to, rFrom, rTo) => segs.push([from, to, rFrom, rTo]);
    follower.execute();

    // Smooth height-based sway, applied to BOTH the core and the bark so the two
    // layers stay registered (bark sits on the swayed core, not the straight one).
    const swayOf = (y) => [
      SWAY_AMP * Math.sin(SWAY_FREQ * y + 0.7),
      0,
      SWAY_AMP * 0.7 * Math.cos(SWAY_FREQ * 0.8 * y + 1.9),
    ];
    // Base flare multiplier on the core radius.
    const flareOf = (y) => 1 + FLARE * Math.max(0, 1 - y / FLARE_H);
    // An orthonormal frame around a (near-vertical) tangent for ring placement.
    const frameOf = (dir) => {
      const t = normalize(dir);
      const ref = Math.abs(t[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
      const n1 = normalize(cross(t, ref));
      const n2 = cross(t, n1);
      return [n1, n2];
    };

    this.fill(MAT.bark);
    this.beginVoxels(VOX);

    // Pass 2: flowing core. Stroke widths are DIAMETERS, so the sweep radius is
    // half the width; the sway curves the stack and the flare thickens the base.
    const coreStep = 0.30;
    for (const [from, to, wFrom, wTo] of segs) {
      const dir = sub(to, from);
      const steps = Math.max(1, Math.floor(length(dir) / coreStep));
      const step = scale(dir, 1 / steps);
      let pos = from.slice();
      for (let k = 0; k < steps; ++k) {
        const f = steps > 1 ? k / (steps - 1) : 0;
        const r = (wFrom + (wTo - wFrom) * f) * 0.5 * flareOf(pos[1]) * CORE_INSET;
        const c = add(pos, swayOf(pos[1]));
        this.sphere(c, r);
        pos = add(pos, step);
      }
    }

    // Pass 3: wandering bark strands. For each thick segment, release many
    // crawler strands that climb the trunk while winding around it. Strands
    // alternate winding direction (cw vs ccw) so they criss-cross, and each
    // strand's angle random-walks as it climbs, so the surface reads as
    // irregular woven bark rather than regular ribs.
    const TWO_PI = Math.PI * 2;
    for (const [from, to, wFrom, wTo] of segs) {
      const r0 = wFrom * 0.5, r1 = wTo * 0.5;
      if (Math.max(r0, r1) < BARK_MIN_R) continue;
      const dir = sub(to, from);
      const segLen = length(dir);
      if (segLen < 1e-4) continue;
      const tdir = scale(dir, 1 / segLen);
      const [n1, n2] = frameOf(dir);

      const avgR  = (r0 + r1) * 0.5;
      const barkA = avgR * BARK_RATIO;
      const stepLen = Math.max(barkA * 0.6, 0.02);
      const steps = Math.max(2, Math.floor(segLen / stepLen));
      // One strand per ~1.6 bark widths of circumference, at least 8.
      const strands = Math.max(8, Math.round((TWO_PI * avgR) / (barkA * 1.6)));

      for (let s = 0; s < strands; ++s) {
        const cw = (s % 2 === 0) ? 1 : -1;            // alternate winding
        let ang = (s / strands) * TWO_PI + Math.random() * 0.6;
        for (let k = 0; k <= steps; ++k) {
          const f = k / steps;
          const hy = from[1] + segLen * f;
          const coreR = (r0 + (r1 - r0) * f) * flareOf(hy);
          const center = add(add(from, scale(tdir, segLen * f)), swayOf(hy));
          const barkR = coreR * BARK_RATIO;
          // Wind around: tangential travel ~ climb travel for a diagonal strand,
          // so the per-step angle advance scales as stepLen / coreR.
          const dAng = stepLen / Math.max(coreR, 1e-3);
          ang += cw * SPIRAL * dAng + (Math.random() - 0.5) * WANDER * dAng;
          const out = coreR * CORE_INSET
                    + barkR * (0.5 + RAD_WANDER * (Math.random() - 0.5));
          const pos = add(center,
                          add(scale(n1, Math.cos(ang) * out),
                              scale(n2, Math.sin(ang) * out)));
          this.sphere(pos, barkR * (0.8 + 0.4 * Math.random()));
        }
      }
    }

    this.endVoxels();
  }
}
