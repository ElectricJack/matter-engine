import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
await import('./castle_shared_lib_hooks.mjs');
const {buildMountainRock,mountainRockCatalog}=await import('../shared-lib/mountain_rocks.js');
const {mountainRockReferenceForSize}=await import('../shared-lib/mountain_rock_sizes.js');
const {planMountainRocks,mountainRockClearance}=await import('../shared-lib/mountain_rock_scatter.js');
const {HABITAT}=await import('../shared-lib/alpine_ecology.js');
const {candidatesInRect}=await import('../../../MatterEngine3/shared-lib/scatter_grid.js');
const key=p=>p.map(v=>Math.round(v*1e8)).join(',');
const sub=(a,b)=>a.map((v,i)=>v-b[i]);
const dot=(a,b)=>a.reduce((s,v,i)=>s+v*b[i],0);
const cross=(a,b)=>[a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]];
const catalog=mountainRockCatalog(),signatures=new Set();
const expanded=[0,1,2].flatMap(shape=>[4,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,65536,0xffffffff]
  .map(seed=>({params:{shape,seed,referenceSizeM:1}})));
let triangles=0;
for(const {params} of [...catalog,...expanded]) {
  const rock=buildMountainRock(params),edges=new Map();let volume=0;
  const reference=params.referenceSizeM;
  assert.deepEqual(buildMountainRock(params),rock,'repeatable prototype geometry');
  assert.ok(rock.triangles>=100&&rock.triangles<400,'bounded useful silhouette detail');
  assert.equal(rock.bounds.min[1],0);
  for(const f of rock.faces) for(let i=1;i+1<f.positions.length;i++) {
    const [a,b,c]=[f.positions[0],f.positions[i],f.positions[i+1]],normal=cross(sub(b,a),sub(c,a));
    assert.ok(dot(normal,f.normal)>1e-12,'nondegenerate outward triangle');
    volume+=dot(a,cross(b,c))/6;triangles++;
    for(const p of [a,b,c])assert.ok(Math.hypot(...p)<1.5*reference,'scatter clearance encloses tilted prototype');
    for(const [p,q] of [[a,b],[b,c],[c,a]]) {
      const akey=key(p),bkey=key(q),edgeKey=[akey,bkey].sort().join('/');
      const edge=edges.get(edgeKey)??{count:0,direction:0};
      edge.count++;edge.direction+=akey<bkey?1:-1;edges.set(edgeKey,edge);
    }
  }
  for(const e of edges.values()){assert.equal(e.count,2,'closed collision/render mesh');assert.equal(e.direction,0,'consistent winding');}
  assert.ok(volume/reference**3>.08&&volume/reference**3<2,'solid rock volume');
  const normalized=rock.faces.map(f=>({normal:f.normal,positions:f.positions.map(p=>p.map(v=>v/reference))}));
  const unit=buildMountainRock({...params,referenceSizeM:1});
  assert.deepEqual(normalized,unit.faces.map(f=>({normal:f.normal,positions:f.positions})),
    'size classes preserve the exact unit silhouette');
  signatures.add(JSON.stringify(normalized));
}
assert.equal(catalog.length,48);
assert.equal(signatures.size,12+expanded.length,'expanded seeds produce distinct base silhouettes');
assert.throws(()=>buildMountainRock({shape:3}),/shape/);
assert.throws(()=>buildMountainRock({seed:NaN}),/seed/);
for(const seed of [-1,.5,Infinity,0x100000000,'100'])
  assert.throws(()=>buildMountainRock({seed}),/seed/);
for(const value of [0,-1,NaN,Infinity,.2,65,'2']) {
  assert.throws(()=>buildMountainRock({referenceSizeM:value}),/size/);
  assert.throws(()=>mountainRockReferenceForSize(value),/size/);
}
for(const size of [.25,.35,.5,.99,1,1.001,2,3.99,4,4.001,8,16,16.001,32,36,64]) {
  const reference=mountainRockReferenceForSize(size);
  assert.ok(size/reference>=.5&&size/reference<=2,'residual scale and world microrelief stay bounded');
}
const habitat=(slope=.15,height=70)=>(x,z,out)=>{
  out.length=0;out[HABITAT.altitude]=height;out[HABITAT.slope]=slope;
};
const options={worldSeed:872,ox:-512,oz:-512,sectorSize:1024,detail:2,
  habitatAt:habitat(),heightAt:(x,z)=>70+.05*x-.025*z,biomeAt:()=> 'foothills',candidatesInRect};
const all=planMountainRocks(options),sorted=items=>items.slice().sort((a,b)=>a.x-b.x||a.z-b.z||a.kind.localeCompare(b.kind));
assert.deepEqual(planMountainRocks(options),all);
const signature=createHash('sha256').update(JSON.stringify(all.map(p=>[
  p.kind,p.params.shape,p.params.seed,p.x,p.z,p.rotation,p.scale*p.params.referenceSizeM,
  p.pitch,p.roll,p.groundY,p.sinkY,p.radius]))).digest('hex');
assert.equal(signature,'abcc1d7bcf36063673a3618a78a8a5006ce59344af35b53e5873dcd5ecabf985',
  'size-class conversion preserves the previous world shapes, sizes, placement, burial and clearance');
assert.ok(all.every(p=>p.scale>=.5&&p.scale<=2));
// Independently transform the rock's support-plane normal and compare with
// the analytic terrain plane. A naive pair of slope angles fails when both
// rotated slope components are nonzero.
const gx=.35,gz=-.27;
for(const p of planMountainRocks({...options,heightAt:(x,z)=>70+gx*x+gz*z})) {
  const cr=Math.cos(p.roll),sr=Math.sin(p.roll),cp=Math.cos(p.pitch),sp=Math.sin(p.pitch);
  const cy=Math.cos(p.rotation),sy=Math.sin(p.rotation);
  const normal=[-cy*sr*cp+sy*sp,cr*cp,sy*sr*cp+cy*sp];
  const expected=[-gx,1,-gz],length=Math.hypot(...expected);
  assert.ok(normal.every((v,i)=>Math.abs(v-expected[i]/length)<1e-12),'tilted rock base follows terrain plane');
}
const cells=[];
for(let z=-512;z<512;z+=64)for(let x=-512;x<512;x+=64)
  cells.push(...planMountainRocks({...options,ox:x,oz:z,sectorSize:64}));
assert.deepEqual(sorted(cells),sorted(all),'subdivision preserves every placement and ground pose');
assert.equal(new Set(all.map(p=>`${p.kind}/${p.x}/${p.z}`)).size,all.length,'unique half-open cell ownership');
assert.deepEqual([...new Set(all.map(p=>p.kind))].sort(),['boulder','boulder-field','landmark','scree']);
for(const detail of [0,1]) {
  const expected=all.filter(p=>detail===0?p.kind==='landmark':p.kind!=='scree');
  assert.deepEqual(planMountainRocks({...options,detail}),expected,'distance tiers retain exact poses');
}
for(const habitatAt of [habitat(.6),habitat(.1,-2),habitat(.1,550),habitat(NaN)])
  assert.deepEqual(planMountainRocks({...options,habitatAt}),[]);
assert.deepEqual(planMountainRocks({...options,biomeAt:()=> 'ocean'}),[]);
assert.deepEqual(planMountainRocks({...options,available:()=>false}),[],'infrastructure exclusion is respected');
const restricted=planMountainRocks({...options,available:(x,z,r)=>x-r>10});
assert.ok(restricted.every(p=>p.x-p.radius>10));
assert.deepEqual(restricted,all.filter(p=>p.x-p.radius>10),'exclusion never reshuffles other placements');
const boulder=all.find(p=>p.kind==='boulder-field');
assert.equal(mountainRockClearance(all,boulder.x,boulder.z,1),false,'forest avoids boulder bodies');
assert.ok(all.every(p=>catalog.some(v=>JSON.stringify(v.params)===JSON.stringify(p.params))),'only predeclared shared prototypes');
console.log(`mountain rocks: PASS (${catalog.length+expanded.length} closed prototypes, ${signatures.size} silhouettes, ${triangles} triangles total; ${all.length} unchanged placements over 1 km²)`);
