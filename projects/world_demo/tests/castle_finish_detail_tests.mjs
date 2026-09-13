import assert from 'node:assert/strict';
import {castleWoodGrainHeight,castleFloorWearHeight,emitCastleFinishDetail,castleFinishMaterialSpec} from '../shared-lib/castle_finish_detail.js';
const epsilon=1e-5;
const derivative=(f,x,z,a)=>(f(x+(a===0?epsilon:0),z+(a===1?epsilon:0))-f(x-(a===0?epsilon:0),z-(a===1?epsilon:0)))/(2*epsilon);
for(const [kind,f,bound] of [['wood',castleWoodGrainHeight,.00038],['floor',castleFloorWearHeight,.00130]]) {
  for(let i=0;i<83;i++)for(let j=0;j<79;j++) {
    const x=i/83,z=j/79,v=f(x,z);
    assert.ok(Number.isFinite(v)&&Math.abs(v)<=bound+1e-12,'bounded metric relief');
    for(const [dx,dz] of [[1,0],[0,1]]) {
      assert.ok(Math.abs(v-f(x+dx,z+dz))<1e-12,'periodic height');
      for(const a of [0,1])assert.ok(Math.abs(derivative(f,x,z,a)-derivative(f,x+dx,z+dz,a))<1e-8,'periodic same-field normal');
    }
  }
  let tile,base,material;
  emitCastleFinishDetail({tile(v){tile=v;},base(f,m){base=f;material=m;}},kind,71);
  assert.equal(tile.size,1);assert.equal(tile.texelsPerMeter,512);assert.equal(base,f);assert.equal(material,71);
  const original={albedo:[.3,.2,.1],roughness:.8};
  const spec=castleFinishMaterialSpec(kind,original);
  assert.equal(spec.detailMode,'surface');assert.equal(spec.roughness,.8);assert.equal(original.detail,undefined);
}
assert.throws(()=>castleFinishMaterialSpec('invalid',{}));
console.log('castle_finish_detail_tests: PASS (periodic height/gradient; sub-mm wood and bounded floor wear; existing Tileset API)');
await import('./castle_shared_lib_hooks.mjs');
const {castleSiteWorldDefinition}=await import('../shared-lib/castle_site_world.js');
const registry=new Map();
globalThis.defineMaterial=(name,spec)=>{
  if(!registry.has(name))registry.set(name,{id:100+registry.size,spec});
  return registry.get(name).id;
};
const normal=castleSiteWorldDefinition('clustered-court',{surface:true});
assert.ok(!registry.has('CastleUpgraded.floorWear'),'default scene has no new atlas dependency');
const finished=castleSiteWorldDefinition('clustered-court',{surface:true,floorWear:true});
const wear=registry.get('CastleUpgraded.floorWear');
assert.equal(wear.spec.detail,'CastleFloorWearDetail');
assert.ok(finished.roots.some(r=>r.params?.floorMaterial===wear.id));
assert.equal(JSON.stringify(normal.entities),JSON.stringify(finished.entities),'finish changes no colliders/entities');
assert.equal(JSON.stringify(normal.lights),JSON.stringify(finished.lights),'finish changes no lights');
assert.ok(![...registry.values()].some(m=>m.spec.detail==='CastleWoodGrainDetail'),'mixed-axis timber receives no automatic directional grain');
console.log('castle_finish_detail_tests: PASS (floorWear explicit scene opt-in; default/light/collision contracts retained)');
