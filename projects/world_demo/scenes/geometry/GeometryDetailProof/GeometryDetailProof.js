// Unique procedural surface plus disconnected solid debris. Geometry, normals
// and UVs are authored once; the engine pages representations of that source.
const DETAIL_STONE = defineMaterial('GeometryDetail.Stone', {
  albedo: [0.34, 0.29, 0.22], roughness: 0.86,
});
class GeometryDetailProof extends World {
  static roots = [{ module: 'DetailedGround', params: { resolution: 64, material: DETAIL_STONE } }];
  static camera = { position: [9, 6, 10], target: [0, 0.2, 0] };
  static lights = {
    sun: { dir: [-0.55, -0.7, -0.4], color: [1, 0.95, 0.85] },
    sky: { color: [0.58, 0.68, 0.82] },
  };
}
