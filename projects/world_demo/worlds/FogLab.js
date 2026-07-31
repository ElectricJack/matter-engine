// FogLab — the smallest world that actually exercises the froxel volumetrics
// path, and the pixel-regression subject for changes to vol_density.comp.
//
// Why it exists: Demo and CornellBox author fog but leave `volumetrics`
// unset, so VkVolumetrics::active() is false and the density shader never
// runs — replaying them proves nothing about a change to it. StreamMountain
// does run it, but it streams 6547 sectors and takes minutes to bake, and
// its residency is explicitly NOT reproducible (see issues/README.md). This
// bakes in seconds and has no streaming at all.
//
// Ground fog ONLY: no cloud layers. That is the point. A change to the
// layered-cloud path must leave this world's pixels untouched, which is the
// check a `MATTER_REPLAY` before/after diff makes cheap.
class FogLab extends World {
  static params = { worldSeed: 20260731 };

  // Camera low and looking slightly up, so the frame is mostly the fog
  // column between the ground plane and the sky — the part of the image the
  // density shader owns.
  static camera = {
    position: [0.0, 24.0, 300.0],
    target:   [0.0, 120.0, 0.0],
  };

  static fog = {
    density: 0.010,
    floor:   0.0,
    falloff: 90.0,
    color:  [0.86, 0.89, 0.95],
    wind:   [0.6, 0.0, 0.2],
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

  // A 40x40 m stone quad scaled to 1200x1200 m, so the fog has ground to sit
  // on out to the far plane, plus three crates near the origin for a depth
  // cue that makes an extinction change visible rather than merely present.
  static roots = [
    {
      module: "PlaygroundFloor",
      transform: [30, 0, 0, 0,  0, 1, 0, 0,  0, 0, 30, 0,  0, 0, 0, 1],
    },
    { module: "Crate",
      transform: [6, 0, 0, 0,  0, 6, 0, 0,  0, 0, 6, 0,  -30, 0, 40, 1] },
    { module: "Crate",
      transform: [6, 0, 0, 0,  0, 6, 0, 0,  0, 0, 6, 0,   20, 0, -60, 1] },
    { module: "Crate",
      transform: [6, 0, 0, 0,  0, 6, 0, 0,  0, 0, 6, 0,   60, 0, -180, 1] },
  ];
}
