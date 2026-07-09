import { ParticleSim, PathRecorder } from 'shared-lib/particleflow';
import { ellipsoidCloud, twigAnchors } from 'shared-lib/strands';

// Particle-flow tree: bundled strands grow upward from a basal disc (Holton-style
// trunk = bundle of strand paths), blending into space colonization as strands
// enter the crown and chase attractor points. Recorded paths are stamped into
// the voxel session as tapered cones (this.paths), so the whole existing
// isosurface -> modifier (simplify/smooth/retopo) pipeline is unchanged.
class Tree extends Part {
  static requires = [{ module: 'TreeBranch' }];

  build(p) {
    const AGE   = 1.0;              // growth knob: scales ticks + strand count
    const SEED  = 42;
    const VOX   = 0.07;

    // --- crown attractor cloud ---------------------------------------------
    const CROWN_C = [0, 11, 0];
    const CROWN_R = [5, 4, 5];
    const attractors = ellipsoidCloud(SEED * 7 + 1, 260, CROWN_C, CROWN_R);

    // --- strand sim ----------------------------------------------------------
    const STRANDS = Math.round(120 * AGE);
    const TICKS   = Math.round(800 * AGE);
    const rec = new PathRecorder(VOX * 0.8, ['thickness']);
    const sim = new ParticleSim({
      seed: SEED, dt: 1.0,
      maxTurnRate: 0.06,            // stiff strands: gentle curvature only
      speedTarget: 0.02, speedRelax: 0.15,
      depositEvery: VOX * 0.9,      // deposited spacing ~ voxel size
      maxParticles: STRANDS + 8, maxAge: TICKS,
      hashCell: 0.25,
      attributes: ['thickness'],
      emitters: [{
        shape: 'disc', center: [0, 0, 0],
        axis: [0, 1, 0],            // vel0 is a SCALAR speed; direction is axis
        radius: 0.55,               // basal bundle radius
        rate: STRANDS / 40,         // all strands born in the first ~40 ticks
        vel0: 0.02, jitter: 0.06,   // ADAPTATION: brief had vel0:[0,0.02,0] (array)
        attrInit: [VOX * 1.5],      // strand radius ~ 1.5 voxels
      }],
      fields: [
        // 0: upward bias, fades out through the crown transition band
        { type: 'bias', dir: [0, 1, 0], weight: 0.9,
          fade: { axis: 'y', from: 6, to: 9 } },
        // 1: adhere — strands hug the deposited bundle (the trunk IS the bundle)
        { type: 'adhere', weight: 0.8, radius: 0.35, surfaceOffset: 0.08 },
        // 2: separate — keeps strands from collapsing into one line
        { type: 'separate', weight: 0.5, radius: 0.15 },
        // 3: curl noise — organic wander
        { type: 'curl', weight: 0.25, scale: 2.0, seed: SEED + 3 },
        // 4: attract — space colonization takes over in the crown
        { type: 'attract', weight: 1.2, influence: 2.5, killRadius: 0.35,
          killOnConsume: true,
          fade: { axis: 'y', from: 5, to: 8 } },
      ],
    }).attach(rec);
    sim.setAttractors(attractors);
    sim.run(TICKS);

    // --- stamp strands into the voxel pipeline ------------------------------
    this.fill(MAT.bark);
    this.beginModifier();
    this.beginVoxels(VOX);
    this.paths(rec, { radiusChannel: 'thickness', minRadius: VOX * 0.9 });
    this.endVoxels();
    this.endModifier([
      { simplify: 0.3 },
      { smooth: { iterations: 2 } },
      { retopo: { target_ratio: 1.0, iterations: 3, seed: 42, timeout_seconds: 120 } },
    ]);

    // --- twigs (scaffold; disabled while trunk iterates, same as before) -----
    const PLACE_TWIGS = false;      // Jack disabled branch placement to iterate on the trunk
    if (PLACE_TWIGS) {
      const anchors = twigAnchors(sim, rec, {
        seed: SEED + 9, perPath: 2, k: 2, maxThickness: 0.25,
        normalRadius: 0.3, blend: 0.6,
      });
      const MAX_TWIGS = 10;
      let placed = 0;
      for (const a of anchors) {
        if (placed >= MAX_TWIGS) break;
        this.pushMatrix();
        this.translate(a.pos[0], a.pos[1], a.pos[2]);
        this.lookAt([a.pos[0] + a.normal[0], a.pos[1] + a.normal[1], a.pos[2] + a.normal[2]]);
        this.placeChild('TreeBranch');
        this.popMatrix();
        ++placed;
      }
    }
  }
}
