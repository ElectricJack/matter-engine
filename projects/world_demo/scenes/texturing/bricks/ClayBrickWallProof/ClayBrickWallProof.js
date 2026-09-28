// Actual whole bricks: left aligned rows; centre/right staggered with headers.
// These source-mesh assemblies review dimensions/ends, not final VT wall cost.
const tones = [[.28,.09,.041],[.25,.077,.036],[.30,.105,.052],[.26,.081,.039],
  [.24,.083,.045],[.285,.085,.04],[.255,.071,.033],[.275,.096,.049]];
const brickMaterials = tones.map((albedo,i) => defineMaterial('ClayBrickWallProof.clay'+i,{albedo,roughness:.88}));
const mortarMaterial = defineMaterial('ClayBrickWallProof.mortar',{albedo:[.21,.19,.155],roughness:.98});
const wall = (id,x,params) => ({ id, module:'ClayBrickWall',
  params:{...params,...Object.fromEntries(brickMaterials.map((m,i)=>['matBrick'+i,m])),mortarMaterial,voxelM:.003},
  transform:[1,0,0,x, 0,1,0,0, 0,0,1,0, 0,0,0,1] });
class ClayBrickWallProof extends World {
  static roots = [
    wall('stack-2x3',-.95,{bond:'stack',columns:2,courses:3}),
    wall('running-4x5',-.28,{columns:4,courses:5}),
    // Requested .5 x .6 m snaps to .5 x .554 m, 2 columns x 6 courses.
    wall('fitted-running',.96,{widthM:.5,heightM:.6,fit:'nearest'}),
  ];
  static camera = {position:[1.75,1.15,2.9],target:[.25,.23,.08]};
  static atmosphere = {groundAlbedo:.25};
  static lights = {sun:{dir:[-.62,-.60,-.51],color:[1,.94,.84]},sky:{color:[.66,.74,.86]}};
}
