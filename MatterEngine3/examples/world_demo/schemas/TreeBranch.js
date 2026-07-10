import { LSystem, LSystemFollower } from 'shared-lib/lsystem';
import { ParticleSim, PathRecorder } from 'shared-lib/particleflow';

// Hybrid branch: the L-system only generates a SKELETON — no geometry is
// emitted from it. Every skeleton node becomes an attractor, and particle
// strands (the trunk's follow recipe in miniature: attract + curl +
// adhere/align, turn-rate limited) grow through the cloud consuming nodes.
// The L-system provides the branching LAYOUT; the particles provide organic
// curvature, welded forks, and accreted tapering thickness. Split angles are
// jittered (0.7-1.3x) and the frame is rolled randomly at every fork, so no
// two splits match. Leaf clusters sit on the thin outer reaches of the
// recorded strand paths. Baked once and instanced many times by Tree.
class TreeBranch extends Part {
  static requires = [{ module: 'Leaf' }];

  build(p) {
    const DEG = Math.PI / 180;
    const SEED = 7;
    const VOX = 0.06;
    const S = 0.5;                  // skeleton step; same world scale as Tree

    const h01 = (k) => (((k * 2654435761) >>> 0) % 997) / 997;
    let rc = SEED * 977;
    const rnd = () => h01(rc++);

    // --- pass 1: L-system skeleton -> attractor cloud ------------------------
    const a1 = -52 * DEG;
    const a2 =  60 * DEG;
    const lsys = new LSystem();
    lsys.init('X');
    lsys.rule('X => Fwd [Rotx1 Y] [Rotx2 X]');
    lsys.rule('Y => Fwd [Roty1 X] [Roty2 Y]');
    lsys.rule('Fwd => Fwd Fwd');
    lsys.rewrite(5);                // ~80 segments / attractor nodes

    // diameters are unused (particles own thickness); maxDistance only feeds t
    const follower = new LSystemFollower(this, lsys, S, S, 13.0 * S);
    // jittered forks: random roll around the growth axis + 0.7-1.3x the
    // divergence angle, so the uniform L-system angles never show through
    follower.addAction('Rotx1', () => {
      this.rotateY((rnd() * 2 - 1) * 50 * DEG);
      this.rotateX(a1 * (0.7 + rnd() * 0.6));
    });
    follower.addAction('Rotx2', () => {
      this.rotateY((rnd() * 2 - 1) * 50 * DEG);
      this.rotateX(a2 * (0.7 + rnd() * 0.6));
    });
    follower.addAction('Roty1', () => this.rotateY(a1 * (0.7 + rnd() * 0.6)));
    follower.addAction('Roty2', () => this.rotateY(a2 * (0.7 + rnd() * 0.6)));

    // depth-0 is the bare lead run (16 segs after 5 rewrites): shorten it so
    // the branch forks early instead of reading as a slingshot on a pole
    follower.onGetForwardDistance =
      () => (follower.distStack.length === 0 ? 0.4 * S : S);

    // Record the skeleton as a graph: each segment endpoint is a node, with
    // its parent found by matching the segment start to a prior endpoint.
    // Root-to-tip chains give each strand ONE real branch path to follow —
    // plain nearest-unconsumed hopping degenerates into a single greedy tour
    // of the whole skeleton (one wand, no forks).
    const MAX_NODES = 200;
    const nodes = [];
    const parent = [], hasChild = [];
    const idxOf = new Map();
    const key = (q) => q[0].toFixed(3) + ',' + q[1].toFixed(3) + ',' + q[2].toFixed(3);
    follower.onSegment = (from, to) => {
      if (parent.length >= MAX_NODES) return;
      const pi = idxOf.has(key(from)) ? idxOf.get(key(from)) : -1;
      const idx = parent.length;
      nodes.push(to[0], to[1], to[2]);
      parent.push(pi);
      hasChild.push(false);
      if (pi >= 0) hasChild[pi] = true;
      idxOf.set(key(to), idx);
    };
    follower.execute();

    const allChains = [];
    for (let i = 0; i < parent.length; ++i) {
      if (hasChild[i]) continue;                 // not a tip
      const c = [];
      for (let n = i; n >= 0; n = parent[n]) c.push(n);
      c.reverse();
      allChains.push(c);
    }
    // Greedy DIVERSE selection: all chains are the same length, so taking
    // the first N in depth-first order picks sibling tips of ONE subtree
    // (shared prefix ~24/31 nodes -> a single wand). Instead take the chain
    // whose shared prefix with everything already picked is shortest, so
    // strands diverge at the earliest forks.
    const chains = [allChains[0]];
    while (chains.length < 8 && chains.length < allChains.length) {
      let best = null, bestShared = Infinity;
      for (const c of allChains) {
        if (chains.includes(c)) continue;
        let shared = 0;
        for (const s of chains) {
          let d = 0;
          while (d < c.length && d < s.length && c[d] === s[d]) ++d;
          if (d > shared) shared = d;
        }
        if (shared < bestShared) { bestShared = shared; best = c; }
      }
      chains.push(best);
    }

    // --- pass 2: particle strands follow the skeleton ------------------------
    const TICKS = 12000;
    const rec = new PathRecorder(VOX * 0.8, ['thickness']);
    const sim = new ParticleSim({
      seed: SEED, dt: 1.0,
      maxTurnRate: 0.15,
      speedTarget: 0.02, speedRelax: 0.15,
      depositEvery: VOX * 0.9,
      maxParticles: 16, maxAge: 1600,
      hashCell: 0.2,
      attributes: ['thickness'],
      emitters: [],
      fields: [
        // strands chase their CLAIMED chain node (claims beeline regardless
        // of influence); small influence keeps the unclaimed-gap fallback
        // from wandering off across the skeleton
        { type: 'attract', weight: 1.0, influence: 1.5, killRadius: 0.25,
          killOnConsume: false },
        { type: 'curl', weight: 0.16, scale: 1.4, seed: SEED + 3 },
        // bundle on shared runs so repeated passes accrete girth at the base
        { type: 'adhere', weight: 0.6, radius: 0.12, surfaceOffset: 0.03 },
        { type: 'align', weight: 0.3, radius: 0.18 },
        { type: 'bias', dir: [0, 1, 0], weight: 0.08 },
      ],
    }).attach(rec);
    sim.setAttractors(new Float32Array(nodes));

    // sequential emission (one strand alive at a time, like the trunk): each
    // strand walks ONE root->tip chain, claiming its next node as it goes.
    // Shared base runs get re-walked by every strand (adhere accretes girth);
    // arms diverge where the chains do — real forks.
    let emitted = 0, chain = null, at = 0, claimed = -1;
    sim.run(TICKS, 4, (v) => {
      let anyAlive = false;
      for (let i = 0; i < v.count; ++i) {
        if (!v.alive[i]) continue;
        anyAlive = true;
        v.attrs.thickness[i] *= 0.992;   // taper doubles as lifetime
        const px = v.pos[3 * i], py = v.pos[3 * i + 1], pz = v.pos[3 * i + 2];
        while (at < chain.length) {
          const n = chain[at];
          const dx = nodes[3 * n] - px, dy = nodes[3 * n + 1] - py,
                dz = nodes[3 * n + 2] - pz;
          if (dx * dx + dy * dy + dz * dz < 0.35 * 0.35) {   // reached: advance
            ++at; claimed = -1; continue;
          }
          if (claimed === n) break;                          // already chasing it
          if (sim.claimAttractor(i, n)) { claimed = n; break; }
          ++at;                          // consumed by a prior strand: skip node
        }
        if (at >= chain.length || v.attrs.thickness[i] <= VOX * 0.9) sim.kill(i);
      }
      if (!anyAlive) {
        if (emitted >= chains.length) return false;
        chain = chains[emitted]; at = 0; claimed = -1;
        const ang = emitted * 2.39996;
        sim.emit({
          center: [Math.cos(ang) * 0.04, 0, Math.sin(ang) * 0.04],
          axis: [0, 1, 0], vel0: 0.02,
          // leader chain (longest) is the fattest; side chains start thinner
          attrInit: [VOX * (1.6 + 1.4 * (chain.length / chains[0].length))],
        });
        ++emitted;
      }
    });

    // --- stamp strands into the voxel pipeline -------------------------------
    this.fill(MAT.bark);
    this.beginModifier();
    this.beginVoxels(VOX);
    this.smoothing(VOX * 1.5);
    this.paths(rec, { radiusChannel: 'thickness', minRadius: VOX * 0.85 });
    this.endVoxels();
    this.endModifier([{ simplify: 0.3 }, { smooth: { iterations: 1 } }]);

    // --- leaves on the thin outer reaches of each strand ----------------------
    // One leaf per site, sites dense near each tip. Frame per site:
    //   lookAt(forward)  -> local +Z = branch growth direction (lookAt aims +Z)
    //   rotateZ(golden)  -> spin around the forward axis: azimuth marches
    //                       evenly AROUND the tube; local +Y becomes the radial
    // Leaf geometry grows +Z with its blade flat in X/Z (normal = ±Y), so the
    // leaf points forward and its face lies along the surface normal. Small
    // jitter rotations vary the forward alignment.
    const MAX_LEAVES = 90;
    const GOLD = 137.508 * DEG;
    let leaves = 0, site = 0;
    rec.forEach((path) => {
      if (leaves >= MAX_LEAVES) return;
      const n = path.xyz.length / 3;
      if (n < 4) return;
      const thick = path.channels && path.channels.thickness;
      // dense sampling right at the tip, opening up walking back toward the
      // wood, so foliage clusters around the ends of each branch
      let step = 3;
      for (let i = n - 2; i >= 1 && leaves < MAX_LEAVES;
           i -= step, step = Math.min(step + 3, 18)) {
        if (thick && thick[i] > VOX * 1.8) break;   // back into fat wood: stop
        const px = path.xyz[3 * i], py = path.xyz[3 * i + 1],
              pz = path.xyz[3 * i + 2];
        const dx = path.xyz[3 * (i + 1)]     - path.xyz[3 * (i - 1)];
        const dy = path.xyz[3 * (i + 1) + 1] - path.xyz[3 * (i - 1) + 1];
        const dz = path.xyz[3 * (i + 1) + 2] - path.xyz[3 * (i - 1) + 2];
        const rWood = thick ? thick[i] : VOX;
        this.pushMatrix();
        this.translate(px, py, pz);
        this.lookAt([px + dx, py + dy, pz + dz]);
        this.rotateZ(site * GOLD + (rnd() * 40 - 20) * DEG);
        // sit the leaf base OUTSIDE the wood, sticking off the branch
        this.translate(0, rWood + VOX * 1.5, 0);
        this.rotateX(-(rnd() * 30 + 8) * DEG);   // tip pitches outward a bit
        this.rotateY((rnd() * 30 - 15) * DEG);   // yaw off exact forward
        this.placeChild('Leaf');
        this.popMatrix();
        ++leaves; ++site;
      }
    });
  }
}
