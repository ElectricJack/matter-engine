import { CLUMP_DEFAULTS, clumpRequires, emitClump } from 'shared-lib/conifer_clump';
class ConiferClump extends Part {
  static params = { ...CLUMP_DEFAULTS };
  static noImpostor = true;
  static requires(p) { return clumpRequires(p); }
  build(p) { emitClump(this, p); }
}
