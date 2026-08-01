// AlpineRockDetail — exposed, weathered alpine rock, a Wang-tile ground atlas
// bound via defineMaterial({ detail: 'AlpineRockDetail' }).
//
// The read: broken rock under frost-shatter debris. The base heightfield is a
// gentle ISOTROPIC undulation — four near-circular rings of tile-periodic
// wavevectors at many angles, so no direction and no wavelength dominates.
// On top: tabular AlpineSlab fragments (broken bed pieces lying on the
// outcrop, some tan/dark so different beds show), fine ScreeStone rubble
// clustered where frost-shatter debris collects, and flat LichenPatch decals
// for the yellow-green crustose accents alpine rock always carries.
//
// THIS TILESET DELIBERATELY DOES NOT CARRY BEDDING STRATA any more. It did
// until 2026-08-01, as one dominant (2,1) harmonic terraced into 3 cm risers,
// and that is what the "ground looks like carpet" report (issue 676ec01c) was
// looking at. A 2 m tile cannot carry strata: real beds run for tens of
// metres, and anything periodic at 2 m is a comb. Strata belong in the
// surfaces() tape, where StreamMountain already authors them at an 11 m
// altitude period. See the block comment on `rings` below for the full
// argument and the measurements.
//
// Per-tile variation (the 16 Wang tiles) comes from layer() scatters, whose
// interior domains are independently seeded per tile; the base is shared
// content and wraps toroidally — which is exactly why the base must not carry
// anything the eye can lock onto.
//
// Height accounting: base sigma ~0.018 m (range ~ +-0.06), slabs up to ~+0.09
// proud of local base. The runtime POM relief cap (~0.12 m) is never reached
// by the base alone.
class AlpineRockDetail extends Tileset {
  static requires = [
    ...[0, 1, 2, 3].map(s => ({ module: 'AlpineSlab',  params: { seed: s } })),
    ...[0, 1, 2, 3, 4, 5].map(s => ({ module: 'ScreeStone', params: { seed: s } })),
    ...[0, 1, 2, 3].map(s => ({ module: 'LichenPatch', params: { seed: s } })),
  ];

  build() {
    const SIZE = 2.0;
    this.tile({ size: SIZE, texelsPerMeter: 512, seed: 20260729 });

    const TAU = Math.PI * 2;
    let lcg = 0x51ab33c7 >>> 0;
    const rnd = () => {
      lcg = (Math.imul(lcg, 1664525) + 1013904223) >>> 0;
      return lcg / 4294967296;
    };

    // Base undulation. INTEGER wavenumbers only, so the field is exactly
    // SIZE-periodic and the tile has no seams.
    //
    // 2026-08-01 (issue 676ec01c step 2). THE BEDDING STRIKE IS GONE FROM
    // THIS FIELD, and that is the fix rather than a casualty of it.
    //
    // What was here: four harmonics with (2,1) at amplitude 0.040 against a
    // total base sigma of 0.031 — one wavevector carrying 83% of the base
    // variance. |k| = sqrt(5) on a 2 m tile is a 0.89 m comb. The base is
    // SHARED CONTENT (all 16 Wang tiles are byte-identical outside the scatter
    // layers; the dumped 4096^2 atlas shows one unbroken stripe field across
    // the whole thing), so that comb ran in phase across the entire world.
    // Terraced into 3 cm risers it read as woven fabric, and that is the
    // "carpet" in the report. Note where it lives: this tileset's ALBEDO is a
    // flat MAT.stone grey with sparse slab and lichen accents and cannot
    // produce a repeating pattern at all. The artefact is entirely in the
    // height/normal channel, being lit.
    //
    // Two things cannot fix it and were measured not to. Wang tiling shuffles
    // tiles, and every tile has the same base. A macro albedo term (step 1)
    // moved the repetition metric by 0.000. A first attempt at THIS commit
    // kept the strike and merely spread it over six magnitudes; that only
    // moved the metric 0.286 -> 0.271, because six wavevectors all pointing
    // the same way still sum to a one-dimensional field, i.e. still stripes.
    //
    // So: A 2 M TILE CANNOT CARRY BEDDING STRATA. Real beds run for tens of
    // metres; anything periodic at 2 m is a comb by construction, and a comb
    // at this contrast is exactly what a carpet is. Strata belong in the tape,
    // at the scale they actually occur — StreamMountain already authors them
    // there (an 11 m altitude band with a narrow seam mask, surfaces() in
    // StreamMountain.js), which is the right home and does not tile.
    //
    // What this tileset should carry, and now does, is ISOTROPIC weathered
    // rock: four near-circular rings of wavevectors at many angles with
    // pseudo-random phases, so no direction dominates and the power spectrum
    // has no line to lock onto. Same construction and the same reasoning as
    // ForestFloor.js, whose header records making exactly this change for
    // exactly this symptom, and which duly measures ~3x lower repetition than
    // this tileset did. Within each ring every term takes amp/sqrt(n/2), so
    // the ring's variance sums to amp^2.
    //
    // AMPLITUDES ARE ~57% OF WHAT THE STRIKE FIELD CARRIED (measured total
    // sigma 0.0176 against 0.031). That is the other half of the fix and it
    // took three captures to see: removing the directional line does not
    // remove the carpet if the surface is still covered edge to edge in
    // repeating relief at the same contrast. An isotropic field at full
    // amplitude just trades corduroy for tweed — measured, it RAISED the
    // high-frequency contrast of a rock crop from 0.057 to 0.088 while barely
    // moving the repetition peak. This tileset's albedo is flat, so relief is
    // the only thing the eye can lock onto, and every bit of it repeats every
    // 2 m. Less of it is strictly better here.
    //
    // The separate `fine` harmonic set that used to follow this one is gone
    // for the same reason and for one more: it was six pure cosines at (7,0),
    // (0,7), (5,5), (5,-5) and a |k| = 12 pair, i.e. a SQUARE LATTICE at
    // 0.29 m. Once the terrace stopped hiding it, it became the loudest
    // structure left and printed a visible grid of dashes. Rings 3 and 4
    // already cover those wavenumbers with eight angles each, so the set was
    // redundant as well as anisotropic; its variance is folded into them.
    const rings = [
      { amp: 0.012, ks: [[2, 0], [0, 2], [2, 1], [1, 2], [2, -1], [1, -2]] },
      { amp: 0.010, ks: [[4, 0], [0, 4], [3, 2], [2, 3], [3, -2], [2, -3],
                         [4, 1], [1, -4]] },
      { amp: 0.007, ks: [[7, 0], [0, 7], [6, 3], [3, 6], [6, -3], [3, -6],
                         [5, 5], [5, -5]] },
      { amp: 0.004, ks: [[12, 0], [0, 12], [11, 4], [4, 11], [11, -4],
                         [4, -11], [9, 8], [8, -9]] },
    ];
    const dip = [];
    for (const ring of rings) {
      const a = ring.amp / Math.sqrt(ring.ks.length / 2);
      for (const [kx, kz] of ring.ks) dip.push({ kx, kz, a, ph: rnd() * TAU });
    }

    // THE TERRACE IS GONE with the bedding it existed to cut. It quantised g
    // into 3 cm steps with a sixth-power riser, i.e. it is a level-set
    // operator: it converts whatever contours the field has into hard lines
    // and therefore AMPLIFIES the field's dominant structure. Against the old
    // 1-D dip field that made a comb, which is the defect. Against the
    // isotropic rings above it was measured making a dense high-contrast dash
    // pattern — the same amount of hard line, now scattered instead of
    // combed, so the surface read as tweed rather than as stone. Either way
    // it is spending the whole 2 m tile's contrast budget on relief that
    // repeats, and this tileset's albedo is flat, so the relief is ALL the
    // eye has to lock onto.
    //
    // What is left is a gentle isotropic undulation. The tileset's character
    // now comes from the layers below — slabs, scree, lichen — which are the
    // parts that actually DO differ between the 16 Wang tiles, and are
    // therefore the parts that can carry variety.
    //
    // Relief budget: base sigma is now ~0.018 m (was ~0.045 m with the
    // terrace) and slabs still stand up to ~0.09 m proud, so the POM relief
    // cap and the baked horizon channels have MORE headroom, not less, and
    // the litter reads more proud of the floor than it did.
    this.base((x, z) => {
      let h = 0;
      for (const t of dip) h += t.a * Math.cos(TAU * (t.kx * x + t.kz * z) / SIZE + t.ph);
      return h;
    }, MAT.stone);

    // Broken bed fragments lying on the outcrop. Snapped (no settle): flat
    // slabs on gently undulating rock sit naturally with a fraction embedded.
    // Scale ranges are HALF the original: AlpineSlab bakes at a doubled
    // native size so its box core clears the part mesher's ~67 mm voxel
    // (see AlpineSlab.js header); world-space size is unchanged.
    this.layer('AlpineSlab', {
      density: 0.5, scale: [0.35, 0.65], placement: 'poisson',
      physics: false, embed: 0.42,
      params: r => ({ seed: r.int(4) }),
    });
    // Frost-shatter rubble collecting in the hollows.
    // ScreeStone's native height grew 0.2 -> ~0.45 m (same mesher-resolution
    // reason); scales are multiplied by ~0.44 to keep 5-22 cm world rubble.
    this.layer('ScreeStone', {
      density: 2.6, scale: [0.10, 0.22], placement: 'cluster',
      physics: false, embed: 0.4,
      params: r => ({ seed: r.int(6) }),
    });
    // Crustose lichen colonies -- pure albedo accents, near-zero relief.
    this.layer('LichenPatch', {
      density: 1.0, scale: [0.6, 1.3], placement: 'cluster',
      physics: false, embed: 0.0,
      params: r => ({ seed: r.int(4) }),
    });
  }
}
