import { emitWood } from 'shared-lib/conifer';
class ConiferWood extends Part {
  static params = { species: 0, slot: 0, branchSeed: 17, trunk: false,
    seed: 42, height: 14, dbh: 0.32, lean: 0.025, woodDetail: 0 };
  static noImpostor = true;
  static lods = [
    { at: 0 },
    { at: 6, params: { woodDetail: 1 } },
    { at: 22, params: { woodDetail: 2 } },
    { at: 65, params: { woodDetail: 3 } },
  ];
  build(p) { emitWood(this, p); }
}
