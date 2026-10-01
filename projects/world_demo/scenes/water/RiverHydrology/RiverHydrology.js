import {
  authorRiverHydrologyNetwork,
  buildRiverHydrologyDefinition,
  buildRiverHydrologyField,
  riverHydrologyBiomes,
} from 'shared-lib/river_hydrology_definition';

export {
  authorRiverHydrologyNetwork,
  buildRiverHydrologyDefinition,
  buildRiverHydrologyField,
};

class RiverHydrology extends World {
  static world = { sectorSize: 64, yMin: -48, yMax: 144 };
  static camera = { position: [142, 138, 190], target: [150, 29, 0] };
  static volumetrics = { enabled: false };
  static streaming = {
    nestedSectors: true, volumetricSectors: true,
    terrainBands: [
      { radius: 128, lod: 5 }, { radius: 256, lod: 4 },
      { radius: 448, lod: 3 }, { radius: 704, lod: 2 },
    ],
  };
  static roots = buildRiverHydrologyDefinition(0).roots;

  hydrology() {
    authorRiverHydrologyNetwork(this.worldSeed);
  }

  field(p) {
    return buildRiverHydrologyField(p);
  }

  biomes() {
    return riverHydrologyBiomes();
  }
}
