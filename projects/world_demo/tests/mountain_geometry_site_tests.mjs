import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {mountainGeometrySamples,mountainGeometryCatalog} from '../shared-lib/mountain_geometry_site.js';
const sha256=value=>createHash('sha256').update(JSON.stringify(value)).digest('hex');
const samples=mountainGeometrySamples(7),catalog=mountainGeometryCatalog(7);
const stress=mountainGeometrySamples(7,{stress:true}),stressCatalog=mountainGeometryCatalog(7,{stress:true});
// The shipped scene is the three-rock inspection site; resolution 128 detail
// is retained, only the 1,280 stress-grid additions are opt-in.
assert.deepEqual(samples.map(s=>[s.x,s.z,s.params.seed,s.params.shape,s.params.size,s.params.resolution]),
  [[416,1456,0,0,6,128],[425,1458,1,2,4,128],[422,1448,2,1,7,128]]);
assert.equal(catalog.length,3);
assert.deepEqual(mountainGeometrySamples(7,{stress:false}),samples);
assert.deepEqual(mountainGeometrySamples(7,{}),samples);
// The benchmark profile reproduces the 2026-09-19 stress records exactly.
assert.equal(stress.length,1283);
assert.equal(stressCatalog.length,19);
assert.equal(sha256(stress),'cb4e9f79e99013ea81bab0bbba4ec5c7b0e5e2c7c74170975b0457512e5d48ee',
  'stress profile records match 6c70efe2e mountain_geometry_site.js');
assert.deepEqual(stress.slice(0,3),samples);
assert.deepEqual(catalog,stressCatalog.slice(0,3));
for(const [rows,assets] of [[samples,catalog],[stress,stressCatalog]]){
 assert.deepEqual(rows,mountainGeometrySamples(7,{stress:rows===stress}));
 const keys=new Set(assets.map(x=>JSON.stringify(x.params)));
 for(const s of rows){
  assert.ok(Number.isFinite(s.x)&&Number.isFinite(s.z));
  assert.ok(keys.has(JSON.stringify(s.params)));
  assert.equal(s.params.resolution,128);
  assert.equal(s.params.material,7);
  assert.equal(Math.fround(s.params.size),s.params.size,
    "rock size survives the engine float parameter round trip exactly");
 }
 // Half-open ownership places every rock exactly once at each nested level.
 for(const size of [64,128,256,512]){
  let count=0;
  for(let z=Math.floor(1310/size);z<=Math.floor(1590/size);z++)
  for(let x=Math.floor(290/size);x<=Math.floor(570/size);x++)
  count+=rows.filter(s=>s.x>=x*size&&s.x<(x+1)*size&&s.z>=z*size&&s.z<(z+1)*size).length;
  assert.equal(count,rows.length);
 }
}
console.log('Rock inspection site: 3 placements / 3 dense assets by default; '+
  'stress profile 1283 placements / 19 assets; stable nested ownership');
