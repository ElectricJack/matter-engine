// Optional periodic surface finishes using the existing Tileset heightfield
// bake and surface-detail material contract. No source Parts or runtime mesh.
// Tile coordinates and amplitudes are physical metres; all harmonics repeat
// over exactly one metre, so the shared Wang base has continuous boundaries.
const TAU = 2 * Math.PI;
export const CASTLE_FINISH_TILE_SIZE_M = 1;
export const CASTLE_FINISH_TEXELS_PER_M = 512;
export function castleWoodGrainHeight(x, z) {
  const warp = .16 * Math.sin(TAU*x) + .07 * Math.sin(TAU*(3*x+z));
  const broad = .00024 * Math.cos(TAU*(21*z+warp));
  const fine = .00009 * Math.cos(TAU*(63*z+2*warp));
  const pores = .00005 * Math.cos(TAU*(11*x+37*z));
  return broad + fine + pores;
}
export function castleFloorWearHeight(x, z) {
  // Low-amplitude, multi-directional wear; joints remain real slab geometry.
  return .00055*Math.cos(TAU*(2*x+z)+.3)
       + .00042*Math.cos(TAU*(x-3*z)+1.1)
       + .00021*Math.cos(TAU*(7*x+4*z)+2.4)
       + .00012*Math.cos(TAU*(5*x-9*z)+.7);
}
export function emitCastleFinishDetail(part, kind, material) {
  if (kind !== 'wood' && kind !== 'floor') throw new RangeError('unknown castle surface finish');
  part.tile({size:CASTLE_FINISH_TILE_SIZE_M,texelsPerMeter:CASTLE_FINISH_TEXELS_PER_M,seed:27183});
  part.base(kind === 'wood' ? castleWoodGrainHeight : castleFloorWearHeight, material);
}
export function castleFinishMaterialSpec(kind, base) {
  if (kind !== 'wood' && kind !== 'floor') throw new RangeError('unknown castle surface finish');
  return {...base, detail:kind === 'wood' ? 'CastleWoodGrainDetail' : 'CastleFloorWearDetail',detailMode:'surface'};
}
