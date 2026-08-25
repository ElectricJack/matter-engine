// Physics-proof raft: a centred 4.8 x 0.7 x 3.0 m flattened, softly rounded
// wood-like body. Runtime collision is authored separately as an exact box.
class RiverRaft extends Part {
  build() {
    this.beginVoxels(0.08);
    this.fill(MAT.bark);
    this.smoothing(0.12);
    this.box([0, 0, 0], [2.4, 0.35, 1.5]);
    this.endVoxels();
  }
}
