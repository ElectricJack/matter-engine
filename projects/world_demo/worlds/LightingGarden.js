class LightingGarden extends World {
  static roots = [{
    module: "LightingGarden",
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
  }];

  static lights = {
    sun: {
      dir: [-0.5534701, -0.3522082, -0.7547319],
      color: [0.45, 0.24, 0.12],
    },
    sky: { color: [0.055, 0.075, 0.16] },
  };
}
