// Dynamic PBR materials shared by the grid-castle kit.
//
// Contract:
//   const materials = defineCastleMaterials();
//   class MyWorld extends World {
//     static roots = [{ module: 'CastleStone',
//       params: { material: materials.limestone[0], ... } }];
//   }
//
// Call defineCastleMaterials() while the world module is evaluated, before
// World.roots is read. Every returned value (and each limestone entry) is an
// integer material handle suitable for a flat scalar Part parameter. Calling
// the function repeatedly with the same prefix is safe because defineMaterial
// folds identical declarations. Geometry must use these handles via fill();
// voxel tint is deliberately not part of this palette contract.
// Returned keys: limestone[4], foundation, mortar, oak, oakEnd, iron, gold,
// agedGold, clearGlass, coloredGlass, slate, terracotta, and plaster.

// These opaque materials intentionally share one meshing group. They coexist
// as end grain, pegs, and straps within a timber CSG expression; separate
// dynamic default groups would make the engine mesh the coincident field once
// per material instead of retaining one surface with local material ownership.
const CASTLE_JOINERY_GROUP = 42001;

export const CASTLE_MATERIAL_SPECS = Object.freeze({
  limestone0: Object.freeze({
    albedo: [0.58, 0.51, 0.39], roughness: 0.86, metallic: 0,
  }),
  limestone1: Object.freeze({
    albedo: [0.66, 0.59, 0.46], roughness: 0.82, metallic: 0,
  }),
  limestone2: Object.freeze({
    albedo: [0.49, 0.44, 0.36], roughness: 0.91, metallic: 0,
  }),
  limestone3: Object.freeze({
    albedo: [0.72, 0.65, 0.51], roughness: 0.78, metallic: 0,
  }),
  foundation: Object.freeze({
    albedo: [0.25, 0.24, 0.22], roughness: 0.94, metallic: 0,
  }),
  mortar: Object.freeze({
    albedo: [0.39, 0.37, 0.32], roughness: 0.98, metallic: 0,
  }),
  oak: Object.freeze({
    albedo: [0.28, 0.135, 0.052], roughness: 0.72, metallic: 0,
    specularStrength: 0.45, mergeGroup: CASTLE_JOINERY_GROUP,
  }),
  oakEnd: Object.freeze({
    albedo: [0.38, 0.19, 0.072], roughness: 0.79, metallic: 0,
    specularStrength: 0.4, mergeGroup: CASTLE_JOINERY_GROUP,
  }),
  iron: Object.freeze({
    albedo: [0.095, 0.085, 0.073], roughness: 0.58, metallic: 1,
    mergeGroup: CASTLE_JOINERY_GROUP,
  }),
  gold: Object.freeze({
    albedo: [0.95, 0.69, 0.24], roughness: 0.16, metallic: 1,
    clearcoat: 0.12, clearcoatRoughness: 0.1,
    mergeGroup: CASTLE_JOINERY_GROUP,
  }),
  agedGold: Object.freeze({
    albedo: [0.66, 0.43, 0.12], roughness: 0.28, metallic: 1,
    mergeGroup: CASTLE_JOINERY_GROUP,
  }),
  clearGlass: Object.freeze({
    albedo: [0.94, 0.98, 1.0], roughness: 0.035, metallic: 0,
    transmission: 0.985, translucency: 0.985, ior: 1.52, opacity: 1,
    absorptionColor: [0.91, 0.97, 0.98], absorptionDistance: 3.5,
    volumeBoundary: true,
  }),
  coloredGlass: Object.freeze({
    albedo: [0.24, 0.48, 0.72], roughness: 0.055, metallic: 0,
    transmission: 0.94, translucency: 0.94, ior: 1.51, opacity: 1,
    absorptionColor: [0.18, 0.52, 0.78], absorptionDistance: 0.65,
    volumeBoundary: true,
  }),
  slate: Object.freeze({
    albedo: [0.13, 0.16, 0.18], roughness: 0.76, metallic: 0,
  }),
  terracotta: Object.freeze({
    albedo: [0.49, 0.17, 0.075], roughness: 0.83, metallic: 0,
  }),
  plaster: Object.freeze({
    albedo: [0.72, 0.68, 0.58], roughness: 0.92, metallic: 0,
  }),
});

export function defineCastleMaterials(prefix = 'Castle') {
  const define = (suffix, key) =>
    defineMaterial(prefix + suffix, CASTLE_MATERIAL_SPECS[key]);
  const limestone = [0, 1, 2, 3].map((index) =>
    define('Limestone' + index, 'limestone' + index));

  return Object.freeze({
    limestone: Object.freeze(limestone),
    foundation: define('FoundationStone', 'foundation'),
    mortar: define('LimeMortar', 'mortar'),
    oak: define('WarmOak', 'oak'),
    oakEnd: define('OakEndGrain', 'oakEnd'),
    iron: define('ForgedIron', 'iron'),
    gold: define('PolishedGold', 'gold'),
    agedGold: define('AgedGold', 'agedGold'),
    clearGlass: define('ClearGlass', 'clearGlass'),
    coloredGlass: define('ColoredGlass', 'coloredGlass'),
    slate: define('RoofSlate', 'slate'),
    terracotta: define('Terracotta', 'terracotta'),
    plaster: define('WarmPlaster', 'plaster'),
  });
}
