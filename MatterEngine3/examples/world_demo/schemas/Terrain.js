import { heightAt } from 'shared-lib/terrain_noise';

// One 16x16-unit heightfield tile of the Meadow: a 64x64 quad grid (SPACING
// 0.25) sampled from the shared terrain noise. Local x/z span [0, TILE]; the
// parent places the tile at (tx*TILE, 0, tz*TILE), so heights sample the noise
// at the tile origin + local offset and adjacent tiles share identical edge
// samples (seamless). Material by slope: steep quads read as dirt, flat as
// grass. SPACING 0.125 is the terrain lever for the breaking-point run.
class Terrain extends Part {
  static params = { tx: 0, tz: 0 };

  build(p) {
    const TILE = 16.0;
    const SPACING = 0.25;
    const N = Math.round(TILE / SPACING);        // 64 quads per side
    const SLOPE_DIRT = 0.55;                     // |grad h| above this -> dirt
    const ox = p.tx * TILE, oz = p.tz * TILE;    // world-space tile origin

    // Sample the (N+1)^2 height lattice once.
    const h = [];
    for (let j = 0; j <= N; ++j) {
      const row = [];
      for (let i = 0; i <= N; ++i)
        row.push(heightAt(ox + i * SPACING, oz + j * SPACING));
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
        this.fill(Math.sqrt(sx * sx + sz * sz) > SLOPE_DIRT ? MAT.dirt : MAT.grass);
        this.beginShape(SHAPE.triangles);
          this.vertex(x0, h00, z0); this.vertex(x0, h01, z1); this.vertex(x1, h10, z0);
          this.vertex(x1, h10, z0); this.vertex(x0, h01, z1); this.vertex(x1, h11, z1);
        this.endShape();
      }
    }
  }
}
