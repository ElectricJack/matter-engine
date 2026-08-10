class AtmospherePresentationReceiver extends Part {
  build(p) {
    this.fill(MAT.plaster);
    this.box([0,-0.05,0], [8,0.1,8]);
    this.box([0,1,0], [0.6,2,0.6]);
  }
}
