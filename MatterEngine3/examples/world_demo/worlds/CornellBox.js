class CornellBox extends World {
  static roots = [{
    module: "CornellBox",
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
  }];

  static lights = {
    sun: {
      dir: [0.1003569, -0.9834976, -0.1505354],
      color: [3.0, 3.0, 3.0],
    },
    sky: { color: [0.6, 0.65, 0.75] },
  };
}
