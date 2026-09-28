// One streamed tile of StreamCaverns. TERRAIN ONLY.
//
// This is the shortest WorldSector in the project and that is the whole design.
// StreamMountain's version is ~290 lines because ~280 of them are scatter: a
// candidate grid, a habitat tape, an O(viable^2) exclusion pass, a placement
// loop -- and at the far bands that scatter is ~96% of a sector bake's self
// time. This world exists to measure the VOXEL MESHER, so none of it is here.
// Nothing below plants anything; `build` calls the native mesher and returns.
//
// Keep it that way. Adding scatter here would not make the world prettier, it
// would make the number it produces mean something else.

// The level-0 tile size. A level-L tile is 64 << L metres across and meshes at
// voxel rung -L, so cells-per-tile is constant -- but unlike the alpine sector,
// nothing here is computed per cell, so the tile size only ever reaches the
// mesher. Kept as the default for `sectorSize` so an older engine that sends no
// sectorSize still meshes a level-0 tile.
const SECTOR = 64.0;

class WorldSector extends Part {
  // Same parameter block every streamed sector takes (matter_engine.cpp builds
  // the JSON); the scatter-only fields are absent because nothing reads them.
  // terrainLod 5 = the 2 m voxel rung, counting DOWN with distance.
  // `ty` is the VERTICAL tile index (volumetric-sectors M3). It is 0 for every
  // request unless the world declares `streaming.volumetricSectors`, in which
  // case a stack of tiles at one (tx, tz) differs in nothing else -- which is
  // why it is in the params at all: it is part of the tile's bake identity, and
  // a cache keyed without it would hand a tile 400 m up the artifact baked for
  // the one at ground level.
  static params = { tx: 0, ty: 0, tz: 0, rung: 0, terrainLod: 5,
                    sectorSize: SECTOR, volumetric: 0,
                    worldSeed: 0, fieldHash: '', biomes: '' };

  // NO ASSETS. StreamMountain installs 16 rock variants plus a vegetation
  // catalog at world load, and every one of those is a procedural mesh bake
  // that blocks world connect. A cave world places nothing, so it waits for
  // nothing -- the first sector bake starts as soon as the world is loaded,
  // which also makes the fill timing easier to read.
  static requires() { return []; }

  build(p) {
    const table = p.biomes ? JSON.parse(p.biomes) : null;
    const oneDirtMaterial =
      table && table.__terrain && table.__terrain.material === 'dirt';
    // One bucket. The geometry band table is not doing any classification here
    // -- appearance comes from the surfaces() tape in StreamCaverns.js, and
    // material_at is a function of (x, z) anyway, so it could not tell a tunnel
    // wall from the plateau above it even if it wanted to.
    const terrainMaterials = oneDirtMaterial
      ? [MAT.dirt, MAT.dirt, MAT.dirt, MAT.dirt]
      : [MAT.grass, MAT.dirt, MAT.rock, MAT.snow];

    // ALL-VOXEL LADDER, same mapping every streamed world uses:
    //   terrainLod 5 -> voxel rung  0 ->  2 m ... terrainLod 0 -> rung -5 -> 64 m
    //
    // CROSS-RUNG SEAMS MATTER MORE HERE than in a single-sheet world: across
    // a rung step the join has to reconcile a whole boundary PLANE of density
    // rather than a surface polyline, or every band boundary rings the world
    // in cracked tunnels. It is not this file's problem -- a sector used to
    // pass an `edgeMask` so the mesher could stitch that border at BAKE time,
    // and the engine now welds the two ACTUALLY DRAWN tiles at runtime
    // (volumetric-sectors M0). The parameter is gone.
    const terrainLod = p.terrainLod === undefined ? 5 : (p.terrainLod | 0);
    const voxelRung = Math.max(-5, Math.min(0, terrainLod - 5));

    // A CUBE: [ty*S, (ty+1)*S) in y. This is what makes yMin cheap -- the
    // column path sampled every row from the surface down to yMin (-1000 m
    // here, ~590 rows at level 0) whether or not anything was down there,
    // while a cube away from the surface or a cavern shell costs one
    // all-air/all-solid pass and yields nothing at all. That is the difference
    // this world was built to show, and it is why it flipped first.
    //
    // COLUMN PATH COMMENTED OUT (2026-08-12):
    //
    //   } else {
    //     this.terrainVolume(p.tx, p.tz, voxelRung, terrainMaterials);
    //   }
    this.terrainVolumeTiled(p.tx, p.ty | 0, p.tz, voxelRung, terrainMaterials);
  }
}
