import { candidatesInRect } from 'shared-lib/scatter_grid';
import { mountainRockCatalog } from 'shared-lib/mountain_rocks';
import { mountainGeometrySamples, mountainGeometryCatalog } from 'shared-lib/mountain_geometry_site';
import { planMountainRocks, mountainRockClearance, MOUNTAIN_ROCK_HALO } from 'shared-lib/mountain_rock_scatter';
import { mountainForestCatalog, planMountainForest,
  MOUNTAIN_FOREST_MIN_LOD } from 'shared-lib/mountain_forest';

// One streamed column of the infinite world. Terrain comes from the native
// world field (terrainVolume); scatter reads the biomes table passed down
// from the world definition. Geometry is sector-local in x/z, world y.
//
// p.rung is a SCATTER DETAIL TIER (see matter_engine.cpp rings), NOT a mesh
// resolution -- that is p.terrainLod, which now selects a VOXEL rung rather
// than choosing between two different terrain representations. Landmark
// boulders place at every rung; rocks gate on `p.rung >= 1`; vegetation
// uses the mixed evergreen planner and the world habitat tape.
//
// Rocks use stable world-space cluster anchors and candidate grids. Geometry
// comes from twelve analytic silhouettes in four reference sizes; terrain fit
// and pose belong to instances. The same boulder footprints keep trunks clear.

// The SCATTER CELL, and the level-0 tile size. Under nested sector LOD a tile
// may be 2^level of these across (p.sectorSize), but scatter is always
// computed per 64 m cell so a placement does not move when its carrier tile
// changes size -- see the cell loop in build().
const SECTOR = 64.0;

// ---- ScriptProfile slots (MatterEngine3/src/dsl_bindings.h) ----------------
//
// The sector-level split around terrain, planning and placement. `sector.terrain`
// is the native mesher call and is here as the SCALE: every other label is
// only meaningful against something known to be real work. Inert unless
// MATTER_SCRIPT_PROFILE is set.
//
// typeof-guarded for the same reason alpine_ecology.js guards: this file is
// evaluated by more than one kind of JS context and their preludes differ.
const pslot  = typeof profSlot  === 'function' ? profSlot  : () => -1;
const pbegin = typeof profBegin === 'function' ? profBegin : () => {};
const pend   = typeof profEnd   === 'function' ? profEnd   : () => {};

const P_TERRAIN  = pslot('sector.terrain');
const P_BOULDERS = pslot('sector.boulders');
const P_PLAN     = pslot('sector.plan');
const P_PLACE    = pslot('sector.place');
const P_ROCKS    = pslot('sector.rocks');
const VEGETATION_MIN_LOD = MOUNTAIN_FOREST_MIN_LOD;

// `biomesJson` is this world's biomes() table (the same string build() gets
// as p.biomes) -- world-level, so every sector of a given world sees the same
// string and therefore the same variant list, which is what the child-hash
// stability requires.
function assetVariants(biomesJson) {
  let table = null;
  try { table = biomesJson ? JSON.parse(biomesJson) : null; } catch (e) {}
  if (table?.__terrainOnly) return table.__geometryRocks
    ? mountainGeometryCatalog(table.__geometryRocks.material) : [];
  const req = mountainRockCatalog();
  req.push(...mountainForestCatalog(table?.__vegetation?.materials));
  if(table?.__geometryRocks)req.push(...mountainGeometryCatalog(table.__geometryRocks.material));
  return req;
}

class WorldSector extends Part {
  // terrainLod: 5 = native voxel mesh (rung 0), 0-4 coarsen 2x per step (see
  // PHASE 1 below). sectorSize is the TILE's own width -- a level-L tile is
  // 64 << L metres and meshes at voxel rung -L, so cells-per-tile stays
  // constant. Defaults (5, SECTOR) keep an older engine, which sends neither,
  // on the voxel path at a level-0 tile.
  // `ty` is the VERTICAL tile index and `volumetric` says whether it means
  // anything (volumetric-sectors M3). Both are 0 for every request unless the
  // world declares `streaming.volumetricSectors`. They are in the params, not
  // inferred, because they are part of the tile's BAKE IDENTITY: a stack of
  // tiles at one (tx, tz) differs in nothing else, and the same (tx, ty, tz,
  // rung) means different geometry in the two modes.
  static params = { tx: 0, ty: 0, tz: 0, rung: 0, terrainLod: 5, volumetric: 0,
                    sectorSize: SECTOR,
                    worldSeed: 0, fieldHash: '', biomes: '' };
  // FIXED variant list — independent of tx/tz so the whole asset set installs
  // once at world load and every sector bake hits the same child hashes.
  static requires(p) { return assetVariants(p && p.biomes); }

  build(p) {
    const table = p.biomes ? JSON.parse(p.biomes) : null;
    const oneDirtMaterial =
      table && table.__terrain && table.__terrain.material === 'dirt';
    const terrainMaterials = oneDirtMaterial
      ? [MAT.dirt, MAT.dirt, MAT.dirt, MAT.dirt]
      : [MAT.grass, MAT.dirt, MAT.rock, MAT.snow];

    // ==== PHASE 1: TERRAIN ===================================================
    // ALL-VOXEL TERRAIN LADDER: every rung is the same voxel mesher,
    // coarsening 2x per step (no separate heightfield representation below
    // terrainLod 5 anymore -- that old seam could not express overhangs,
    // arches, or caves and no band tuning could line the two sides up):
    //   terrainLod 5 -> voxel rung  0 ->  2 m   (unchanged near appearance)
    //   terrainLod 4 -> voxel rung -1 ->  4 m
    //   terrainLod 3 -> voxel rung -2 ->  8 m
    //   terrainLod 2 -> voxel rung -3 -> 16 m
    //   terrainLod 1 -> voxel rung -4 -> 32 m
    //   terrainLod 0 -> voxel rung -5 -> 64 m   (one cell per sector)
    //
    // CROSS-RUNG SEAMS ARE NOT THIS FILE'S PROBLEM any more. The [1..n]
    // ownership rule only makes EQUAL-rung neighbours watertight; across rungs
    // the coarse side interpolates between every other sample while the fine
    // side follows the field, and the two part company. A sector used to pass
    // an `edgeMask` naming the neighbours it believed were one rung coarser so
    // the mesher could stitch that border at BAKE time. The engine now welds
    // the two ACTUALLY DRAWN tiles at runtime (volumetric-sectors M0), so the
    // parameter is gone and a tile's geometry depends on nothing outside it.
    const terrainLod = p.terrainLod === undefined ? 5 : (p.terrainLod | 0);
    // Stress profile: 8x source sampling density; finest spacing is 0.25 m.
    const voxelRung = Math.max(-5, Math.min(3, terrainLod - 2));
    pbegin(P_TERRAIN);
    // COLUMN OR CUBE, never both: the two verbs describe the same matter at
    // different extents, so calling both would mesh the whole column and then
    // a slice of it again.
    // COLUMN PATH COMMENTED OUT (2026-08-12). Every streamed scene declares
    // `volumetricSectors`, so `p.volumetric` is always 1 and the else-branch
    // below was dead; `terrainVolume` itself is commented out in part_base.js
    // and its native binding is gone, so calling it would now throw.
    //
    //   } else {
    //     this.terrainVolume(p.tx, p.tz, voxelRung, terrainMaterials);
    //   }
    //
    // The engine still SENDS `volumetric`, and this file still ignores it
    // rather than asserting on it: MATTER_VOLUMETRIC_SECTORS=0 remains the
    // A/B, and under it the streamer hands out column keys while this bakes a
    // cube at ty 0 -- wrong, but wrong in the one configuration a person has
    // to opt into by hand. Restoring the branch is how you take that A/B back.
    this.terrainVolumeTiled(p.tx, p.ty | 0, p.tz, voxelRung, terrainMaterials);
    pend(P_TERRAIN);
    if (!table) return;

    // ==== PHASES 2-4: BOULDERS / ROCKS / FOREST ===================
    // SCATTER RUNS PER FIXED 64 m CELL, NOT PER TILE. Terrain meshes as one
    // whole tile above; a level-L tile covers 4^L of the 64 m cells the world
    // has always scattered in, and the loop at the end of build() walks them
    // one at a time with a level-0 bake's own origin/biome/RNG/candidates/caps.
    // That is what keeps placements STABLE ACROSS LEVEL TRANSITIONS -- `r` is
    // seeded from CELL coordinates (not tile), so a tile that changed size
    // never reshuffles a rock or tree as its carrier tile changes size.
    // The candidate spacing is in metres and remains constant across bands.
    const TILE   = p.sectorSize > 0 ? p.sectorSize : SECTOR;
    const cells  = Math.max(1, Math.round(TILE / SECTOR));
    const tileOx = p.tx * TILE, tileOz = p.tz * TILE;
    // The tile's Y ORIGIN. Zero on the column path -- a column has no y origin
    // to be local to, which is why terrainVolume returns world-absolute y and
    // `place` below has always used heightAt directly. A CUBE tile has one, and
    // the engine's publish transform supplies transform[7] = ty * S to put it
    // back, so every y this file emits must be relative to it or the whole
    // scatter layer floats `ty * S` metres above the ground it belongs to.
    const volumetric = (p.volumetric | 0) !== 0;
    const tileOy = volumetric ? (p.ty | 0) * TILE : 0;
    const baseCx = p.tx * cells, baseCz = p.tz * cells;
    const seed = p.worldSeed >>> 0;
    // A unique half-open XYZ owner at every sector level. These assets use the
    // same streamer as the surrounding mountain, including eviction/re-entry.
    if(table.__geometryRocks)for(const sample of mountainGeometrySamples(table.__geometryRocks.material)) {
      if(sample.x<tileOx||sample.x>=tileOx+TILE||sample.z<tileOz||sample.z>=tileOz+TILE)continue;
      const y=this.heightAt(sample.x,sample.z);
      if(volumetric&&(y<tileOy||y>=tileOy+TILE))continue;
      this.pushMatrix();this.translate(sample.x-tileOx,y-Math.min(.35,sample.params.size*.12)-tileOy,sample.z-tileOz);
      this.placeChild('MountainDetailRock',sample.params,{instanced:true,inlineBelowPx:0});
      this.popMatrix();
    }

    if (table.__terrainOnly) return; // detailed rock stress site, without forest

    // One 64 m cell. A function rather than an inlined loop body so the
    // VEGETATION_MIN_LOD gate below stays a `return` -- it reads as "this
    // cell is done". At cells === 1 the cell coordinates ARE the tile
    // coordinates, so every expression below is the one a level-0 bake
    // always ran -- the emitted placement list is bitwise identical.
    //
    // hasHabitat is asked once per bake with the PREDICATE, not by calling
    // habitatAt and catching: a missing tape arms the sticky DSL error, which
    // a JS try/catch cannot see. A world with no habitat() opts out, not in.
    const hasHabitat = this.hasHabitat();

    const scatterCell = (cellTx, cellTz) => {
    const ox = cellTx * SECTOR, oz = cellTz * SECTOR;
    // COLUMN OWNERSHIP (design 3.4). Under the octree a column of world passes
    // through a STACK of tiles, and every one of them would otherwise scatter
    // the same cell -- the same tree planted once per 64 m of altitude. So a
    // tile owns a cell iff the surface at the cell centre falls inside its own
    // vertical span. Exactly one tile per column satisfies that at any level,
    // which makes the rule total and disjoint without any tile knowing what the
    // others decided.
    //
    // Tested at the CELL CENTRE, one sample, not per placement: the cell is the
    // unit scatter has always been planned in (caps, RNG seed and the exclusion
    // pass are all per-cell), so splitting one cell across two owners would
    // change the plan rather than just move it. A cell whose surface straddles a
    // tile boundary therefore goes wholly to the tile holding its centre, and a
    // few of its placements sit outside that tile's span -- which is fine, since
    // a placed child is not clipped to the tile that placed it.
    if (volumetric) {
      const h = this.heightAt(ox + SECTOR / 2, oz + SECTOR / 2);
      if (h < tileOy || h >= tileOy + TILE) return;
    }
    // The one placement primitive every tier below uses. Random scatter
    // (boulders, rocks) and planned forest scatter used to go through two
    // near-identical helpers, put/putPlanned; the only real difference was
    // rotation and Y-scale, both plain arguments here. Placements are
    // TILE-local (the part's own frame) while scatter reasons in world space
    // off the CELL -- hence tileOx/tileOz against wx/wz.
    const place = (module, params, wx, wz, rotation, sx, sy, sz, sinkY) => {
      this.pushMatrix();
      this.translate(wx - tileOx, this.heightAt(wx, wz) - sinkY - tileOy, wz - tileOz);
      this.rotateY(rotation);
      this.scale(sx, sy, sz);
      this.placeChild(module, params);
      this.popMatrix();
    };
    // One world-coordinate plan supplies rock instances and forest exclusion.
    // Include neighboring rock centers whose footprint can reach this cell.
    pbegin(P_BOULDERS);
    const rockDetail=terrainLod<VEGETATION_MIN_LOD?0:1;
    const halo=terrainLod<VEGETATION_MIN_LOD?0:MOUNTAIN_ROCK_HALO;
    const rockOptions={worldSeed:seed,ox:ox-halo,oz:oz-halo,sectorSize:SECTOR+2*halo,
      candidatesInRect,habitatAt:hasHabitat?this.habitatAt.bind(this):undefined,
      biomeAt:this.biomeAt.bind(this),heightAt:this.heightAt.bind(this)};
    const rocks=planMountainRocks({...rockOptions,detail:rockDetail});
    const emitRock=pl=>{
      if(pl.x<ox||pl.x>=ox+SECTOR||pl.z<oz||pl.z>=oz+SECTOR)return;
      this.pushMatrix();
      this.translate(pl.x-tileOx,pl.groundY-pl.sinkY-tileOy,pl.z-tileOz);
      this.rotateY(pl.rotation);this.rotateZ(pl.roll);this.rotateX(pl.pitch);
      this.scale(pl.scale,pl.scale,pl.scale);
      this.placeChild(pl.module,pl.params,{instanced:true,inlineBelowPx:0});this.popMatrix();
    };
    for(const pl of rocks)emitRock(pl);
    pend(P_BOULDERS);
    if(terrainLod<VEGETATION_MIN_LOD)return;
    if(p.rung>=1) {
      pbegin(P_ROCKS);
      // Small scree is a separate stable tier. It never changes forest layout.
      for(const pl of planMountainRocks({...rockOptions,ox,oz,sectorSize:SECTOR,detail:2}))
        if(pl.kind==='scree')emitRock(pl);
      pend(P_ROCKS);
    }

    // ---- PHASE 4: EVERGREEN FOREST + PLACE ---------------------------------------
    // Split PLAN from PLACE. They look like one loop and are not: planning is
    // field sampling and asset selection, while placing is matrix pushes and
    // one placeChild per survivor -- and placeChild's cost is the engine's,
    // not the ecology's. Reading them as a single number is how a scatter
    // investigation ends up optimizing the wrong half.
    {
      pbegin(P_PLAN);
      const planned = planMountainForest({
        worldSeed: seed, ox, oz, sectorSize: SECTOR,
        materials: table.__vegetation?.materials,
        candidatesInRect, biomeAt: this.biomeAt.bind(this),
        habitatAt: hasHabitat ? this.habitatAt.bind(this) : undefined,
      });
      pend(P_PLAN);
      pbegin(P_PLACE);
      for (const pl of planned) {
        if(!mountainRockClearance(rocks,pl.x,pl.z,1.5*pl.scale))continue;
        const heightScale = pl.heightScale === undefined ? 1 : pl.heightScale;
        place(pl.module, pl.params, pl.x, pl.z, pl.rotation,
          pl.scale, pl.scale * heightScale, pl.scale, pl.sinkY);
      }
      pend(P_PLACE);
      return;
    }
    };

    for (let cz = 0; cz < cells; ++cz)
      for (let cx = 0; cx < cells; ++cx)
        scatterCell(baseCx + cx, baseCz + cz);
  }
}
