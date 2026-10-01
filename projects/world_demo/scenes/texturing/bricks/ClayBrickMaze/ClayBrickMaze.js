import { wallWeatherSeed } from 'shared-lib/wall_weathering';
import { clayBrickMazeWalls } from 'shared-lib/clay_brick_maze_layout';
const mortarMaterial=defineMaterial('ClayBrickMaze.mortar',{albedo:[.21,.19,.155],roughness:.98});
const groundMaterial=defineMaterial('ClayBrickMaze.ground',{albedo:[.16,.18,.13],roughness:.95});
class ClayBrickMaze extends World {
  static roots=[...clayBrickMazeWalls().map(w=>({...w,params:{...w.params,mortarMaterial,voxelM:.003,weathering:1,weatherSeed:wallWeatherSeed(w.id),graffiti:['outer-ne','outer-sw','south-court','north-switchback','broad-bend','east-upper','west-route-divider'].includes(w.id)?1:0}})),
    {id:'ground',module:'MazeGround',params:{material:groundMaterial}}];
  static camera={position:[27,24,30],target:[9.3,.5,9.3]};
  static atmosphere={groundAlbedo:.25};
  static lights={sun:{dir:[-.62,-.60,-.51],color:[1,.94,.84]},sky:{color:[.66,.74,.86]}};
}
