// An establishing view across the three rooms: close herbs in front, low
// shrubs at centre, then the tree collection as the garden's quiet backdrop.
class VegetationGallery extends World {
  static camera = {
    position: [24.0, 22.0, 50.0],
    target: [0.0, 2.5, -29.0],
  };

  static lights = {
    sun: {
      dir: [-0.46, -0.79, -0.41],
      color: [2.30, 2.14, 1.78],
    },
    sky: { color: [0.34, 0.42, 0.54] },
  };

  static roots = [
    {
      module: 'VegetationGalleryGround',
      transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
    },
    {
      module: 'VegetationGallery',
      transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
      expand: true,
    },
  ];
}
