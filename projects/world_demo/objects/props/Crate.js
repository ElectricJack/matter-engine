class Crate extends Part {
  // Selected content for the portable authoring route (clear-crest-48.9.1):
  // HostBaker bakes this part through the authoring adapter, not the DSL.
  static authoring = "matter.authoring.portable.v1";
  build(p) {
    this.fill(MAT.plaster);
    this.box([0, 0, 0], [1.5, 1.5, 1.5]);
  }
}
