// CloudLab — four cloud decks with deliberately contrasting parameters, and
// the frame-cost benchmark subject for the density pass.
//
// Two jobs:
//
//   1. Show that decks are independently configurable. The four below differ
//      in every dial that matters — noise scale spans a factor of 25, octaves
//      1 to 4, shoulders from hard to 120 m, coverage from overcast to broken
//      — so a capture makes the difference between them legible rather than
//      merely present.
//
//   2. Sweep the layer count. Each deck's `enabled` carries a
//      MATTER_CLOUD_LAYER<n> env override, and vol_density.comp is specialized
//      on the ENABLED COUNT, so
//
//        MATTER_CLOUD_LAYER0=0 MATTER_CLOUD_LAYER1=0 \
//        MATTER_CLOUD_LAYER2=0 MATTER_CLOUD_LAYER3=0   -> 0 layers
//        MATTER_CLOUD_LAYER1=0 MATTER_CLOUD_LAYER2=0 MATTER_CLOUD_LAYER3=0 -> 1
//        MATTER_CLOUD_LAYER2=0 MATTER_CLOUD_LAYER3=0                       -> 2
//        (nothing set)                                                     -> 4
//
//      measures four different compiled pipelines against one scene. Switch
//      them off from the TOP down: the env layer does not run the panel's
//      compaction, and the count is a PREFIX count, so a hole at layer 0
//      truncates everything behind it.
//
// Volumetrics must be forced on for a headless capture (MATTER_VOLUMETRICS=1)
// AND ray tracing must be left enabled — MATTER_DISABLE_VK_RT makes the whole
// froxel path inert, so a replay taken with it proves nothing about this
// world. Measured: volumetrics on vs off with RT enabled differs in 99.8% of
// pixels; with RT disabled, 0%.
class CloudLab extends World {
  static params = { worldSeed: 20260731 };

  // Under the lowest deck, aimed a few degrees above the horizon, so all four
  // decks stack up the frame in the order they occur.
  static camera = {
    position: [0.0, 60.0, 600.0],
    target:   [0.0, 150.0, 0.0],
  };

  static fog = {
    // Ground fog stays a separate, always-on term underneath the decks: it
    // has no upper bound and the decks do, which is why the two are not
    // unified. Thin, so the decks are what the eye reads.
    density: 0.0025,
    floor:   0.0,
    falloff: 120.0,
    color:  [0.88, 0.90, 0.95],
    wind:   [0.4, 0.0, 0.1],

    clouds: [
      // 0 — low broken cumulus. Small features, hard base, soft top, drifting
      // fastest. The classic "deck you fly under".
      { minHeight: 130.0, maxHeight: 210.0,
        maxDensity: 0.020,
        falloffMin: 6.0, falloffMax: 45.0,
        noiseScale: 0.0020, octaves: 3, lacunarity: 2.03, gain: 0.50,
        coverage: 0.52,
        wind: [2.4, 0.0, 0.6] },

      // 1 — mid stratus sheet. Ten times coarser noise and a single octave,
      // so it reads as a flat lid rather than as banks, and nearly overcast.
      // Overlaps deck 0's top by 20 m on purpose: extinction SUMS there, and
      // that band should be visibly denser than either deck alone.
      { minHeight: 190.0, maxHeight: 250.0,
        maxDensity: 0.014,
        falloffMin: 25.0, falloffMax: 25.0,
        noiseScale: 0.00060, octaves: 1, lacunarity: 2.0, gain: 0.5,
        coverage: 0.78,
        wind: [1.1, 0.0, -0.3] },

      // 2 — high altocumulus, broken. Four octaves at a middling scale: the
      // most expensive deck of the four, and the one whose detail the froxel
      // grid is closest to failing to resolve.
      { minHeight: 420.0, maxHeight: 520.0,
        maxDensity: 0.009,
        falloffMin: 30.0, falloffMax: 30.0,
        noiseScale: 0.0012, octaves: 4, lacunarity: 2.4, gain: 0.62,
        coverage: 0.40,
        wind: [-0.8, 0.0, 0.4] },

      // 3 — cirrus veil. Weather-system-scale noise, very thin, very soft at
      // both bounds, barely moving. Almost the opposite of deck 0 in every
      // dial, which is the point.
      { minHeight: 780.0, maxHeight: 1020.0,
        maxDensity: 0.0035,
        falloffMin: 120.0, falloffMax: 120.0,
        noiseScale: 0.00016, octaves: 2, lacunarity: 2.1, gain: 0.55,
        coverage: 0.62,
        wind: [-0.3, 0.0, 0.1] },
    ],
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
