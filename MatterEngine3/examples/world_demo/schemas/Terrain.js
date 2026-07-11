import { heightField } from 'shared-lib/terrain_noise';

// One 16x16-unit heightfield tile of the Meadow. Grid density is set by `res`:
// 'coarse' → 8×8 quads (SPACING 2.0), 'full' → 64×64 quads (SPACING 0.25),
// sampled from the shared terrain noise. Local x/z span [0, TILE]; the
// parent places the tile at (tx*TILE, 0, tz*TILE), so heights sample the noise
// at the tile origin + local offset and adjacent tiles share identical edge
// samples (seamless).
//
// Material by slope and height:
//   slope > 0.75 -> rock   (cliff faces)
//   height > 90  -> snow   (mountain peaks)
//   slope > 0.55 -> dirt   (steep slopes)
//   else         -> grass
//
// worldSeed and worldSize are passed from the parent Meadow so all tiles share
// one consistent heightField instance. res: 'coarse' → N=8 (SPACING=2.0),
// res: 'full' → N=64 (SPACING=0.25). Coarse tiles are placed immediately;
// full tiles bake lazily during the refine loop (Task 6).
class Terrain extends Part {
  static params = { tx: 0, tz: 0, res: 'full', worldSeed: 0, worldSize: 256.0 };

  build(p) {
    const TILE    = 16.0;
    const N       = (p.res === 'coarse') ? 8 : 64;    // quads per side
    const SPACING = TILE / N;                          // 2.0 coarse, 0.25 full
    const ox      = p.tx * TILE, oz = p.tz * TILE;    // world-space tile origin

    const H = heightField(p.worldSeed, p.worldSize);

    // Sample the (N+1)^2 height lattice once.
    const h = [];
    for (let j = 0; j <= N; ++j) {
      const row = [];
      for (let i = 0; i <= N; ++i)
        row.push(H.heightAt(ox + i * SPACING, oz + j * SPACING));
      h.push(row);
    }

    for (let j = 0; j < N; ++j) {
      for (let i = 0; i < N; ++i) {
        const x0 = i * SPACING, x1 = x0 + SPACING;
        const z0 = j * SPACING, z1 = z0 + SPACING;
        const h00 = h[j][i],     h10 = h[j][i + 1];
        const h01 = h[j + 1][i], h11 = h[j + 1][i + 1];
        // Per-quad slope from the already-sampled lattice (no extra noise calls).
        const sx = (h10 - h00 + h11 - h01) * 0.5 / SPACING;
        const sz = (h01 - h00 + h11 - h10) * 0.5 / SPACING;
        const slope  = Math.sqrt(sx * sx + sz * sz);
        const height = (h00 + h10 + h01 + h11) * 0.25;
        let mat;
        if      (slope > 0.75)  mat = MAT.rock;
        else if (height > 90)   mat = MAT.snow;
        else if (slope > 0.55)  mat = MAT.dirt;
        else                    mat = MAT.grass;
        this.fill(mat);
        this.beginShape(SHAPE.triangles);
          this.vertex(x0, h00, z0); this.vertex(x0, h01, z1); this.vertex(x1, h10, z0);
          this.vertex(x1, h10, z0); this.vertex(x0, h01, z1); this.vertex(x1, h11, z1);
        this.endShape();
      }
    }
  }
}
