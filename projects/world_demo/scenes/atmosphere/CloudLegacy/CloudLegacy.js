// CloudLegacy — a world authored in the PRE-2026-07-31 cloud syntax, kept
// deliberately in that syntax forever.
//
// `fog.minHeight` / `maxHeight` / `noiseScale` are the single bounded cloud
// layer that predates `fog.clouds`. The loader maps them onto clouds[0], and
// this world exists so that mapping has a subject that can actually be
// measured: StreamMountain is the only other world using the old syntax, and
// a streaming world's replay is not comparable frame to frame (its sector
// residency is explicitly not reproducible — see issues/README.md). This one
// bakes in seconds and replays bit-exact.
//
// Do NOT modernise this file. Its whole value is being the old spelling.
class CloudLegacy extends World {
  static params = { worldSeed: 20260731 };

  // Below the deck, aimed up through it: the layer's underside fills the top
  // of the frame and the ground recedes under it, so both the deck's shape
  // and its effect on the distance are in one image.
  static camera = {
    position: [0.0, 60.0, 600.0],
    target:   [0.0, 150.0, 0.0],
  };

  // Legacy bounded layer. Under the old shader this meant "solid below 120 m,
  // fading to nothing at 220 m"; under the new one it is a deck that exists
  // only between those two heights. That difference is the point of the
  // before/after diff this world is for.
  static fog = {
    density:    0.050,
    minHeight:  120.0,
    maxHeight:  220.0,
    noiseScale: 0.0016,
    color:     [0.88, 0.90, 0.95],
    wind:      [0.5, 0.0, 0.15],
  };

  static volumetrics = {
    enabled: true,
    phaseG: 0.40,
    temporalBlend: 0.85,
  };

  static lights = {
    sun: { dir: [-0.35, -0.62, -0.70], color: [3.2, 3.0, 2.7] },
    sky: { color: [0.42, 0.50, 0.66] },
  };

  static roots = [
    {
      module: "PlaygroundFloor",
      transform: [30, 0, 0, 0,  0, 1, 0, 0,  0, 0, 30, 0,  0, 0, 0, 1],
    },
    { module: "Crate",
      transform: [8, 0, 0, 0,  0, 8, 0, 0,  0, 0, 8, 0,  -40, 12, 540, 1] },
    { module: "Crate",
      transform: [8, 0, 0, 0,  0, 8, 0, 0,  0, 0, 8, 0,   30, 12, 420, 1] },
    { module: "Crate",
      transform: [8, 0, 0, 0,  0, 8, 0, 0,  0, 0, 8, 0,  -20, 12, 180, 1] },
  ];
}
