import { ParticleSim, PathRecorder } from 'shared-lib/particleflow';
import { ellipsoidCloud } from 'shared-lib/strands';

// Particle-flow tree, sequential strand growth:
//   - Strands are emitted ONE AT A TIME from the base. Each climbs upward
//     adhering to the wood laid down by its predecessors, so the trunk is the
//     accumulated bundle — naturally fattest at the base, thinning as strands
//     peel off above.
//   - Each strand carries its own branch height (branch_y state, hashed from
//     the emission counter — slot indices get reused, so they can't seed
//     randomness). Once past it, the strand looks for the nearest unclaimed
//     crown attractor with an open-space ray probe (depositNear) and flips to
//     branch mode: space colonization — it grows toward and CONSUMES nearest
//     attractors (killOnConsume:false hops it attractor-to-attractor),
//     tapering as it goes until it dies at twig thickness. The next strand is
//     emitted as soon as the previous one branches or dies.
//   - ROOTS run in a completely SEPARATE sim with their own recorder: their
//     wood never enters the trunk sim's deposit field, so climbers can't
//     latch onto radiating root lines (which split the trunk into stems).
// Recorded paths are stamped as tapered cones into the voxel session, so the
// isosurface -> modifier (simplify/smooth/retopo) pipeline is unchanged.
class Tree extends Part {
  static params = { seed: 42 };
  static requires = [{ module: 'TreeBranch' }];

  build(p) {
    const AGE   = 1.0;              // growth knob: scales strand count
    const SEED  = (p && p.seed !== undefined) ? p.seed : 42;
    const VOX   = 0.07;

    // --- crown attractor cloud ---------------------------------------------
    // underside reaches y=3 (= MIN_BRANCH_Y): low climbers get real targets,
    // so peel-offs spread over the whole trunk instead of whorling at the top
    const CROWN_C = [0, 7.5, 0];
    const CROWN_R = [6.5, 4.5, 6.5];
    // min spacing 1.3: coarse, evenly spread targets — limbs reach instead of clump
    const attractors = ellipsoidCloud(SEED * 7 + 1, 120, CROWN_C, CROWN_R, 1.3);

    // --- policy knobs --------------------------------------------------------
    const MIN_BRANCH_Y = 3.0;       // no limbs below this height
    const MAX_FOLLOW_Y = 7.5;       // climbers past this are culled
    const BRANCH_SEE   = 6.0;       // how far a climber can see an attractor
    const LOS_R        = VOX * 3;   // open-space probe radius along the ray
    const STRANDS      = Math.round(40 * AGE);   // trunk strands; only some limb
    const LIMB_P       = 0.3;       // chance a strand ever branches (~12 limbs);
                                    // the rest die in the bundle as trunk wood
    const ROOTS        = Math.round(16 * AGE);
    const TICKS        = 60000;     // budget; run stops early when all strands die
    const BASE_R       = 0.31;      // basal emission spread: tight ring — the
                                    // strands weld into ONE column by proximity

    // --- strand sim ----------------------------------------------------------
    const rec = new PathRecorder(VOX * 0.8, ['thickness']);
    const sim = new ParticleSim({
      seed: SEED, dt: 1.0,
      maxTurnRate: 0.12,            // supple enough to be steered into limbs, no kinks
      speedTarget: 0.02, speedRelax: 0.15,
      depositEvery: VOX * 0.9,      // deposited spacing ~ voxel size
      maxParticles: 256,            // climbers + forked children + roots
      maxAge: 1400,                 // cap: unsticks stalls, outlives big scaffolds
      hashCell: 0.25,
      attributes: ['thickness'],
      state: ['follow_w', 'branch_w', 'branch_y', 'fork_t', 'limb_f'],
      emitters: [],                 // all emission is sequential, from onTick
      fields: [
        // climbers: hug the existing wood HARDER than they seek "up", so they
        // track the bundle's actual lean/forks instead of shooting straight;
        // shared position-based curl bends the whole bundle coherently
        { type: 'bias', dir: [0, 1, 0], weight: 0.6, weightState: 'follow_w' },
        { type: 'curl', weight: 0.2, scale: 3.0, seed: SEED + 7,
          weightState: 'follow_w' },
        // radius MUST stay smaller than the bundle width: a query that sees
        // through the whole column averages to the trunk AXIS, and every
        // strand collapses onto the same line — the trunk then never fattens.
        // A small radius sees only the near surface patch, so each strand
        // stacks onto the outside of the wood on its own side.
        // surfaceOffset is the trunk-girth lever: strands ride this far
        // OUTSIDE the existing wood, so each pass accretes a wider bundle
        { type: 'adhere', weight: 1.35, radius: 0.2, surfaceOffset: 0.07,
          weightState: 'follow_w' },
        // align WITH the forward direction of prior runs: adhere alone is
        // positional, so a climber can orbit the column while staying glued
        // to it. Align reads the heading stored with each deposit and keeps
        // followers traveling the way the bundle travels. Kept BELOW adhere:
        // girth comes from adhere-stacking, align only stops orbiting.
        { type: 'align', weight: 0.5, radius: 0.3, weightState: 'follow_w' },
        // near-zero steer, real killRadius: EVERY particle consumes the
        // attractors it passes, so the crown interior gets eaten clear
        { type: 'attract', weight: 0.05, influence: 1.0, killRadius: 0.4,
          killOnConsume: false, weightState: 'follow_w' },
        // branched: space colonization — chase + consume nearest attractors,
        // hopping through the crown (killOnConsume:false) so limbs run long
        { type: 'attract', weight: 1.2, influence: BRANCH_SEE, killRadius: 0.4,
          killOnConsume: false, weightState: 'branch_w' },
        // REPULSION from all previously-deposited paths: negative adhere pushes
        // a branched limb out of the trunk/limb bundle into open space
        { type: 'adhere', weight: -0.9, radius: 1.1, surfaceOffset: 0,
          weightState: 'branch_w' },
        { type: 'separate', weight: 0.7, radius: 0.45, weightState: 'branch_w' },
        { type: 'curl', weight: 0.14, scale: 2.0, seed: SEED + 3,
          weightState: 'branch_w' },
        // gentle lift so limbs arch upward instead of looping back inward
        { type: 'bias', dir: [0, 1, 0], weight: 0.22, weightState: 'branch_w' },
      ],
    }).attach(rec);
    sim.setAttractors(attractors);

    const h01 = (k) => (((k * 2654435761) >>> 0) % 997) / 997;
    // fork gate: a limb splits only when it has tapered to FORK_RATIO of its
    // thickness at the LAST split — geometric spacing (~5 units of travel
    // between forks) instead of a threshold ladder that fires in bursts
    const FORK_RATIO = 0.62;
    const CHILD      = 0.65;        // each fork arm is ~65% of the parent
    const MIN_TWIG   = VOX * 1.0;   // arms thinner than this never spawn
    let emitted = 0;
    sim.run(TICKS, 4, (v) => {
      let growing = false;          // a climber is mid-growth: hold emission
      let anyAlive = false;
      for (let i = 0; i < v.count; ++i) {
        if (!v.alive[i]) continue;
        anyAlive = true;
        const p = [v.pos[3 * i], v.pos[3 * i + 1], v.pos[3 * i + 2]];
        if (v.state.branch_w[i] > 0) {
          // taper doubles as a lifetime: the limb dies at twig thickness,
          // which stops old limbs weaving endlessly through the crown
          v.attrs.thickness[i] *= 0.99;
          // SPLIT when the taper reaches the per-limb fork gate: the parent
          // effectively dies and TWO ~65% arms form — one is a fresh emit
          // (+kick), the other is the parent slot itself dropped to 65%
          // girth with the opposite kick. Real dichotomous forks.
          if (v.state.fork_t[i] > 0 &&
              v.attrs.thickness[i] < v.state.fork_t[i]) {
            const child = v.attrs.thickness[i] * CHILD;
            if (child > MIN_TWIG) {
              const s = Math.hypot(v.vel[3 * i], v.vel[3 * i + 1],
                                   v.vel[3 * i + 2]) || 0.02;
              const bx = v.vel[3 * i] / s, by = v.vel[3 * i + 1] / s,
                    bz = v.vel[3 * i + 2] / s;
              const kk = (v.tick * 131 + i * 7) | 0;
              const kx = (h01(kk) * 2 - 1) * 0.9;
              const ky = (h01(kk + 1) * 2 - 1) * 0.9;
              const kz = (h01(kk + 2) * 2 - 1) * 0.9;
              let dx = bx + kx, dy = by + ky + 0.15, dz = bz + kz;
              const n = Math.hypot(dx, dy, dz) || 1;
              sim.emit({
                center: [p[0], p[1], p[2]],
                axis: [dx / n, dy / n, dz / n], vel0: 0.02,
                attrInit: [child],
                stateInit: [0, 1, 0, child * FORK_RATIO, 0],
              });
              let ex = bx - kx, ey = by - ky + 0.15, ez = bz - kz;
              const en = Math.hypot(ex, ey, ez) || 1;
              v.vel[3 * i]     = ex / en * s;
              v.vel[3 * i + 1] = ey / en * s;
              v.vel[3 * i + 2] = ez / en * s;
              v.attrs.thickness[i] = child;   // parent becomes the other arm
            }
            // re-arm (or disarm at twig scale) either way
            v.state.fork_t[i] = child > MIN_TWIG
              ? v.attrs.thickness[i] * FORK_RATIO : 0;
          }
          if (v.attrs.thickness[i] <= VOX * 0.9 ||
              !sim.nearestAttractor(p, BRANCH_SEE, false)) sim.kill(i);
          continue;
        }
        // climber
        const y = p[1];
        if (y > MAX_FOLLOW_Y) { sim.kill(i); continue; }  // summit guard
        // continuous gentle taper: strands start ~10% fatter at the base and
        // thin as they climb (on top of the 65% cuts at branch forks)
        v.attrs.thickness[i] *= 0.998;
        growing = true;
        if (y < v.state.branch_y[i]) continue;
        if (!v.state.limb_f[i]) {
          // trunk-only strand: its job was girth — die inside the bundle at
          // its staggered height, which is what tapers the trunk
          sim.kill(i);
          continue;
        }
        const near = sim.nearestAttractor(p, BRANCH_SEE, true);
        if (!near) continue;
        // line-of-sight: the ray to the attractor must cross open space
        let open = true;
        for (const t of [0.35, 0.6, 0.85]) {
          const q = [p[0] + (near.pos[0] - p[0]) * t,
                     p[1] + (near.pos[1] - p[1]) * t,
                     p[2] + (near.pos[2] - p[2]) * t];
          if (sim.depositNear(q, LOS_R) > 0) { open = false; break; }
        }
        if (!open || !sim.claimAttractor(i, near.idx)) continue;
        v.state.follow_w[i] = 0;    // stop climbing/adhering
        v.state.branch_w[i] = 1;    // space colonization from here on
        // thickness hierarchy: low peel-offs become beefy scaffold limbs
        // (~1.7x, live long via taper-lifetime), top peel-offs start twiggy
        v.attrs.thickness[i] *=
          1.7 - ((y - MIN_BRANCH_Y) / (MAX_FOLLOW_Y - MIN_BRANCH_Y));
        v.state.fork_t[i] = v.attrs.thickness[i] * FORK_RATIO;  // arm the gate
        growing = false;            // branched: free the emission slot
      }
      // Climbers that never find an attractor still matter: they add trunk
      // mass before dying at the summit, so emission runs the full count.
      const done = emitted >= STRANDS;
      if (!growing && !done) {
        const k = emitted;
        // golden-angle: uniform circumferential coverage, so trunk girth
        // accumulates evenly all around
        const ang = k * 2.39996;
        const rr = Math.sqrt(h01(k * 3 + 2)) * BASE_R;
        const r01 = h01(k * 3 + 3);
        // r*r biases heights low: big scaffold limbs (and most trunk-wood
        // deaths) near the base — that's the trunk taper
        const hB = MIN_BRANCH_Y + r01 * r01 * (MAX_FOLLOW_Y - MIN_BRANCH_Y);
        const isLimb = h01(k * 5 + 4) < LIMB_P ? 1 : 0;
        sim.emit({
          center: [Math.cos(ang) * rr, -0.2, Math.sin(ang) * rr],
          axis: [0, 1, 0], vel0: 0.02,
          attrInit: [VOX * 2.86], stateInit: [1, 0, hB, 0, isLimb],
        });
        ++emitted;
      } else if (!anyAlive && done) {
        return false;               // forest grown: stop the sim early
      }
    });

    // --- root sim: COMPLETELY separate particles and paths -------------------
    // Roots share nothing with the trunk sim — separate deposit field,
    // separate recorder — so there is zero interaction between root wood and
    // climbing strands. All roots launch at once (concurrently), radiating
    // outward/down with vertical hops, repelled from their own prior wood.
    const rootRec = new PathRecorder(VOX * 0.8, ['thickness']);
    const rootSim = new ParticleSim({
      seed: SEED + 11, dt: 1.0,
      maxTurnRate: 0.12,
      speedTarget: 0.02, speedRelax: 0.15,
      depositEvery: VOX * 0.9,
      maxParticles: 32, maxAge: 800,
      hashCell: 0.25,
      attributes: ['thickness'],
      emitters: [],
      fields: [
        { type: 'bias', dir: [0, -1, 0], weight: 0.16 },
        // negative adhere: repelled from prior ROOT wood — spreads the fan
        { type: 'adhere', weight: -0.4, radius: 1.0, surfaceOffset: 0 },
        { type: 'separate', weight: 0.5, radius: 0.5 },
        { type: 'curl', weight: 0.25, scale: 1.2, seed: SEED + 5 },
      ],
    }).attach(rootRec);
    let rootsOut = false;
    rootSim.run(4000, 4, (v) => {
      let anyAlive = false;
      for (let i = 0; i < v.count; ++i) {
        if (!v.alive[i]) continue;
        anyAlive = true;
        const y = v.pos[3 * i + 1];
        v.attrs.thickness[i] *= 0.978;   // ~4-5 units of squiggly reach
        // vertical hop band: kick up at the trough, dive at the crest, so
        // roots weave in and out of the soil as they run away from the trunk
        const yv = v.vel[3 * i + 1];
        if (y < -0.45 && yv < 0) v.vel[3 * i + 1] = 0.010;
        else if (y > 0.15 && yv > 0) v.vel[3 * i + 1] = -0.010;
        if (v.attrs.thickness[i] <= VOX * 0.9 || y < -2.0) rootSim.kill(i);
      }
      if (!rootsOut) {
        for (let k = 0; k < ROOTS; ++k) {
          const ang = k * 2.39996;
          const dx = Math.cos(ang), dz = Math.sin(ang);
          const n = Math.hypot(dx, -0.5, dz);
          rootSim.emit({
            center: [dx * BASE_R * 0.8, -0.1, dz * BASE_R * 0.8],
            axis: [dx / n, -0.5 / n, dz / n], vel0: 0.02,
            attrInit: [VOX * (2.6 + h01(k * 3 + 2) * 1.2)],
          });
        }
        rootsOut = true;
      } else if (!anyAlive) {
        return false;               // all roots finished
      }
    });

    // --- stamp strands into the voxel pipeline ------------------------------
    this.fill(MAT.bark);
    this.beginModifier();
    this.beginVoxels(VOX);
    this.smoothing(VOX * 1.5);      // smooth-union welds tangent strand tubes into clean flesh
    this.paths(rec, { radiusChannel: 'thickness', minRadius: VOX * 0.85 });
    this.paths(rootRec, { radiusChannel: 'thickness', minRadius: VOX * 0.85 });
    this.endVoxels();
    this.endModifier([
      { simplify: 0.3 },
      { smooth: { iterations: 2 } },
      // TODO(tuning): re-enable retopo once the shape settles — the current
      // strand geometry trips an unbounded loop in autoremesher (repeated
      // halfedge), so it's out of the stack while iterating on the tree shape
      // { retopo: { target_ratio: 1.0, iterations: 3, seed: 42, timeout_seconds: 120 } },
    ]);

    // --- branches on the limbs (same frame recipe as TreeBranch's leaves) ----
    // Walk each LIMB path from its tip back toward the trunk. Per site:
    //   lookAt(forward) -> +Z = limb direction; rotateZ(golden) -> azimuth
    //   marches evenly around the limb, +Y = radial; rotateX(+~40°) sweeps
    //   the branch (grows +Y) from straight-radial toward the limb tip.
    // Sites cluster at limb ends (step opens up walking back). Paths whose
    // endpoint sits inside the trunk bundle (trunk-girth strands) are skipped.
    const DEG = Math.PI / 180;
    const GOLD = 137.508 * DEG;
    const MAX_BRANCHES = 60;
    let rb = SEED * 131;
    const rnd = () => h01(rb++);
    let branches = 0, site = 0;
    rec.forEach((path) => {
      if (branches >= MAX_BRANCHES) return;
      const n = path.xyz.length / 3;
      if (n < 8) return;
      const thick = path.channels && path.channels.thickness;
      // limb test: tip must reach out into the crown, not die in the trunk
      const ex = path.xyz[3 * (n - 1)],     ey = path.xyz[3 * (n - 1) + 1],
            ez = path.xyz[3 * (n - 1) + 2];
      if (Math.hypot(ex, ez) < 1.2 || ey < MIN_BRANCH_Y) return;
      let step = 12;
      for (let i = n - 2; i >= 1 && branches < MAX_BRANCHES;
           i -= step, step = Math.min(step + 8, 40)) {
        if (thick && thick[i] > VOX * 2.5) break;   // into scaffold wood: stop
        const px = path.xyz[3 * i], py = path.xyz[3 * i + 1],
              pz = path.xyz[3 * i + 2];
        if (py < MIN_BRANCH_Y) break;
        const dx = path.xyz[3 * (i + 1)]     - path.xyz[3 * (i - 1)];
        const dy = path.xyz[3 * (i + 1) + 1] - path.xyz[3 * (i - 1) + 1];
        const dz = path.xyz[3 * (i + 1) + 2] - path.xyz[3 * (i - 1) + 2];
        this.pushMatrix();
        this.translate(px, py, pz);
        this.lookAt([px + dx, py + dy, pz + dz]);
        this.rotateZ(site * GOLD + (rnd() * 40 - 20) * DEG);
        this.rotateX((30 + rnd() * 30) * DEG);   // sweep toward the limb tip
        this.scale(0.35, 0.35, 0.35);
        this.placeChild('TreeBranch');
        this.popMatrix();
        ++branches; ++site;
      }
    });
  }
}
