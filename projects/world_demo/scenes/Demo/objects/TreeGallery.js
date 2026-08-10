// TreeGallery: 8 particle-flow Tree seed variants in a row for side-by-side
// comparison of the growth recipe across seeds.
const TREE_VARIANTS = 8;
const SPACING = 14; // crown radius 6.5 -> 14 keeps neighbouring crowns clear

function makeRequires() {
  const req = [];
  for (let s = 0; s < TREE_VARIANTS; ++s) req.push({ module: 'Tree', params: { seed: s * 101 + 42 } });
  return req;
}

class TreeGallery extends Part {
  static requires = makeRequires();

  build(p) {
    for (let s = 0; s < TREE_VARIANTS; ++s) {
      this.pushMatrix();
      this.translate((s - (TREE_VARIANTS - 1) / 2) * SPACING, 0, 0);
      this.placeChild('Tree', { seed: s * 101 + 42 });
      this.popMatrix();
    }
  }
}
