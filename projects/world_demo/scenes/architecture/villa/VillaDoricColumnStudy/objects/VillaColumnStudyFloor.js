class VillaColumnStudyFloor extends Part {
  static params={material:8};
  static lodBudgets=[1];
  static noImpostor=true;
  build(p) {
    this.fill(p.material);
    this.beginShape(0);
    for(const [x,z] of [[-30,-30],[-30,30],[30,-30],[30,-30],[-30,30],[30,30]])
      this.surfaceVertex(x,-.004,z,0,1,0,x,z);
    this.endShape();
  }
}
