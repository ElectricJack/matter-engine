class LocalLightPointIsolation extends World {
  static camera = { position: [0, 6, 16], target: [0, 1.2, 0] };
  static roots = [{
    module: "LocalLightGalleryFixture",
    params: { mode: 1 },
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
  }];
  static lights = {
    sun: { dir: [-0.45, -0.80, -0.35], color: [0, 0, 0] },
    sky: { color: [0, 0, 0] },
    points: [
      { position: [-3.5, 3.2, 1.5], color: [1.0, 0.14, 0.05],
        intensity: 85, range: 7, sourceRadius: 0.10 },
      { position: [3.5, 3.2, 1.5], color: [0.05, 0.25, 1.0],
        intensity: 85, range: 7, sourceRadius: 0.10 },
    ],
  };
}
