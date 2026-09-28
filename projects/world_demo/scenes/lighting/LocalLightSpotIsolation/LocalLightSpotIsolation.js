class LocalLightSpotIsolation extends World {
  static camera = { position: [0, 7, 17], target: [0, 1.0, 0] };
  static roots = [{
    module: "LocalLightGalleryFixture",
    params: { mode: 2 },
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
  }];
  static lights = {
    sun: { dir: [-0.45, -0.80, -0.35], color: [0, 0, 0] },
    sky: { color: [0, 0, 0] },
    spots: [
      { position: [-4.0, 7.5, 2.0], direction: [0.25, -1, -0.12],
        color: [1.0, 0.58, 0.12], intensity: 240, range: 12,
        sourceRadius: 0.12, inner: 10, outer: 24 },
      { position: [4.0, 7.5, 2.0], direction: [-0.25, -1, -0.12],
        color: [0.12, 0.42, 1.0], intensity: 240, range: 12,
        sourceRadius: 0.12, inner: 10, outer: 24 },
    ],
  };
}
