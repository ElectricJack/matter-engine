// One material module, finite walls measuring 1x, 2x and 4x its brick counts.
// All three remain twelve-triangle boxes with whole-brick end treatments.
const mortarMaterial = defineMaterial('PeriodicBrickWallProof.mortar', {
  albedo:[.21,.19,.155],roughness:.98,
});
const wall = (id,x,multiple) => ({
  id,module:'ClayBrickWallSurface',
  params:{columns:8*multiple,courses:4*multiple,moduleColumns:8,moduleCourses:4,
    seed:0,mortarMaterial,voxelM:.003},
  transform:[1,0,0,x,0,1,0,0,0,0,1,0,0,0,0,1],
});
class PeriodicBrickWallProof extends World {
  static roots=[wall('module-1x',-3,1),wall('module-2x',0,2),wall('module-4x',5,4)];
  static camera={position:[11,6,17],target:[5.1,.75,.1225]};
  static atmosphere={groundAlbedo:.25};
  static lights={sun:{dir:[-.62,-.60,-.51],color:[1,.94,.84]},sky:{color:[.66,.74,.86]}};
}
