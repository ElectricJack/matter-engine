class ChimneySmoke extends Part {
  build(p) {
    emitVolume({
      pos:        [0, 0, 0],
      dir:        [0, 1, 0],
      radius:     0.35,
      spread:     0.12,
      length:     14,
      density:    0.9,
      color:      [0.82, 0.82, 0.85],
      rise:       1.8,
      turbulence: 0.55
    });
  }
}
