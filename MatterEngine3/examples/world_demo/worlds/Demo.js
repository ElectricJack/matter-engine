class Demo extends World {
  static fog = {
    density:  0.008,
    floor:   -1.0,
    falloff:  35.0,
    color:   [0.88, 0.90, 0.95],
    wind:    [0.3, 0.0, 0.1],
  };

  static roots = [{
    module: "TreeGallery",
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
  }, {
    module: "ChimneySmoke",
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 0, 1],
  }, {
    module: "WaterfallMist",
    transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -8, 0, 10, 1],
  }];
}
