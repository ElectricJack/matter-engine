import { rng } from 'shared-lib/rng';
import { add, sub, scale as vscale, normalize, length } from 'shared-lib/vecmath';

// A ~1-unit SDF boulder. Body: 4-7 ellipsoid blobs (rotate+scale+sphere)
// sharing a dominant horizontal axis, squashed in Y so the mass reads as a
// settled boulder. Facets: 5-9 plane cuts placed via raycast() surface probes
// — each cut shaves a controlled depth below a real surface point, oriented
// by the (jittered) surface normal, so cuts can never gouge the core. One
// baked variant per seed; Meadow instances with random yaw/scale, sunk ~15%.
class Rock extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(1000 + p.seed);
    this.beginModifier();
    this.beginVoxels(0.10);
    this.fill(MAT.rock);
    this.smoothing(0.035);

    // Dominant axis + global anisotropy: settled/bedded, not potato-round.
    const yaw = r.range(0, Math.PI * 2);
    const axis = [Math.cos(yaw), 0, Math.sin(yaw)];
    const stretch = r.range(1.1, 1.5);
    const squashY = r.range(0.72, 0.95);

    const blobs = 4 + r.int(4); // 4-7 ellipsoids
    for (let i = 0; i < blobs; ++i) {
      const along = r.range(-0.35, 0.35);
      const c = add(vscale(axis, along * stretch),
                    [r.range(-0.12, 0.12), 0.42 + r.range(-0.10, 0.14), r.range(-0.12, 0.12)]);
      this.pushMatrix();
      this.translate(c[0], c[1], c[2]);
      this.rotateY(yaw + r.range(-0.4, 0.4));
      this.rotateX(r.range(-0.25, 0.25));
      this.scale(stretch * r.range(0.85, 1.15),
                 squashY * r.range(0.85, 1.15),
                 r.range(0.85, 1.15));
      this.sphere([0, 0, 0], r.range(0.28, 0.5));
      this.popMatrix();
    }

    // Facet cuts: probe the real surface, shave a shallow slice behind it.
    const C = [0, 0.42, 0];            // body centroid estimate (probe target)
    const B = 2.0;                     // cut-box half extent (acts as a plane)
    const cuts = 5 + r.int(5);         // 5-9
    for (let i = 0; i < cuts; ++i) {
      // Direction biased to sides/top (bottoms are sunk into the ground).
      const az = r.range(0, Math.PI * 2);
      const el = r.range(-0.15, 0.95);
      const horiz = Math.sqrt(Math.max(0, 1 - el * el));
      const dir = [Math.cos(az) * horiz, el, Math.sin(az) * horiz];

      const hit = this.raycast(add(C, vscale(dir, 3)), vscale(dir, -1));
      if (!hit) continue;

      // Cut-plane normal: surface normal jittered up to ~25 degrees.
      const m = normalize(add(hit.normal,
        [r.range(-0.45, 0.45), r.range(-0.45, 0.45), r.range(-0.45, 0.45)]));

      // Depth below the surface point, clamped so the plane keeps >=55% of
      // the centroid->surface distance (structurally no L-shape gouges).
      const hitDist = length(sub(hit.point, C));
      const t = Math.min(r.range(0.06, 0.2), 0.45 * hitDist);

      // Large box whose face lies on the plane through (hit.point - m*t):
      // center the box at q + m*B and aim +Z along -m via lookAt.
      const q = sub(hit.point, vscale(m, t));
      const c = add(q, vscale(m, B));
      const up = Math.abs(m[1]) > 0.95 ? [1, 0, 0] : [0, 1, 0];
      this.pushMatrix();
      this.translate(c[0], c[1], c[2]);
      this.lookAt(sub(c, m), up);
      this.rotateZ(r.range(0, Math.PI * 2)); // roll: varies facet intersections
      this.box([0, 0, 0], [B, B, B]);
      this.difference();
      this.popMatrix();
    }

    this.endVoxels();
    this.endModifier([
      { retopo: { target_ratio: 0.35, iterations: 3, seed: 42, timeout_seconds: 60 } },
    ]);
  }
}
