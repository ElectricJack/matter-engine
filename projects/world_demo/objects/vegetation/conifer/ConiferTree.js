import { CONIFER_DEFAULTS, treeRequires, emitTree } from 'shared-lib/conifer';
class ConiferTree extends Part {
  // Let coniferParams inherit an omitted branch material from trunk bark.
  static params = Object.fromEntries(Object.entries(CONIFER_DEFAULTS).filter(([key]) => key !== 'branchMaterial'));
  static noImpostor = true;
  static requires(p) { return treeRequires(p); }
  build(p) { emitTree(this, p); }
}
