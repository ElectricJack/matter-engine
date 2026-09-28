import { rng } from 'shared-lib/rng';

// A single alpine grass tuft for AlpineMeadowDetail: 46-60 tapered strip
// blades (3 tris each, like Grass.js) rising 10-20 cm from a tight crown,
// with a root skirt below y=0 so snapped placement on uneven soil never
// floats. Pure triangle emission — immune to the part mesher's ~67 mm voxel
// grid that bit the voxel-built alpine parts.
//
// COLOR is carried by per-blade MATERIALS, not tint(): the .gtex bake shader
// resolves albedo from the per-triangle material id only (tileset_bake_
// primary.comp mat_albedo) and ignores vertex tints. Each seed picks a
// dominant green family with minority blades from a second family, so a
// scattered population reads as flecks of DIFFERENT greens:
//   0 bright yellow-green    (grass + leaf minority)
//   1 deep green             (leaf + foliageThin minority)
//   2 olive, part-dried      (grass + heavy sand minority)
//   3 dark green             (foliageThin + leaf minority)
//   4 mid green + blossoms   (leaf + grass minority, white accents)
//   5 mostly dried tan       (sand + grass minority)
class MeadowTuft extends Part {
  static params = { seed: 0 };

  build(p) {
    const r = rng(9200 + p.seed);
    const SKIRT = 0.05;
    const BLADES = 46 + r.int(15);

    // [dominant, minority, minorityChance]
    const palettes = [
      [MAT.grass,       MAT.leaf,        0.18],
      [MAT.leaf,        MAT.foliageThin, 0.25],
      [MAT.grass,       MAT.sand,        0.38],
      [MAT.foliageThin, MAT.leaf,        0.30],
      [MAT.leaf,        MAT.grass,       0.28],
      [MAT.sand,        MAT.grass,       0.30],
    ];
    const pal = palettes[p.seed % 6];

    for (let b = 0; b < BLADES; ++b) {
      const ang = r.range(0, Math.PI * 2);
      const d = 0.17 * Math.sqrt(r.random());       // crown radius
      const hgt = r.range(0.10, 0.20);
      const w = r.range(0.010, 0.018);              // half-width at base
      const lean = r.range(0.04, 0.14);             // outward flop
      const yaw = r.range(0, Math.PI * 2);
      const mat = (r.random() < pal[2]) ? pal[1] : pal[0];

      this.fill(mat);
      this.pushMatrix();
      this.translate(Math.cos(ang) * d, 0, Math.sin(ang) * d);
      this.rotateY(yaw);
      // 5-vertex strip: root pair below y=0, tapered mid pair, tip.
      this.beginShape(SHAPE.strip);
        this.vertex(-w, -SKIRT, 0);
        this.vertex( w, -SKIRT, 0);
        this.vertex(-w * 0.6, hgt * 0.55, lean * 0.5);
        this.vertex( w * 0.6, hgt * 0.55, lean * 0.5);
        this.vertex( 0, hgt, lean);
      this.endShape();
      this.popMatrix();
    }

    // Tiny white blossom accents on the mid-green tuft only: a few ragged
    // fan discs held just above the blade mass. MAT.plaster = matte white on
    // the default mesher; fans mirror LichenPatch's winding (+Y face normal).
    if (p.seed % 6 === 4) {
      const blossoms = 2 + r.int(3);
      const SEGS = 7;
      this.fill(MAT.plaster);
      for (let i = 0; i < blossoms; ++i) {
        const ang = r.range(0, Math.PI * 2);
        const d = r.range(0.03, 0.13);
        const cx = Math.cos(ang) * d, cz = Math.sin(ang) * d;
        const cy = r.range(0.11, 0.17);
        const rad = r.range(0.008, 0.014);
        this.beginShape(SHAPE.fan);
        this.vertex(cx, cy, cz);
        const t0 = r.range(0, Math.PI * 2);
        const rim = [];
        for (let s = 0; s < SEGS; ++s) {
          const t = t0 - (s / SEGS) * Math.PI * 2;   // winds for +Y normal
          const rr = rad * r.range(0.75, 1.2);
          rim.push([cx + Math.cos(t) * rr, cy, cz + Math.sin(t) * rr]);
        }
        for (let s = 0; s < SEGS; ++s) this.vertex(rim[s][0], rim[s][1], rim[s][2]);
        this.vertex(rim[0][0], rim[0][1], rim[0][2]);
        this.endShape();
      }
    }
  }
}
