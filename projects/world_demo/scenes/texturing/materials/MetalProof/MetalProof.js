// MetalProof — renderer test world for per-texel metalness (VT ORM.b).
//
// Purpose: verify that surfaces()-tape metalness renders as a real metal BRDF
// in the RT-lit viewport (albedo-tinted F0, diffuse killed, reflection lobe
// with sky fallback) instead of the historical solid black. Deliberately NOT
// a pretty world: three altitude bands of scalar-albedo material at polished /
// brushed / rough roughness, with a broad noise mask driving s.metallic to
// saturation over roughly half of each band. Every screenshot therefore holds
// metal and dielectric patches of the same material side by side — if metals
// regress to black the failure is unmissable, and if dielectrics drift the
// untouched half of the frame shows it.
//
// Geometry buckets stay all-dirt via __terrain (band-table mechanism, exactly
// like ChartVtProof); the tape drives APPEARANCE only.

const METAL_POLISHED = defineMaterial("metalPolished", {
  // Neutral steel-gray: metal patches should mirror sky/terrain near-white.
  albedo: [0.72, 0.74, 0.78],
  roughness: 0.12,
});
const METAL_BRUSHED = defineMaterial("metalBrushed", {
  // Warm gold: metal F0 must tint the reflection — a gray glint here means
  // albedo never reached F0.
  albedo: [0.85, 0.64, 0.28],
  roughness: 0.45,
});
const METAL_ROUGH = defineMaterial("metalRough", {
  // Dark rough iron: the worst case from the bug report (rough + metal).
  albedo: [0.42, 0.40, 0.38],
  roughness: 0.85,
});

class MetalProof extends World {
  static params = { worldSeed: 20260730 };
  static world = { sectorSize: 64, yMin: -32, yMax: 160 };

  static camera = {
    position: [40.0, 120.0, 150.0],
    target: [0.0, 45.0, 0.0],
  };

  static streaming = {
    // OCTREE STREAMING (volumetric-sectors M3). Every streamed scene runs cube
    // tiles now; the column path is deprecated and its world-facing route is
    // commented out (part_base.js.h, dsl_bindings.cpp).
    //
    // These scenes were on the UNIFORM grid -- no nesting, so no level ladder
    // for an octree to descend -- which is why they needed `nestedSectors` as
    // well. With no `terrainBands` authored, resolve_terrain_defaults() fills a
    // table scaled off sectorSize (5S/8S/12S/18S/27S/40S), so the ladder each
    // one gets is proportional to its own tile size rather than inherited from
    // a world it has nothing to do with.
    nestedSectors: true,
    volumetricSectors: true,
    rings: [
      { radius: 96.0, rung: 1 },
      { radius: 288.0, rung: 0 },
    ],
  };

  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  field(p) {
    // Same compact ridge-plus-foothills terrain as ChartVtProof: one broad
    // ridge through the origin so the spawn view carries all three altitude
    // bands and a clean skyline for mirror checks.
    const ridgeBase = ridge2(p.worldSeed ^ 0x11, 1/260, 4, 0.55, 2.0);
    const ridge = ridgeBase.add(1).mul(0.5).smoothstep(0.55, 0.97);
    const rolling = noise2(p.worldSeed ^ 0x12, 1/90, 3).mul(9);
    const height = ridge.mul(78).add(rolling).add(4);

    const relief = noise2(p.worldSeed ^ 1, 1/900, 2);
    const moisture = noise2(p.worldSeed ^ 2, 1/700, 2);
    return {
      density: heightToDensity(height),
      moisture,
      relief,
      seaLevel: -20.0,
    };
  }

  surfaces(s) {
    // Altitude bands: rough iron on the valley floor, brushed gold on the
    // mid slopes, polished steel on the crests.
    const mid = s.altitude.smoothstep(15, 30);
    const high = s.altitude.smoothstep(45, 65);
    const low = mid.oneMinus();
    s.weight(METAL_ROUGH, low);
    s.weight(METAL_BRUSHED, mid.mul(high.oneMinus()));
    s.weight(METAL_POLISHED, high);

    // Metal mask: ~20 m patches, saturating to exactly 1 over roughly half
    // the surface (the report's failure needs the mask AT 1, not near it).
    const seed = MetalProof.params.worldSeed;
    const patches = s.noise3World(seed ^ 0x3E, 1 / 20, 3, 0.5, 2.0);
    s.metallic(patches.smoothstep(0.45, 0.55));
  }

  biomes() {
    return {
      __terrain: { material: "dirt" },
      foothills: {},
      meadow: {},
      mountains: {},
      ocean: {},
    };
  }
}
