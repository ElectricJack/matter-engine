// RockGallery: all 8 Rock seed variants in two rows at the origin for fast
// close-up iteration (no terrain/grass/trees — bakes in seconds, not minutes).
const ROCK_VARIANTS = 8;
const SPACING = 3.0;

function makeRequires() {
  const req = [];
  for (let s = 0; s < ROCK_VARIANTS; ++s) req.push({ module: 'Rock', params: { seed: s } });
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
  }
}
