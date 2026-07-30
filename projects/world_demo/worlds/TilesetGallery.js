// TilesetGallery — viewer/QA world for the alpine detail tilesets.
//
// Three near-flat ground bands split by worldX, each bound to one of the new
// detail tilesets through the Wave-1 authoring API (defineMaterial +
// surfaces()), so each atlas can be inspected full-screen at POM range:
//   x < -8   GalleryRock  -> AlpineRockDetail  (strata / outcrop)
//   -8..8    GalleryScree -> ScreeDetail       (settled talus)
//   x > 8    GallerySnow  -> AlpineSnowDetail  (sastrugi snow)
// The ground undulates ~0.5 m at 45 m wavelength — flat enough to read the
// atlases cleanly, bent enough that grazing sun rakes across them. The sun is
// pinned low (~25 deg elevation) to exercise POM self-shadowing / horizon
// maps in every shot.
//
// Geometry buckets keep WorldSector's band-table mechanism (all-dirt via
// __terrain); the surfaces() tape drives appearance only, like ChartVtProof.

const GALLERY_ROCK = defineMaterial("GalleryRock", {
  albedo: [0.45, 0.43, 0.40],
  roughness: 0.95,
  detail: "AlpineRockDetail",
});
const GALLERY_SCREE = defineMaterial("GalleryScree", {
  albedo: [0.44, 0.42, 0.40],
  roughness: 0.97,
  detail: "ScreeDetail",
});
const GALLERY_SNOW = defineMaterial("GallerySnow", {
  albedo: [0.92, 0.94, 0.97],
  roughness: 0.55,
  detail: "AlpineSnowDetail",
});

class TilesetGallery extends World {
  static params = { worldSeed: 20260729 };
  static world = { sectorSize: 64, yMin: -32, yMax: 64 };

  // Spawn looking across all three bands from the scree side.
  static camera = {
    position: [0.0, 22.0, 34.0],
    target: [0.0, 4.0, 0.0],
  };

  // Low sun from the west-ish so relief rakes; slightly warm.
  static lights = {
    sun: { dir: [-0.78, -0.42, -0.30], color: [2.2, 2.0, 1.75] },
    sky: { color: [0.55, 0.65, 0.85] },
  };

  static streaming = {
    rings: [
      { radius: 96.0, rung: 1 },
      { radius: 224.0, rung: 0 },
    ],
  };

  static biomeThresholds = { mountRelief: 2.0, rockyMoisture: 2.0 };

  field(p) {
    // Near-flat: gentle 45 m swells, +-0.5 m, floor at ~4 m.
    const height = noise2(p.worldSeed ^ 0x21, 1 / 45, 2).mul(0.5).add(4);
    const relief = noise2(p.worldSeed ^ 1, 1 / 900, 2);
    const moisture = noise2(p.worldSeed ^ 2, 1 / 700, 2);
    return {
      density: heightToDensity(height),
      moisture,
      relief,
      seaLevel: -20.0,
    };
  }

  // Bands by world X (terrain sectors are world-anchored variants, so world
  // inputs are valid here). 4 m soft crossfades at x = -8 and x = +8.
  surfaces(s) {
    const toSnow = s.worldX.smoothstep(6, 10);        // 1 on the snow side
    const offRock = s.worldX.smoothstep(-10, -6);     // 0 on the rock side
    const rock = offRock.oneMinus();
    const scree = offRock.mul(toSnow.oneMinus());
    s.weight(GALLERY_ROCK, rock);
    s.weight(GALLERY_SCREE, scree);
    s.weight(GALLERY_SNOW, toSnow);
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
