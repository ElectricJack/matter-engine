class CastleTimberProbeStand extends Part {
  static params = { material: MAT.stone, support: MAT.stone, marks: MAT.metal };
  static lodBudgets = [1];
  static noImpostor = true;
  build(p) {
    this.fill(p.material);
    this.box([0, -0.025, 0], [2.0, 0.025, 1.2]);
    this.box([0, 0.9, -0.95], [2.0, 0.9, 0.035]);
    this.fill(p.support);
    for (const x of [-0.62, 0.62]) {
      this.box([x, 0.35, 0.45], [0.05, 0.35, 0.14]);
      this.box([x, 0.35, -0.30], [0.05, 0.35, 0.055]);
    }
    // A one-metre rule on the floor: centimetre ticks, longer every 10 cm.
    this.fill(p.marks);
    for (let i = 0; i <= 100; ++i) {
      const depth = i % 10 === 0 ? 0.032 : 0.014;
      this.box([-0.5 + i * 0.01, 0.002, 0.87], [0.001, 0.002, depth]);
    }
  }
}
