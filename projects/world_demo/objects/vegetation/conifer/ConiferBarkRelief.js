import { emitBarkRelief } from 'shared-lib/conifer_bark';
class ConiferBarkRelief extends Part {
  static params = { kind: 'trunk' };
  static noImpostor = true;
  static lodBudgets = [1];
  build(p) { emitBarkRelief(this, p.kind, MAT.bark); }
}
