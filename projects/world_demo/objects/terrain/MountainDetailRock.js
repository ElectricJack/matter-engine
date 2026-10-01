import { buildMountainDetailRock, emitMountainDetailRock } from 'shared-lib/mountain_detail_rocks';

class MountainDetailRock extends Part {
  static params = {seed:0,shape:2,size:5,resolution:96,material:MAT.rock};
  build(p) { emitMountainDetailRock(this,buildMountainDetailRock(p),p.material); }
}
