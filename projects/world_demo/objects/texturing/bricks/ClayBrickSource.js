import { clayBrickSourceSpec } from 'shared-lib/clay_brick_source';

class ClayBrickSource extends Part {
  static lodBudgets = [1];
  static noImpostor = true;
  static params = { seed: 0, length: 0.245, height: 0.084, depth: 0.112,
    material: 8, voxelM: 0.0015, maxVertices: 500000 };
  build(p) {
    this.fill(p.material);
    this.solidSource(clayBrickSourceSpec(p));
  }
}
