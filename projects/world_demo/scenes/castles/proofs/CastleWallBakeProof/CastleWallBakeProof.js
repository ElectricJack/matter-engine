import { proofRigidMatrix } from 'shared-lib/castle_fast_bake_proof';
const brick = defineMaterial('CastleWallBakeProof.brick', {
  albedo:[.68,.64,.56], roughness:.78, detail:'CastleBrickBondDetail', detailMode:'surface',
});
const paving = defineMaterial('CastleWallBakeProof.paving', {
  albedo:[.24,.25,.25], roughness:.88,
});
const roots = [
  {id:'wall-front', module:'CastleWallSurface', params:{shape:2,material:brick,revealMaterial:brick},
    transform:proofRigidMatrix({origin:[-2.1,0,0]})},
  {id:'wall-rotated', module:'CastleWallSurface', params:{shape:2,material:brick,revealMaterial:brick},
    transform:proofRigidMatrix({origin:[2.5,0,-.8],yawDeg:30})},
];
for(let x=-4;x<=4;x+=2) for(let z=-2;z<=2;z+=2)
  roots.push({id:`floor-${x}-${z}`,module:'CastleFloorSurface',params:{shape:2,material:paving},
    transform:proofRigidMatrix({origin:[x,-.08,z]})});
class CastleWallBakeProof extends World {
  static camera={position:[6.8,4.4,8.6],target:[0,1.4,-.2]};
  static roots=roots;
  static atmosphere={groundAlbedo:.25};
  static lights={
    sun:{dir:[-.65,-.48,-.3],color:[.95,.88,.75]},sky:{color:[.23,.28,.36]},
    points:[{position:[-3.8,2.5,1.1],color:[1,.81,.59],intensity:45,range:6,sourceRadius:.10,castsShadow:true}],
  };
}
