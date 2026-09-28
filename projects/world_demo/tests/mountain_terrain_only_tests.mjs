import assert from 'node:assert/strict';
import fs from 'node:fs';
// Execute the scene class with a recording DSL; scatter/catalog entry points
// throw so the isolation contract covers preparation as well as placement.
const source = fs.readFileSync(new URL('../scenes/streaming/StreamMountain/objects/WorldSector.js', import.meta.url), 'utf8')
  .replace(/^import[\s\S]*?;\s*/gm, '');
const unexpected = () => { throw new Error('terrain-only scene attempted scatter/catalog work'); };
const placed = [];
class Part {
  terrainVolumeTiled(...args) { this.terrain = args; }
  placeChild(...args) { placed.push(args); }
  heightAt() { return 0; }
  pushMatrix() {}
  translate() {}
  popMatrix() {}
}
const Sector = new Function('Part', 'MAT', 'MOUNTAIN_FOREST_MIN_LOD',
  'mountainRockCatalog', 'mountainForestCatalog', 'mountainGeometrySamples', 'mountainGeometryCatalog',
  source + '\nreturn WorldSector;')(Part, {grass:1,dirt:2,rock:3,snow:4}, 3,
    unexpected, unexpected,
    (material) => [{x: 4, z: 4, params: {size: 1, material}}],
    (material) => [{module: 'MountainDetailRock', params: {size: 1, material}}]);
const terrainOnly = JSON.stringify({__terrainOnly:true, __terrain:{material:'dirt'}});
assert.deepEqual(Sector.requires({biomes: terrainOnly}), []);
// terrainLod -> voxel rung follows WorldSector.js's stress profile
// (max(-5, min(3, terrainLod - 2)), finest spacing 0.25 m).
for (const [lod, rung] of [[0, -2], [2, 0], [5, 3]]) {
  const sector = new Sector();
  sector.build({tx:-2,ty:1,tz:3,terrainLod:lod,biomes: terrainOnly});
  assert.deepEqual(sector.terrain, [-2,1,3,rung,[2,2,2,2]]);
}
assert.equal(placed.length, 0, 'terrain-only without geometry rocks places nothing');
const withRocks = JSON.stringify({__terrainOnly:true, __terrain:{material:'dirt'}, __geometryRocks:{material:'rock'}});
assert.deepEqual(Sector.requires({biomes: withRocks}),
  [{module: 'MountainDetailRock', params: {size: 1, material: 'rock'}}]);
new Sector().build({tx:0,ty:0,tz:0,terrainLod:0,biomes: withRocks});
assert.equal(placed.length, 1, 'terrain-only with geometry rocks places the geometry site samples');
assert.equal(placed[0][0], 'MountainDetailRock');
console.log('Terrain-only sectors: terrain retained at each rung; catalogs and placement only with __geometryRocks');

const worldSource = fs.readFileSync(new URL('../scenes/streaming/StreamMountain/StreamMountain.js', import.meta.url), 'utf8')
  .replace(/^import[\s\S]*?;\s*/gm, '');
const registered = [];
const Mountain = new Function('World','defineMaterial','MOUNTAIN_FOREST_PROFILE',
  worldSource + '\nreturn StreamMountain;')(class {}, (name, spec) => {
    registered.push({name,spec}); return registered.length;
  }, 'fixture');
// Every material handle is declared at module scope, so the set registered is
// fixed at load and toggling terrainOnly can never shift a bake identity.
const registeredAtLoad = registered.length;
assert.equal(registered.filter(x=>x.spec.detail).length, 3, 'forest detail materials register once, at load');
const geometryRock = registered.findIndex(x=>x.name === 'Mountain.GeometryRock') + 1;
assert.ok(geometryRock > 0, 'geometry-rock material is registered');
const world = new Mountain();
assert.equal(world.biomes().__terrainOnly, false, 'terrainOnly defaults off');
assert.ok(world.biomes().__vegetation.materials.barkMaterial);
assert.deepEqual(world.biomes().__geometryRocks, {material: geometryRock});
Mountain.params.terrainOnly = true;
assert.equal(world.biomes().__terrainOnly, true);
assert.equal(world.biomes().__vegetation, undefined);
assert.deepEqual(world.biomes().__geometryRocks, {material: geometryRock},
  'terrain-only keeps the geometry-rock site');
assert.equal(registered.length, registeredAtLoad, 'toggling terrainOnly registers nothing');
console.log('StreamMountain biomes: forest only without terrainOnly; geometry-rock site in both modes');
