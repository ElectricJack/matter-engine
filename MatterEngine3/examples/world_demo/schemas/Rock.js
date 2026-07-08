import { rng } from 'shared-lib/rng';

// A ~1-unit SDF boulder: 3-5 overlapping smoothed spheres/boxes jittered by
// seed, then 1-2 difference cuts for facets (difference() retags the
// last-emitted brush). One baked variant per seed; Meadow instances them with
// random yaw/scale and sinks them ~15% into the ground.
class Rock extends Part {
  static params = { seed: 0 };

  // Phase 5 autoremesher opt-in — 2026-07-07 demo.
  //
  // Meadow bake produces 8 unique Rock meshes (ROCK_VARIANTS=8). Empirically,
  // 3 seeds retopo cleanly (2234→64 / 852→62 / 1008→52 tris across the LOD
  // ladder) and at least one triggers a geo_assert abort() inside
  // autoremesher_core's LSCM solver — the same crash class as Tree.js.
  //
  // Blacklist mechanism (retopo_blacklist.{h,cpp}): each retopo attempt
  // journals its cache-key hash on disk; a hash present in the pending journal
  // without a matching success entry crashed the previous run and is skipped.
  // Convergence: 2-3 viewer restarts identify every crasher, after which the
  // meadow bake is stable and the 3+ safe Rock seeds render with retopo'd
  // topology (visible in wireframe via F9 or FIFO `wireframe on`).
  //
  // On a fresh checkout WITHOUT a populated blacklist: the first meadow bake
  // will crash on the first bad seed. Just relaunch — the blacklist grows,
  // the crasher gets skipped, and eventually the bake completes cleanly.
  static retopo = {
    enabled: true,
    target_ratio: 1.0,
    iterations: 3,
    seed: 42,
    timeout_seconds: 60,
  };

  build(p) {
    const r = rng(1000 + p.seed);
    this.beginVoxels(0.15);
    this.fill(MAT.rock);
    this.smoothing(0.12);

    const blobs = 3 + r.int(3);                 // 3-5 union brushes
    for (let i = 0; i < blobs; ++i) {
      const c = [r.range(-0.35, 0.35), r.range(0.2, 0.55), r.range(-0.35, 0.35)];
      if (r.random() < 0.5) {
        this.sphere(c, r.range(0.3, 0.55));
      } else {
        this.box(c, [r.range(0.25, 0.5), r.range(0.2, 0.4), r.range(0.25, 0.5)]);
      }
    }

    const cuts = 1 + r.int(2);                  // 1-2 facet cuts
    for (let i = 0; i < cuts; ++i) {
      const ang = r.range(0, Math.PI * 2);
      const d = r.range(0.75, 1.0);
      this.box([Math.cos(ang) * d, r.range(0.4, 0.9), Math.sin(ang) * d],
               [0.45, 0.45, 0.45]);
      this.difference();                        // retag the cut brush
    }

    this.endVoxels();
  }
}
