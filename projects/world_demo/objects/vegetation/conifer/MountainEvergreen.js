import { mountainTreeParams } from 'shared-lib/mountain_forest';
import { treeRequires, emitTree } from 'shared-lib/conifer';
import { clumpRequires, emitClump } from 'shared-lib/conifer_clump';

class MountainEvergreen extends Part {
  static params = { kind:0, barkMaterial:14, branchMaterial:14,
    needleMaterial:29, coneMaterial:14, redwoodMaterial:14 };
  static noImpostor = true;
  static sharedSurfaces = true;
  static requires(p) {
    const params=mountainTreeParams(p);
    return p.kind===3 ? clumpRequires(params) : treeRequires(params);
  }
  build(p) {
    const params=mountainTreeParams(p);
    if(p.kind===3) emitClump(this,params); else emitTree(this,params);
  }
}
