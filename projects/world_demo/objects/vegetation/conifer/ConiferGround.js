class ConiferGround extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { size: 50 };
  build(p) {
    this.fill(MAT.bark); this.tint(0.20, 0.185, 0.14, 1);
    this.box([0, -0.15, 0], [Math.max(20, p.size), 0.20, Math.max(20, p.size)]);
  }
}
