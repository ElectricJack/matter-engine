// LiveTerrainFixture — the native editor fixture for live terrain
// (clear-crest-48.9.10). With MATTER_LIVE_TERRAIN=1 the session hands this
// field program to the terrain coordinator, which feeds the coordinator cache,
// the installed collision and the portable graphics scene from the same tile
// products; MATTER_PORTABLE_SCENE_CAPTURE rasterizes those tiles through
// VulkanRaster. The Crate root does not reach the portable scene in this
// world-kind session (no upload; see live-terrain-editor-qualification.md), so
// every captured draw is a live terrain tile.
//
// The field stays inside the portable CPU generator's op set (noise2, mul,
// add, heightToDensity): a program it cannot sample is refused at install.
class LiveTerrainFixture extends World {
  static world = { sectorSize: 16, yMin: -16, yMax: 16 };

  static camera = {
    position: [0.0, 14.0, 30.0],
    target:   [0.0, -2.0, 0.0],
  };

  static lights = {
    sun: { dir: [-0.35, -0.62, -0.70], color: [3.2, 3.0, 2.7] },
    sky: { color: [0.42, 0.50, 0.66] },
  };

  field() {
    const ground = noise2(704, 0.16, 2).mul(0.12).add(0.35), zero = ground.mul(0);
    return { density: heightToDensity(ground), moisture: zero, relief: zero, seaLevel: -12 };
  }

  static roots = [
    { module: "Crate",
      transform: [1, 0, 0, 0,  0, 1, 0, 2,  0, 0, 1, 0,  0, 0, 0, 1] },
  ];
}
