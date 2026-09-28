import assert from 'node:assert/strict';
import {mountainGeometrySamples,mountainGeometryCatalog} from '../shared-lib/mountain_geometry_site.js';
const samples=mountainGeometrySamples(7),catalog=mountainGeometryCatalog(7);
assert.equal(samples.length,1283);
assert.equal(catalog.length,19);
assert.deepEqual(samples,mountainGeometrySamples(7));
const keys=new Set(catalog.map(x=>JSON.stringify(x.params)));
for(const s of samples){
 assert.ok(Number.isFinite(s.x)&&Number.isFinite(s.z));
 assert.ok(keys.has(JSON.stringify(s.params)));
 assert.equal(s.params.resolution,128);
 assert.equal(Math.fround(s.params.size),s.params.size,
   "rock size survives the engine float parameter round trip exactly");
}
// Half-open ownership places every rock exactly once at each nested level.
for(const size of [64,128,256,512]){
 let count=0;
 for(let z=Math.floor(1310/size);z<=Math.floor(1590/size);z++)
 for(let x=Math.floor(290/size);x<=Math.floor(570/size);x++)
 count+=samples.filter(s=>s.x>=x*size&&s.x<(x+1)*size&&s.z>=z*size&&s.z<(z+1)*size).length;
 assert.equal(count,samples.length);
}
console.log('Rock stress site: 1283 deterministic placements, 19 dense assets, stable nested ownership');
