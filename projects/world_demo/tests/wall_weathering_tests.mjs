import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
await import('./castle_shared_lib_hooks.mjs');
const {wallWeatherSeed}=await import('../shared-lib/wall_weathering.js');
const {clayBrickMazeWalls}=await import('../shared-lib/clay_brick_maze_layout.js');
const {clayBrickWallPathSurface}=await import('../shared-lib/clay_brick_wall_path.js');
const {clayBrickWallSurface}=await import('../shared-lib/clay_brick_wall_surface.js');
const context=vm.createContext({});
for(const file of ['surface_base.js.inc','finite_surface_base.js.inc']) {
 const text=fs.readFileSync(new URL('../../../MatterEngine3/src/'+file,import.meta.url),'utf8');
 vm.runInContext([...text.matchAll(/R"JS\(([\s\S]*?)\)JS"/g)].map(m=>m[1]).join(''),context);
}
const record=base=>context.__recordSurfaceRecipe(9,base);
const identities=new Set();
for(const w of clayBrickMazeWalls()) {
 const surface=w.params.kind?clayBrickWallPathSurface:clayBrickWallSurface;
 const clean=surface(w.params),p={...w.params,weathering:1,weatherSeed:wallWeatherSeed(w.id),graffiti:1};
 const dirty=surface(p),tape=record(dirty.base);
 assert.equal(tape,record(surface(p).base),'repeatable weathering with stable wall identity');
 assert.equal(JSON.stringify(dirty.placements),JSON.stringify(clean.placements),'weathering preserves brick layout');
 assert.equal(JSON.stringify(dirty.sources),JSON.stringify(clean.sources),'weathering reuses source geometry');
 for(let i=0;i<clean.sources.length;++i)
  assert.equal(record(dirty.sources[i].appearance),record(clean.sources[i].appearance),'source material remains shared');
 assert.match(tape,/^coat /m);
 assert.doesNotMatch(record(clean.base),/^coat /m,'unweathered walls retain their old tape');
 assert.doesNotMatch(tape,/NaN|Infinity/);
 identities.add(tape);
 console.log(`${w.id}: ${tape.trim().split('\n').length} raw tape lines`);
}
assert.equal(identities.size,20,'each wall has a distinct appearance');
for(const invalid of [s=>s.coat({baseColor:[1,0,NaN],roughness:.5},1),
 s=>s.coat({baseColor:[1,0,0],roughness:.5},NaN),
 s=>s.coat({baseColor:[1,0,0],roughness:Infinity},1)])
 assert.throws(()=>record(invalid));
console.log('wall weathering: PASS (20 deterministic variants, unchanged source bank/layout, clean opt-out)');
