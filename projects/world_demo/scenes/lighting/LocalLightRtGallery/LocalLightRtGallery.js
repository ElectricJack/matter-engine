import { defineCastleMaterials } from 'shared-lib/castle_materials';

// Focused native-RT acceptance world for local-light visibility and secondary
// hit lighting.  Every source has a finite range and requests traced visibility;
// authored sun/sky are black so a capture cannot accidentally pass on ambient.
const M = defineCastleMaterials('LocalLightRtGallery');
const POM_LOCAL_GROUND = defineMaterial('LocalLightRtPomGround', {
  albedo: [0.34, 0.32, 0.28],
  roughness: 0.95,
  detail: 'ForestFloor',
});
const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

const POINTS = Object.freeze([
  // West room.  The partition stops this warm source except at the doorway.
  Object.freeze({
    position: [-14.5, 2.35, -0.8], color: [1.0, 0.43, 0.12],
    intensity: 95, range: 8.0, sourceRadius: 0.13, castsShadow: true,
  }),
  // Neutral source lights a saturated terracotta card hidden behind the
  // L-corner.  The pale receiver around the return has no direct line of sight.
  Object.freeze({
    position: [-2.5, 2.1, 0.0], color: [1.0, 0.94, 0.82],
    intensity: 120, range: 7.0, sourceRadius: 0.10, castsShadow: true,
  }),
  // Warm key on the gold sphere.
  Object.freeze({
    position: [10.2, 3.35, 3.1], color: [1.0, 0.53, 0.14],
    intensity: 105, range: 7.0, sourceRadius: 0.16, castsShadow: true,
  }),
  // Cool source behind the closed glass volume and its pale receiver.
  Object.freeze({
    position: [16.5, 2.0, -3.2], color: [0.20, 0.55, 1.0],
    intensity: 135, range: 7.5, sourceRadius: 0.11, castsShadow: true,
  }),
  // This source and card sit outside the material camera's view but remain in
  // range of the gold/glass secondary hits (world-space light-index check).
  Object.freeze({
    position: [18.4, 3.0, 3.4], color: [0.30, 1.0, 0.46],
    intensity: 100, range: 8.5, sourceRadius: 0.12, castsShadow: true,
  }),
]);

const SPOTS = Object.freeze([
  Object.freeze({
    position: [8.1, 7.3, -1.0], direction: [0.08, -0.995, 0.06],
    color: [0.22, 0.48, 1.0], intensity: 270, range: 10.0,
    sourceRadius: 0.14, inner: 10, outer: 25, castsShadow: true,
  }),
]);

// The factories are deliberately exported: the editor loads the default below,
// while the fixture test and zero-light contract can exercise the exact same
// authored records without maintaining a second copy of the scene.
export function localLightRtGalleryLights(enabled = true) {
  return {
    sun: { dir: [-0.45, -0.80, -0.35], color: [0, 0, 0] },
    sky: { color: [0, 0, 0] },
    points: enabled ? POINTS.map((light) => ({ ...light })) : [],
    spots: enabled ? SPOTS.map((light) => ({ ...light })) : [],
  };
}

export function localLightRtGalleryRoots(emissiveProxy = true) {
  const roots = [{
    module: 'LocalLightRtGalleryFixture',
    params: {
      plaster: M.plaster,
      limestone: M.limestone[0],
      foundation: M.foundation,
      terracotta: M.terracotta,
      iron: M.iron,
      gold: M.gold,
      clearGlass: M.clearGlass,
      pomGround: POM_LOCAL_GROUND,
    },
    transform: [...IDENTITY],
  }];
  if (emissiveProxy) {
    roots.push({
      module: 'LocalLightRtGlowProxy',
      params: {},
      transform: [...IDENTITY],
    });
  }
  return roots;
}

class LocalLightRtGallery extends World {
  // Overview; the README lists tight, repeatable cameras for each assertion.
  static camera = { position: [23, 11, 27], target: [0, 1.7, 0] };
  static atmosphere = { groundAlbedo: 0.04 };
  static roots = localLightRtGalleryRoots(true);
  static lights = localLightRtGalleryLights(true);
}
