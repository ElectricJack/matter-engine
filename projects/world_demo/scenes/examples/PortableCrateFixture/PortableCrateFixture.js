// PortableCrateFixture — the matched-view fixture for the portable graphics
// adapter (clear-crest-48.9.1). Crate opts into matter.authoring.portable.v1,
// so the legacy renderer draws the .part baked from the retained AssetProduct
// while MATTER_PORTABLE_SCENE_CAPTURE rasterizes the same instances through
// MatterGraphics; the floor stays legacy content.
//
// Placements are row-major with the translation in the last column (see
// WorldRoot::transform) and cover a plain, a yawed, a mirrored and a scaled
// crate, so the capture checks placement, winding and per-instance identity.
class PortableCrateFixture extends World {
  static camera = {
    position: [0.0, 7.0, 26.0],
    target:   [0.0, 1.5, 0.0],
  };

  static lights = {
    sun: { dir: [-0.35, -0.62, -0.70], color: [3.2, 3.0, 2.7] },
    sky: { color: [0.42, 0.50, 0.66] },
  };

  static roots = [
    { module: "PlaygroundFloor",
      transform: [1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1] },
    { module: "Crate",
      transform: [1, 0, 0, -8,  0, 1, 0, 1.5,  0, 0, 1, 0,  0, 0, 0, 1] },
    { module: "Crate",
      transform: [0.819, 0, 0.574, 0,  0, 1, 0, 1.5,  -0.574, 0, 0.819, -4,  0, 0, 0, 1] },
    { module: "Crate",
      transform: [-1, 0, 0, 8,  0, 1, 0, 1.5,  0, 0, 1, 2,  0, 0, 0, 1] },
    { module: "Crate",
      transform: [2, 0, 0, 4,  0, 2, 0, 3,  0, 0, 2, -14,  0, 0, 0, 1] },
  ];
}
