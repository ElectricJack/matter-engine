import assert from 'node:assert/strict';
await import('./castle_shared_lib_hooks.mjs');
const { planMountainForest, mountainForestCatalog, mountainTreeParams } = await import('../shared-lib/mountain_forest.js');
const { HABITAT } = await import('../shared-lib/alpine_ecology.js');
const { candidatesInRect } = await import('../../../MatterEngine3/shared-lib/scatter_grid.js');
const habitat = (values={}) => (_x,_z,out) => {
  out.length=0;
  for(const [key,value] of Object.entries({altitude:80,slope:.1,moisture:.8,forest:1,forestEdge:1,...values})) out[HABITAT[key]]=value;
};
const options={worldSeed:42,ox:-128,oz:-128,sectorSize:256,candidatesInRect,habitatAt:habitat(),biomeAt:()=> 'forest'};
const forest=planMountainForest(options);
assert.ok(forest.length>100);
assert.deepEqual(planMountainForest(options),forest,'placement is deterministic');
assert.deepEqual([...new Set(forest.map(p=>p.kind))].sort(),[0,1,2,3]);
assert.ok(forest.every(p=>p.module==='MountainEvergreen' && Object.values(p.params).every(Number.isInteger)));
const cells=[];
for(let z=-128;z<128;z+=64) for(let x=-128;x<128;x+=64) cells.push(...planMountainForest({...options,ox:x,oz:z,sectorSize:64}));
const sorted=items=>items.slice().sort((a,b)=>a.x-b.x || a.z-b.z);
assert.deepEqual(sorted(cells),sorted(forest),'tile subdivision preserves placements exactly');
assert.equal(new Set(cells.map(p=>`${p.x},${p.z}`)).size,cells.length,'tile edges never duplicate a tree');
for(const changes of [{altitude:440},{altitude:-1},{slope:.65},{forest:0,forestEdge:0},{forestEdge:NaN}])
  assert.deepEqual(planMountainForest({...options,habitatAt:habitat(changes)}),[]);
assert.deepEqual(planMountainForest({...options,biomeAt:()=> 'ocean'}),[]);
assert.ok(planMountainForest({...options,habitatAt:habitat({altitude:250})}).every(p=>p.kind<2),'redwoods stay in low groves');
assert.equal(mountainForestCatalog().length,4);
assert.equal(mountainTreeParams({...mountainForestCatalog()[2].params}).height,62);
assert.equal(mountainTreeParams({...mountainForestCatalog()[3].params}).stemCount,3);
console.log(`mountain forest: PASS (${forest.length} placements; four evergreen assemblies; stable cell ownership)`);
