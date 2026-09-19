import assert from 'node:assert/strict';
import fs from 'node:fs';
// Execute the scene class with a recording DSL; scatter/catalog entry points
// throw so the isolation contract covers preparation as well as placement.
const source = fs.readFileSync(new URL('../scenes/streaming/StreamMountain/objects/WorldSector.js', import.meta.url), 'utf8')
  .replace(/^import[\s\S]*?;\s*/gm, '');
const unexpected = () => { throw new Error('terrain-only scene attempted scatter/catalog work'); };
class Part {
  terrainVolumeTiled(...args) { this.terrain = args; }
  placeChild() { unexpected(); }
}
const Sector = new Function('Part', 'MAT', 'MOUNTAIN_FOREST_MIN_LOD',
  'mountainRockCatalog', 'mountainForestCatalog', 'mountainGeometrySamples',
  source + '\nreturn WorldSector;')(Part, {grass:1,dirt:2,rock:3,snow:4}, 3,
    unexpected, unexpected, unexpected);
const biomes = JSON.stringify({__terrainOnly:true, __terrain:{material:'dirt'}, __geometryRocks:{}});
assert.deepEqual(Sector.requires({biomes}), []);
for (const lod of [0, 2, 5]) {
  const sector = new Sector();
  sector.build({tx:-2,ty:1,tz:3,terrainLod:lod,biomes});
  assert.deepEqual(sector.terrain, [-2,1,3,lod-5,[2,2,2,2]]);
}
console.log('Terrain-only sectors: terrain retained at each rung, no asset catalogs or scatter');

const worldSource = fs.readFileSync(new URL('../scenes/streaming/StreamMountain/StreamMountain.js', import.meta.url), 'utf8')
  .replace(/^import[\s\S]*?;\s*/gm, '');
const registered = [];
const Mountain = new Function('World','defineMaterial','MOUNTAIN_FOREST_PROFILE',
  worldSource + '\nreturn StreamMountain;')(class {}, (name, spec) => {
    registered.push({name,spec}); return registered.length;
  }, 'fixture');
const world = new Mountain();
assert.equal(world.biomes().__terrainOnly, true);
assert.equal(world.biomes().__vegetation, undefined);
assert.equal(registered.filter(x=>x.spec.detail).length, 0, 'no unused forest texture bakes');
Mountain.params.terrainOnly = false;
assert.ok(world.biomes().__vegetation.materials.barkMaterial);
assert.equal(registered.filter(x=>x.spec.detail).length, 3, 'forest materials available when re-enabled');
