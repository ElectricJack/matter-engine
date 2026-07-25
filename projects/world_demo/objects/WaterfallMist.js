class WaterfallMist extends Part {
  build(p) {
    this.emitVolume({
      pos:        [0, 0, 0],
      dir:        [0, -0.3, 1],
      radius:     1.2,
      spread:     0.4,
      length:     8,
      density:    0.6,
      color:      [0.95, 0.97, 1.0],
      rise:       0.0,
      turbulence: 0.85
    });
  }
}
