// RockGallery: all 8 Rock seed variants in two rows at the origin for fast
// close-up iteration (no terrain/grass/trees — bakes in seconds, not minutes).
const ROCK_VARIANTS = 8;
const SPACING = 3.0;
// Size sweep: pebble -> house-scale boulder, one bake per size (seed fixed).
const SIZES = [0.12, 0.35, 1.0, 2.5, 6.0];
const SIZE_ROW_Z = 14;

function sizeRowX(i) {
  // Cumulative x so neighbours clear each other (~1.8*size extent each).
  let x = 0;
  for (let j = 1; j <= i; ++j) x += 1.5 * (SIZES[j - 1] + SIZES[j]);
  return x;
}

function makeRequires() {
  const req = [];
  for (let s = 0; s < ROCK_VARIANTS; ++s) req.push({ module: 'Rock', params: { seed: s } });
  for (const sz of SIZES) req.push({ module: 'Rock', params: { seed: 2, size: sz } });
  return req;
}

class RockGallery extends Part {
  static requires = makeRequires();

  build(p) {
    for (let s = 0; s < ROCK_VARIANTS; ++s) {
      const row = Math.floor(s / 4), col = s % 4;
      this.pushMatrix();
      // Same 15% sink Meadow applies (scale 1 here).
      this.translate(col * SPACING, -0.15, row * SPACING + 2);
      this.placeChild('Rock', { seed: s });
      this.popMatrix();
    }
    for (let i = 0; i < SIZES.length; ++i) {
      this.pushMatrix();
      this.translate(sizeRowX(i), -0.15 * SIZES[i], SIZE_ROW_Z);
      this.placeChild('Rock', { seed: 2, size: SIZES[i] });
      this.popMatrix();
    }
  }
}
