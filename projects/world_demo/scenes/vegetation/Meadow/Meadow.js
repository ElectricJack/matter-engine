class Meadow extends World {
  static roots = [
    {
      module: "Meadow",
      transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
      expand: true,
    },
    {
      module: "ForestFloor",
      transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
      tileset: true,
    },
  ];
}
