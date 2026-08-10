class AtmospherePresentationFixture extends World {
  static params = { worldSeed: 20260809 };
  static camera = { position: [0,2,12], target: [0,1,0] };
  static fog = { density: 0.002, floor: 0, falloff: 30,
                 color: [0.9,0.92,0.95], wind: [0,0,0] };
  static volumetrics = { enabled: true, temporalBlend: 0.0 };
  static lights = {
    sun: { dir: [0,-1,0], color: [1,1,1] },
    sky: { color: [1,1,1] },
  };
  static roots = [
    { module: "AtmospherePresentationReceiver",
      transform: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
  ];
}
