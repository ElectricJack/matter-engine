// Small native field terrain for direct procedural rock / soil / moss review.
// All surface scales and heights are metres; all colours are linear RGB.
const TERRAIN = defineMaterial('ProceduralProof.RockSoilMoss', {
  albedo: [0.24, 0.25, 0.22], roughness: 0.88,
});
class ProceduralTerrainProof extends World {
  static world = { sectorSize: 16, yMin: -16, yMax: 16 };
  static streaming = { nestedSectors: true, volumetricSectors: true,
    terrainBands: [{ radius: 24, lod: 5 }], rings: [{ radius: 24, rung: 0 }] };
  static camera = { position: [12, 7, 19], target: [8, 2.5, 8] };
  static atmosphere = { groundAlbedo: 0.25 };
  static lights = {
    sun: { dir: [-0.62, -0.60, -0.51], color: [1.0, 0.94, 0.84] },
    sky: { color: [0.66, 0.74, 0.86] },
  };
  field() {
    const broad = noise2(901, 0.065, 3).mul(3);
    const ground = broad.add(dome(8, 8, 7, 3)).add(0.5);
    const zero = broad.mul(0);
    return { density: heightToDensity(ground), moisture: zero,
             relief: zero, seaLevel: -12 };
  }
  surfaces(s) {
    // Regional geology has a broad drift, tilted warped strata, and fractures.
    // These are continuous world fields, not a periodic source-image lookup.
    const broad = s.noise3World(134, 0.24, 3, 0.5, 2,
      { seed: 315, freq: 0.7, amp: 0.45 });
    const rock = s.ridge3World(919, 1.1, 3, 0.5, 2,
      { seed: 714, freq: 0.8, amp: 0.2 });
    const phase = s.altitude.mul(1.8).add(s.worldX.mul(0.32))
      .add(s.worldZ.mul(0.15)).add(broad.mul(1.6));
    const strata = phase.fract().sub(0.5).abs().mul(2);
    const strataVisible = s.footprint.smoothstep(0.05, 0.22).oneMinus();
    const seam = strata.smoothstep(0.78, 0.98).mul(strataVisible);
    const fine = s.footprint.smoothstep(0.008, 0.03).oneMinus();
    const grain = s.noise3World(281, 36, 2).mul(fine);
    const stoneValue = rock.mul(0.025).add(broad.mul(0.035)).sub(seam.mul(0.035)).add(0.18);
    const stone = [stoneValue, stoneValue.mul(0.92), stoneValue.mul(0.78)];
    const low = s.altitude.smoothstep(0.3, 3.3).oneMinus();
    const soil = broad.add(low.mul(0.40)).add(rock.mul(0.08)).smoothstep(0.04, 0.21);
    const rockHeight = rock.mul(0.038).sub(seam.mul(0.006)).add(grain.mul(0.0012));
    const rockMaterial = { baseColor: stone, roughness: 0.84,
      height: rockHeight, heightRange: [-0.046, 0.040] };
    const soilNoise = s.noise3World(483, 3.5, 3);
    const soilValue = soilNoise.mul(0.025).add(broad.mul(0.016));
    const soilMaterial = { baseColor: [soilValue.add(0.095),
      soilValue.mul(0.8).add(0.068), soilValue.mul(0.5).add(0.038)], roughness: 0.96,
      height: grain.mul(0.0012).add(0.004), heightRange: [0.0028, 0.0052] };
    let surface = s.layer(rockMaterial, soilMaterial, { coverage: soil, width: 0.025 });
    const mossNoise = s.noise3World(745, 5, 3, 0.5, 2, { seed: 714, freq: 8, amp: 0.12 });
    const moss = s.noise3World(431, 0.65, 3).add(low.mul(0.18))
      .add(mossNoise.mul(0.08)).smoothstep(0.10, 0.24).mul(soil);
    const mossMaterial = { baseColor: [mossNoise.mul(0.014).add(0.060),
      mossNoise.mul(0.016).add(0.068), mossNoise.mul(0.009).add(0.027)], roughness: 0.95,
      height: mossNoise.mul(0.001).add(0.003), heightRange: [0.002, 0.004] };
    surface = s.layer(surface, mossMaterial, { operation: 'deposit', coverage: moss, width: 0.012 });
    // A bounded damp pocket spans receivers in world space, then carries the
    // same moss material as the broad field, with a separate placement mask.
    surface = s.splat(surface, { baseColor: surface.baseColor.map(c => c.mul(0.65)), roughness: 0.68 },
      { shape: 'ellipsoid', anchor: 'world', center: [6, 1.5, 11],
        halfSize: [3.5, 3, 2.5], feather: 0.35 },
      { operation: 'appearance', coverage: 0.65 });
    surface = s.splat(surface, mossMaterial,
      { shape: 'ellipsoid', anchor: 'world', center: [6, 1.5, 11],
        halfSize: [3.0, 2.8, 2.2], feather: 0.3 },
      { operation: 'deposit', coverage: mossNoise.smoothstep(-0.08, 0.18), width: 0.012 });
    const tint = grain.mul(0.04).add(1);
    s.source(TERRAIN, { ...surface, baseColor: surface.baseColor.map(c => c.mul(tint)) });
  }
}
