// A finite catalogue of physical source bricks projected directly into one
// periodic unlit detail atlas. Only the final .gtex is persisted.
export const CASTLE_BRICK_BOND_DETAIL = Object.freeze({
  detailBake: 'brickBondV1',
  sourceModule: 'CastleStoneSource',
  sourceParams: {
    length: 0.30, height: 0.14, depth: 0.20, reliefStyle: 1,
    voxelM: 0.003, material: 8, maxVertices: 500000,
  },
  variants: 8,
  projection: { pixelM: 0.003, paddingM: 0.01, hitEpsilonM: 0.000005, normalEpsilonM: 0.00002 },
  bond: {
    brickWidthM: 0.30, brickHeightM: 0.14,
    pitchUM: 0.3125, pitchVM: 0.15625,
    columns: 4, rows: 8, tilePixels: 512, seed: 0,
    nominalFaceHeightM: 0.10, mortarHeightM: -0.012,
    brickRgb: [[173,164,145],[183,174,154],[163,157,142],[190,179,157],
               [176,168,149],[158,152,136],[186,176,155],[169,162,144]],
    mortarRgb: [112,108,98], brickRoughness: 190, mortarRoughness: 230,
  },
});
