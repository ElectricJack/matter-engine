// Masonry assemblies expand to individual voxel bricks; architectural details remain named parts.
const STONES = [
  [0.46, 0.39, 0.28],
  [0.4, 0.35, 0.27],
  [0.53, 0.45, 0.33],
  [0.46, 0.42, 0.34],
].map((albedo, i) =>
  defineMaterial("KreuzensteinLimestone" + i, {
    albedo,
    roughness: 0.9,
    metallic: 0,
  }),
);
const BUILDINGS = [
  "Keep",
  "Chapel",
  "Spire",
  "RoundTower",
  "Curtain",
  "Gate",
  "Bridge",
  "Court",
];
class Kreuzenstein extends World {
  static roots = [
    ...BUILDINGS.map((name) => ({
      module: "Kreuzenstein" + name,
      params: { stage: "masonry", stoneBase: STONES[0] },
      expand: true,
      transform:
        name === "RoundTower"
          ? [1, 0, 0, 6, 0, 0.85, 0, -1.25, 0, 0, 1, 0, 0, 0, 0, 1]
          : name === "Chapel"
            ? [1.15, 0, 0, 1.5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
            : [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
    })),
    ...BUILDINGS.filter((name) => !["Bridge", "Court"].includes(name)).map(
      (name) => ({
        module: "Kreuzenstein" + name,
        params: { stage: "details", stoneBase: STONES[0] },
        transform:
          name === "RoundTower"
            ? [1, 0, 0, 6, 0, 0.85, 0, -1.25, 0, 0, 1, 0, 0, 0, 0, 1]
            : name === "Chapel"
              ? [1.15, 0, 0, 1.5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
              : [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
      }),
    ),
    { module: "KreuzensteinGround" },
  ];
  static lights = {
    sun: { dir: [0.45, -0.75, -0.48], color: [1, 0.94, 0.83] },
    sky: { color: [0.24, 0.3, 0.4] },
  };
}
