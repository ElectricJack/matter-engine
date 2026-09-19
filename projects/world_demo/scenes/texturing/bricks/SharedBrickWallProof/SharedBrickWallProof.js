// Different receiver dimensions, one anchored brick recipe and source bank.
// Shared material-page counters determine actual reuse; source reuse alone is
// insufficient. Each wall remains one six-face, twelve-triangle receiver.
const mortarMaterial = defineMaterial('SharedBrickWallProof.mortar', {
  albedo: [.21, .19, .155], roughness: .98,
});
const wall = (id, x, columns, courses) => ({
  id, module: 'ClayBrickWallSurface',
  params: { columns, courses, seed: 0, mortarMaterial, voxelM: .003 },
  transform: [1,0,0,x, 0,1,0,0, 0,0,1,0, 0,0,0,1],
});
class SharedBrickWallProof extends World {
  static roots = [
    wall('running-8x12', -2.5, 8, 12),
    wall('running-12x16', 0, 12, 16),
    wall('running-16x20', 3.5, 16, 20),
  ];
  static camera = { position: [7, 3.6, 10.5], target: [2.5, .9, .12] };
  static atmosphere = { groundAlbedo: .25 };
  static lights = {
    sun: { dir: [-.62, -.60, -.51], color: [1, .94, .84] },
    sky: { color: [.66, .74, .86] },
  };
}
