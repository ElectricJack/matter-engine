import { rng } from 'shared-lib/rng';
import { add, sub, scale as vscale, normalize, length } from 'shared-lib/vecmath';

// An angular talus fragment for scree slopes: a small SDF stone that keeps its
// fracture facets instead of Rock's weathered rounding. Two elongated core
// blobs give a tabular/wedge mass, then 9-13 deep raycast-probed plane cuts
// with almost no smoothing shave it into the sharp-edged, freshly-fractured
// shapes frost-shattered rock actually produces. Native height ~0.4 m at
// size 1; tilesets scale instances 0.1-0.8x for 5-35 cm stones.
//
// Bake-big-place-small: the part mesher samples a fixed ~67 mm voxel grid
// regardless of beginVoxels(spacing) (choose_division_pow never tiers up for
// script parts — base_detail == every particle's detail_size), so a 0.2 m
// native stone lost its cores to sub-voxel thinness and baked EMPTY for most
// seeds (zero-triangle .part -> settle failure). Doubling the NATIVE size
// keeps every ellipsoid half-axis comfortably above one voxel; instances are
// scaled down at placement, which preserves the baked triangles.
//
// Authored sitting on y=0 (mass around y ~ 0.35*S) like Rock/Pebble, so both
// snapped (embed) and physics-settled placements work. Kept cheap on purpose:
// these bake into texture and act as settle colliders in the thousands, so
// the isosurface is decimated hard (simplify 0.12).
class ScreeStone extends Part {
  static params = { seed: 0, size: 1.0 };

  build(p) {
    const S = 0.66 * Math.max(0.1, p.size);
    const r = rng(7100 + p.seed);
    this.beginModifier();
    const spacing = Math.max(0.008, 0.085 * S);
    this.beginVoxels(spacing);
    // Two grey tones across the population so a settled pile reads as mixed
    // rubble, not a single cloned stone.
    this.fill(r.random() < 0.6 ? MAT.stone : MAT.stoneDark);
    // Minimal smoothing: fracture edges must stay crisp.
    this.smoothing(0.25 * spacing);

    // Core: two overlapping ellipsoids sharing a random long axis. Talus
    // fragments trend tabular-to-wedge, so stretch one way, squash Y a bit.
    const yaw = r.range(0, Math.PI * 2);
    const axis = [Math.cos(yaw), 0, Math.sin(yaw)];
    const stretch = r.range(1.15, 1.65);
    // squashY floor raised 0.62 -> 0.72: the mesher's ~67 mm voxels need the
    // ellipsoid's vertical half-axis to stay >~ 1.5 voxels or thin seeds
    // vanish (see the bake-big note in the header).
    const squashY = r.range(0.72, 0.95);
    const C = [0, 0.35 * S, 0];               // centroid estimate (probe target)
    for (let i = 0; i < 2; ++i) {
      // Tight overlap (was 0.05-0.22): a wide gap put the probe centroid C
      // near the waist BETWEEN the cores, where hitDist -> 0 and the cut
      // planes sliced straight through the body.
      const along = (i === 0 ? -1 : 1) * r.range(0.04, 0.12);
      const c = add(C, vscale(axis, along * stretch * S));
      this.pushMatrix();
      this.translate(c[0], c[1] + r.range(-0.05, 0.05) * S, c[2]);
      this.rotateY(yaw + r.range(-0.3, 0.3));
      this.rotateX(r.range(-0.3, 0.3));
      this.scale(stretch, squashY, r.range(0.8, 1.1));
      this.sphere([0, 0, 0], S * r.range(0.34, 0.46));
      this.popMatrix();
    }

    // Fracture facets: probe the surface from all around (including below --
    // these stones tumble, there is no "buried" side) and shave deep slices.
    const B = 2.0 * S;
    const cuts = 9 + r.int(5);
    for (let i = 0; i < cuts; ++i) {
      const az = r.range(0, Math.PI * 2);
      const el = r.range(-0.9, 0.95);
      const horiz = Math.sqrt(Math.max(0, 1 - el * el));
      const dir = [Math.cos(az) * horiz, el, Math.sin(az) * horiz];

      const hit = this.raycast(add(C, vscale(dir, 3 * S)), vscale(dir, -1));
      if (!hit) continue;
      // Skip rays that graze a waist/thin region: cutting where the body is
      // already thin is what produced sub-voxel slivers.
      if (length(sub(hit.point, C)) < 0.4 * S) continue;

      // Jitter the cut normal up to ~30 degrees off the surface normal.
      const m = normalize(add(hit.normal,
        [r.range(-0.55, 0.55), r.range(-0.55, 0.55), r.range(-0.55, 0.55)]));

      // Cut-depth budget: every plane keeps >=65% of the centroid->surface
      // distance along its own ray, so the 9-13 cuts' intersection always
      // contains a ball of ~0.65*0.86*min_half_axis — about 1.5 of the
      // mesher's ~67 mm voxels. The previous 0.45*hitDist budget left a
      // sub-voxel remnant on unlucky seeds and baked EMPTY.
      const hitDist = length(sub(hit.point, C));
      const t = Math.min(S * r.range(0.06, 0.16), 0.3 * hitDist);

      const q = sub(hit.point, vscale(m, t));
      const c = add(q, vscale(m, B));
      const up = Math.abs(m[1]) > 0.95 ? [1, 0, 0] : [0, 1, 0];
      this.pushMatrix();
      this.translate(c[0], c[1], c[2]);
      this.lookAt(sub(c, m), up);
      this.rotateZ(r.range(0, Math.PI * 2));
      this.box([0, 0, 0], [B, B, B]);
      this.difference();
      this.popMatrix();
    }

    this.endVoxels();
    this.endModifier([{ simplify: 0.12 }]);
  }
}
