import { CLUMP_DEFAULTS, emitRootCrown } from 'shared-lib/conifer_clump';
class ConiferRootCrown extends Part {
  static params = { ...CLUMP_DEFAULTS, rootDetail: 0 };
  static noImpostor = true;
  static lods = [{ at: 0 }, { at: 12, params: { rootDetail: 1 } }, { at: 40, params: { rootDetail: 2 } }];
  build(p) { emitRootCrown(this, p); }
}
