import { rng } from 'shared-lib/rng';

// A tabular rock-slab fragment: a piece of a sedimentary bed that broke
// off along its bedding plane and now lies flat on the strata below. Built as
// a squat box core whose perimeter is shaved by 6-8 near-vertical plane cuts
// (irregular polygon outline, like flagstone) plus a shallow top bevel or two.
// Some seeds stack a smaller offset slab on top so the fragment itself shows
// a bedding step. Native footprint ~1.0 x 0.7 m, ~11-18 cm thick, sitting on
// y=0 for snapped (embed) placement.
//
// Bake-big-place-small: the part mesher samples a fixed ~67 mm voxel grid
// regardless of beginVoxels(spacing) (choose_division_pow never tiers up for
// script parts), so the original ~6-9 cm slab was thinner than one voxel and
// baked EMPTY (zero-triangle .part -> "collider_for_part ... zero triangles"
// settle failure, which blocked every alpine detail tileset). Native dims are
// doubled so the box half-thickness clears the half-voxel threshold; tileset
// layers place instances at half the old scale for the same world-space read.
//
// Seed palette: 0,2 grey stone; 1 tan sand (a weathered lighter bed); 3 dark
// stone -- so a scatter of slabs reads as fragments of *different* beds.
class AlpineSlab extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(8100 + p.seed);
    const L = 1.0 * r.range(0.9, 1.15);    // half-ish length scale base
    const W = 0.72 * r.range(0.85, 1.15);
    const TH = r.range(0.11, 0.18);        // full thickness
    // NOTE: not MAT.sand for the light bed — sand selects the oriented-cube
    // mesher (material_registry meshingAlgorithm=1), which ignores box/fat
    // brushes entirely, so a sand slab bakes EMPTY. Plaster is the light warm
    // tone on the default marching-cubes mesher.
    const mats = [MAT.stone, MAT.plaster, MAT.stone, MAT.stoneDark];

    this.beginModifier();
    const spacing = 0.016;
    this.beginVoxels(spacing);
    this.fill(mats[p.seed & 3]);
    this.smoothing(0.3 * spacing);

    const yaw0 = r.range(0, Math.PI * 2);

    // Core slab: box half-extents, resting on y=0.
    this.pushMatrix();
    this.rotateY(yaw0);
    this.box([0, TH * 0.5, 0], [L * 0.5, TH * 0.5, W * 0.5]);
    this.popMatrix();

    // Optional smaller slab stacked on top with a lateral offset: an in-part
    // bedding step (~45% of seeds).
    const stacked = r.random() < 0.45;
    let topY = TH;
    if (stacked) {
      // th2 floor 0.8*TH (was 0.6): the stacked box is its own thin slab and
      // must also clear the mesher's half-voxel (~33 mm) threshold.
      const th2 = TH * r.range(0.8, 1.0);
      this.pushMatrix();
      this.rotateY(yaw0 + r.range(-0.25, 0.25));
      this.box([r.range(-0.12, 0.12) * L, TH + th2 * 0.5, r.range(-0.12, 0.12) * W],
               [L * 0.5 * r.range(0.55, 0.75), th2 * 0.5, W * 0.5 * r.range(0.55, 0.8)]);
      this.popMatrix();
      topY = TH + th2;
    }

    // Perimeter cuts: big boxes whose inner face passes at dist from center,
    // facing inward at a near-vertical angle -> irregular polygonal outline.
    const B = 1.6;
    const cuts = 6 + r.int(3);
    for (let i = 0; i < cuts; ++i) {
      const az = (i + r.range(-0.35, 0.35)) * (Math.PI * 2 / cuts);
      const dx = Math.cos(az), dz = Math.sin(az);
      // Distance of the cut plane from center, in the footprint's metric.
      const dist = r.range(0.30, 0.48) * (Math.abs(dx) * L + Math.abs(dz) * W);
      this.pushMatrix();
      this.translate(dx * (dist + B), topY * 0.5, dz * (dist + B));
      this.rotateY(-az + Math.PI * 0.5);       // box +X faces the center
      this.rotateZ(r.range(-0.12, 0.12));      // slight off-vertical fracture
      this.box([0, 0, 0], [B, B, B]);
      this.difference();
      this.popMatrix();
    }

    // One or two shallow top bevels: tilted boxes shaving the upper arris.
    // The cut plane is ANCHORED: cy = topY - d + B/cos(tilt) places the box's
    // bottom face exactly d below the top surface over the anchor point
    // (dx*dist, dz*dist), for any B and tilt. rotateZ(-tilt) leans the cut
    // normal toward the outward radial, so the plane dives below the top only
    // outboard of the anchor -- a wedge off the rim, rising clear of the slab
    // toward the center. (The previous topY + B*0.92 placement put the plane
    // ~0.09-0.13 m deep with a footprint covering the whole slab, which
    // erased the entire part for every seed -> zero-triangle bakes.)
    const bevels = 1 + r.int(2);
    for (let i = 0; i < bevels; ++i) {
      const az = r.range(0, Math.PI * 2);
      const dx = Math.cos(az), dz = Math.sin(az);
      const dist = r.range(0.25, 0.42) * (Math.abs(dx) * L + Math.abs(dz) * W);
      const tilt = r.range(0.14, 0.28);        // outward-down lean of the cut
      const d = r.range(0.025, 0.05);          // graze depth at the anchor
      this.pushMatrix();
      this.translate(dx * dist, topY - d + B / Math.cos(tilt), dz * dist);
      this.rotateY(-az);
      this.rotateZ(-tilt);                     // +Y normal leans outward
      this.box([0, 0, 0], [B, B, B]);
      this.difference();
      this.popMatrix();
    }

    this.endVoxels();
    this.endModifier([{ simplify: 0.10 }]);
  }
}
