import { rng } from 'shared-lib/rng';
import { add, sub, scale as vscale, normalize, length } from 'shared-lib/vecmath';

// An SDF boulder, `size` units tall-ish (default ~1). Body: ellipsoid blobs
// (rotate+scale+sphere) sharing a dominant horizontal axis, squashed in Y so
// the mass reads as a settled boulder. Facets: plane cuts placed via raycast()
// surface probes — each cut shaves a controlled depth below a real surface
// point, oriented by the (jittered) surface normal, so cuts can never gouge
// the core. `size` spans pebbles (~0.1) to house-scale boulders (~6+): voxel
// spacing scales linearly with size (constant bake cost per rock at any size)
// while blob/cut counts grow with size, so big rocks read as more fractured
// rather than magnified. One baked variant per (seed, size); Meadow instances
// with random yaw/scale, sunk ~15%.
// `detail` (default 1 = legacy grid) divides the voxel spacing: detail 2-3
// bakes the same shape on a proportionally finer grid for close-up placements
// (e.g. ForestFloor ground litter) where the size-relative default reads faceted.
class Rock extends Part {
  static params = { seed: 0, size: 1.0, detail: 1.0 };

  build(p) {
    const S = Math.max(0.02, p.size);
    const detail = Math.max(1.0, p.detail || 1.0);
    const r = rng(1000 + p.seed);
    this.beginModifier();
    const spacing = Math.min(0.8, Math.max(0.01, 0.10 * S / detail));
    this.beginVoxels(spacing);
    this.fill(MAT.rock);
    this.smoothing(0.35 * spacing);

    // Dominant axis + global anisotropy: settled/bedded, not potato-round.
    const yaw = r.range(0, Math.PI * 2);
    const axis = [Math.cos(yaw), 0, Math.sin(yaw)];
    const stretch = r.range(1.1, 1.5);
    // Big rocks trend equant/blocky, small ones stay settled-flat.
    const grow = Math.log2(Math.max(1, S));
    const squashY = Math.min(1.05, r.range(0.72, 0.95) + 0.05 * grow);
    const vspread = 1 + 0.6 * grow; // vertical blob scatter: lens -> monolith

    const blobs = 4 + r.int(4) + Math.round(grow);
    for (let i = 0; i < blobs; ++i) {
      const along = r.range(-0.35, 0.35);
      const c = vscale(add(vscale(axis, along * stretch),
                    [r.range(-0.12, 0.12), 0.42 + r.range(-0.10, 0.14) * vspread, r.range(-0.12, 0.12)]), S);
      this.pushMatrix();
      this.translate(c[0], c[1], c[2]);
      this.rotateY(yaw + r.range(-0.4, 0.4));
      this.rotateX(r.range(-0.25, 0.25));
      this.scale(stretch * r.range(0.85, 1.15),
                 squashY * r.range(0.85, 1.15),
                 r.range(0.85, 1.15));
      this.sphere([0, 0, 0], S * r.range(0.28, 0.5));
      this.popMatrix();
    }

    // Facet cuts: probe the real surface, shave a shallow slice behind it.
    const C = [0, 0.42 * S, 0];        // body centroid estimate (probe target)
    const B = 2.0 * S;                 // cut-box half extent (acts as a plane)
    const cuts = Math.min(16, Math.max(3, Math.round((5 + r.int(5)) * Math.max(0.6, Math.sqrt(S)))));
    for (let i = 0; i < cuts; ++i) {
      // Direction biased to sides/top (bottoms are sunk into the ground).
      const az = r.range(0, Math.PI * 2);
      const el = r.range(-0.15, 0.95);
      const horiz = Math.sqrt(Math.max(0, 1 - el * el));
      const dir = [Math.cos(az) * horiz, el, Math.sin(az) * horiz];

      const hit = this.raycast(add(C, vscale(dir, 3 * S)), vscale(dir, -1));
      if (!hit) continue;

      // Cut-plane normal: surface normal jittered up to ~25 degrees.
      const m = normalize(add(hit.normal,
        [r.range(-0.45, 0.45), r.range(-0.45, 0.45), r.range(-0.45, 0.45)]));

      // Depth below the surface point, clamped so the plane keeps >=55% of
      // the centroid->surface distance (structurally no L-shape gouges).
      const hitDist = length(sub(hit.point, C));
      const t = Math.min(S * r.range(0.06, 0.2), 0.45 * hitDist);

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
