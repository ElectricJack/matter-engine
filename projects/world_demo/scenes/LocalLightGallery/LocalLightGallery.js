function galleryLocalLights() {
  const points = [];
  const palette = [
    [1.00, 0.24, 0.10],
    [0.12, 0.42, 1.00],
    [1.00, 0.72, 0.18],
    [0.26, 1.00, 0.46],
  ];
  for (let z = 0; z < 17; ++z) {
    for (let x = 0; x < 17; ++x) {
      points.push({
        position: [(x - 8) * 4, 2.2, (z - 8) * 4],
        color: palette[(x + z * 3) & 3],
        intensity: 18,
        range: 3.65,
        sourceRadius: 0.09,
      });
    }
  }
  return points;
}

class LocalLightGallery extends World {
  static camera = {
    position: [0, 58, 69],
    target: [0, 0, 0],
  };
  static roots = [{
    module: "LocalLightGalleryFixture",
    params: { mode: 0 },
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
  }];
  static lights = {
    sun: { dir: [-0.45, -0.80, -0.35], color: [0, 0, 0] },
    sky: { color: [0, 0, 0] },
    points: galleryLocalLights(),
    spots: [
      { position: [-5, 8, 0], direction: [0.45, -1, 0],
        color: [1.0, 0.75, 0.28], intensity: 210, range: 12,
        sourceRadius: 0.12, inner: 12, outer: 27 },
      { position: [5, 8, 0], direction: [-0.45, -1, 0],
        color: [0.24, 0.52, 1.0], intensity: 210, range: 12,
        sourceRadius: 0.12, inner: 12, outer: 27 },
    ],
  };
}
