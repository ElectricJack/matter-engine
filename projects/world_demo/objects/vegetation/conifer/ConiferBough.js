import { branchParams, boughRequires, emitBough } from 'shared-lib/conifer';
class ConiferBough extends Part {
  static params = branchParams();
  static noImpostor = true;
  static requires(p) { return boughRequires(p); }
  build(p) { emitBough(this, p); }
}
